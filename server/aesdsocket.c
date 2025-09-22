#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PROG_NAME "aesdsocket"

FILE *log_fp;
pthread_mutex_t mutex;

void global_clean() {
    if (log_fp) fclose(log_fp);
    pthread_mutex_destroy(&mutex);
}

void global_setup() {
    log_fp = fopen("/var/tmp/aesdsocketdata", "a+");
    if (!log_fp) {
        perror("fopen failed");
        exit(1);
    }
    if (pthread_mutex_init(&mutex, NULL)) {
        perror("pthread_mutex_init failed");
        global_clean();
        exit(1);
    }
}

typedef struct {
    int sock;
    struct sockaddr_in info;
} server_t;

void server_close(server_t *s) {
    if (s->sock > 0) {
        shutdown(s->sock, SHUT_RDWR);
        close(s->sock);
    }
}

void server_setup(server_t *s) {
    int enable = 1;
    s->info.sin_family = AF_INET;
    s->info.sin_addr.s_addr = htonl(INADDR_ANY); // fixed from htons to htonl
    s->info.sin_port = htons(9000);
    s->sock = socket(PF_INET, SOCK_STREAM, 0);
    if (s->sock < 0) {
        perror("socket failed");
        goto err;
    }
    if (setsockopt(s->sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable))) {
        perror("setsockopt failed");
        goto err;
    }

    int i;
    for (i = 1; i <= 1024; i++) {
        if (bind(s->sock, (struct sockaddr *)&s->info, sizeof(s->info)) == 0)
            break;
        usleep(i * 100 * 1000); // sleep i*100ms
    }
    if (i > 1024) {
        perror("bind failed");
        goto err;
    }
    if (listen(s->sock, 20)) {
        perror("listen failed");
        goto err;
    }
    return;
err:
    server_close(s);
    exit(1);
}

typedef struct {
    int sock;
    struct sockaddr_in addr;
    char ipaddr[INET_ADDRSTRLEN];
    socklen_t addrlen;
} client_t;

void client_setup(client_t *c) {
    c->addrlen = sizeof(c->addr);
    memset(c->ipaddr, 0, INET_ADDRSTRLEN);
}

// Mutex-guarded client logic similar to your original,
// reading all data until newline and storing it atomically
void client_logic(client_t *c) {
    char buf[1024];
    ssize_t recvd;
    size_t packet_size = 0;
    char *packet_buffer = NULL;
    bool packet_complete = false;

    if (!inet_ntop(AF_INET, &c->addr.sin_addr, c->ipaddr, INET_ADDRSTRLEN)) {
        perror("inet_ntop failed");
        goto cleanup;
    }

    if (pthread_mutex_lock(&mutex)) {
        perror("pthread_mutex_lock failed");
        goto cleanup;
    }
    syslog(LOG_INFO, "Accepted connection from %s", c->ipaddr);

    while (!packet_complete && (recvd = recv(c->sock, buf, sizeof(buf), 0)) > 0) {
        char *new_buf = realloc(packet_buffer, packet_size + recvd);
        if (!new_buf) {
            perror("realloc failed");
            break;
        }
        packet_buffer = new_buf;
        memcpy(packet_buffer + packet_size, buf, recvd);
        packet_size += recvd;

        for (size_t i = 0; i < packet_size; ++i) {
            if (packet_buffer[i] == '\n') {
                // Write up to and including newline atomically
                size_t write_len = i + 1;
                if (fwrite(packet_buffer, 1, write_len, log_fp) != write_len) {
                    perror("fwrite failed");
                    goto unlock_and_cleanup;
                }
                // Flush to ensure it is written to disk
                fflush(log_fp);

                // Send file back line-by-line
                if (fseek(log_fp, 0, SEEK_SET) != 0) {
                    perror("fseek failed");
                    goto unlock_and_cleanup;
                }

                char linebuf[1024];
                while (fgets(linebuf, sizeof(linebuf), log_fp) != NULL) {
                    ssize_t sent = send(c->sock, linebuf, strlen(linebuf), 0);
                    if (sent < 0) {
                        perror("send failed");
                        goto unlock_and_cleanup;
                    }
                }

                // Shift remaining buffer content
                size_t rem = packet_size - write_len;
                if (rem > 0)
                    memmove(packet_buffer, packet_buffer + write_len, rem);
                packet_size = rem;

                packet_complete = true;
                break;
            }
        }
    }

    // Write any remaining incomplete packet on client close
    if (packet_size > 0 && packet_buffer) {
        if (fwrite(packet_buffer, 1, packet_size, log_fp) != packet_size) {
            perror("fwrite incomplete packet failed");
        }
        fflush(log_fp);

        if (fseek(log_fp, 0, SEEK_SET) == 0) {
            char linebuf[1024];
            while (fgets(linebuf, sizeof(linebuf), log_fp) != NULL) {
                ssize_t sent = send(c->sock, linebuf, strlen(linebuf), 0);
                if (sent < 0) {
                    perror("send failed on incomplete packet");
                    break;
                }
            }
        }
    }
unlock_and_cleanup:
    pthread_mutex_unlock(&mutex);

cleanup:
    free(packet_buffer);
    shutdown(c->sock, SHUT_RDWR);
    close(c->sock);
    syslog(LOG_INFO, "Closed connection from %s", c->ipaddr);
}

// Signal handling for graceful termination
static volatile sig_atomic_t program_active = 1;

static void sig_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM)
        program_active = 0;
}

void signal_setup(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;

    if (sigaction(SIGINT, &sa, NULL))
        perror("sigaction SIGINT failed");

    if (sigaction(SIGTERM, &sa, NULL))
        perror("sigaction SIGTERM failed");
}

typedef struct task_t_ {
    pthread_t tid;
    struct task_t_ *next;
    client_t clnt;
} task_t;

static task_t *head = NULL;

// Thread function executing client logic
static void *thread_run(void *arg) {
    client_t *client = (client_t *)arg;
    client_logic(client);
    pthread_exit(NULL);
}

static void thread_clean() {
    task_t *tmp;
    while (head) {
        pthread_join(head->tid, NULL);
        tmp = head;
        head = head->next;
        free(tmp);
    }
}

// Timestamp thread writes timestamp every 10 seconds with mutex lock
static void *timer_log(void *arg) {
    (void)arg;
    while (program_active) {
        sleep(10);
        if (pthread_mutex_lock(&mutex)) {
            perror("pthread_mutex_lock error");
            continue;
        }

        if (fseek(log_fp, 0, SEEK_END)) {
            perror("fseek END failed");
            pthread_mutex_unlock(&mutex);
            continue;
        }

        time_t now = time(NULL);
        struct tm tm_now;
        char timebuf[128];
        if (!localtime_r(&now, &tm_now)) {
            perror("localtime_r failed");
            pthread_mutex_unlock(&mutex);
            continue;
        }

        if (!strftime(timebuf, sizeof(timebuf), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", &tm_now)) {
            perror("strftime failed");
            pthread_mutex_unlock(&mutex);
            continue;
        }

        if (fwrite(timebuf, 1, strlen(timebuf), log_fp) != (ssize_t)strlen(timebuf)) {
            perror("fwrite timestamp failed");
        }
        fflush(log_fp);
        pthread_mutex_unlock(&mutex);
    }
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    server_t serv;
    struct sigaction actn;
    pthread_t log_tid;

    if (argc > 1) {
        if (strcmp(argv[1], "-d") == 0) {
            if (daemon(0, 0)) {
                perror("daemon failed");
                exit(1);
            }
        } else {
            fprintf(stderr, "Option %s not supported\n", argv[1]);
            exit(1);
        }
    }

    openlog(PROG_NAME, LOG_PERROR | LOG_PID, LOG_USER);
    global_setup();
    server_setup(&serv);
    signal_setup();

    if (pthread_create(&log_tid, NULL, timer_log, NULL)) {
        perror("pthread_create LOG failed");
        goto out;
    }

    while (program_active) {
        task_t *new_task = malloc(sizeof(task_t));
        if (!new_task) {
            perror("malloc failed");
            continue;
        }
        memset(new_task, 0, sizeof(task_t));
        client_setup(&new_task->clnt);

        new_task->clnt.sock = accept(serv.sock, (struct sockaddr *)&new_task->clnt.addr, &new_task->clnt.addrlen);
        if (new_task->clnt.sock < 0) {
            free(new_task);
            if (errno == EINTR) continue;
            perror("accept failed");
            break;
        }

        // Add new client thread to linked list
        new_task->next = head;
        head = new_task;

        if (pthread_create(&new_task->tid, NULL, thread_run, &new_task->clnt)) {
            perror("pthread_create CLNT failed");
            server_close(&serv);
            break;
        }
    }

out:
    // Stop server
    program_active = 0;
    close(serv.sock);

    pthread_join(log_tid, NULL);

    // Join and clean client threads
    thread_clean();

    global_clean();
    server_close(&serv);
    closelog();

    return 0;
}
