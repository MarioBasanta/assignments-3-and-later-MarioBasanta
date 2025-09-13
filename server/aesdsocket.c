#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define BUFFER_SIZE 1024

static volatile sig_atomic_t stop_requested = 0;

static void signal_handler(int signo) {
    (void)signo;  // avoid unused parameter warning
    syslog(LOG_INFO, "Caught signal, exiting");
    stop_requested = 1;
}

static void setup_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int setup_socket(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, BACKLOG) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static void log_client_address(struct sockaddr_in *client_addr, const char* msg) {
    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), ipstr, sizeof(ipstr));
    syslog(LOG_INFO, "%s from %s", msg, ipstr);
}

static int append_packet_to_file(const char *packet, size_t len) {
    FILE *fp = fopen(DATA_FILE, "a");
    if (!fp) {
        syslog(LOG_ERR, "Failed to open data file: %s", strerror(errno));
        return -1;
    }
    size_t written = fwrite(packet, 1, len, fp);
    if (written != len) {
        syslog(LOG_ERR, "Failed to write full packet to data file");
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int send_file_content_line_by_line(int sockfd) {
    FILE *fp = fopen(DATA_FILE, "r");
    if (!fp) return 0;

    char *line = NULL;
    size_t len = 0;
    ssize_t read_len;
    int ret = 0;

    while ((read_len = getline(&line, &len, fp)) != -1) {
        ssize_t sent = send(sockfd, line, read_len, 0);
        if (sent < 0) {
            syslog(LOG_ERR, "Failed to send data");
            ret = -1;
            break;
        }
    }

    free(line);
    fclose(fp);
    return ret;
}

static void daemonize(void) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // Parent exits immediately
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid failed");
        exit(EXIT_FAILURE);
    }

    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    syslog(LOG_INFO, "Daemon started with PID %d", getpid());
}

int main(int argc, char *argv[]) {
    openlog("aesdsocket", LOG_PID | LOG_PERROR, LOG_USER);
    setup_signal_handlers();

    bool run_as_daemon = (argc == 2 && strcmp(argv[1], "-d") == 0);

    int sockfd = setup_socket();
    if (sockfd < 0) {
        syslog(LOG_ERR, "Failed to set up socket");
        closelog();
        return EXIT_FAILURE;
    }

    if (run_as_daemon) daemonize();

    while (!stop_requested) {
        struct sockaddr_in client_addr = {0};
        socklen_t addrlen = sizeof(client_addr);

        int client_fd = accept(sockfd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        log_client_address(&client_addr, "Accepted connection");

        char recvbuf[BUFFER_SIZE];
        char *packet_buffer = NULL;
        size_t packet_size = 0;
        bool packet_complete = false;

        while (!packet_complete && !stop_requested) {
            ssize_t received = recv(client_fd, recvbuf, sizeof(recvbuf), 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }
            if (received == 0) break; // Client closed connection

            char *new_buffer = realloc(packet_buffer, packet_size + (size_t)received);
            if (!new_buffer) {
                syslog(LOG_ERR, "Memory allocation failed");
                free(packet_buffer);
                packet_buffer = NULL;
                break;
            }
            packet_buffer = new_buffer;
            memcpy(packet_buffer + packet_size, recvbuf, (size_t)received);
            packet_size += (size_t)received;

            for (size_t i = 0; i < packet_size; i++) {
                if (packet_buffer[i] == '\n') {
                    size_t packet_len = i + 1;

                    if (append_packet_to_file(packet_buffer, packet_len) < 0)
                        syslog(LOG_ERR, "Failed to append packet to file");

                    if (send_file_content_line_by_line(client_fd) < 0)
                        syslog(LOG_ERR, "Failed to send file content");

                    size_t remaining_len = packet_size - packet_len;
                    if (remaining_len > 0)
                        memmove(packet_buffer, packet_buffer + packet_len, remaining_len);
                    packet_size = remaining_len;

                    packet_complete = true;
                    break;
                }
            }
        }

        // Flush any remaining data buffered that did not end with a newline on client disconnect
        if (packet_size > 0 && packet_buffer != NULL) {
            syslog(LOG_INFO, "Appending incomplete packet on client close");
            if (append_packet_to_file(packet_buffer, packet_size) < 0) {
                syslog(LOG_ERR, "Failed to append incomplete packet");
            }
            if (send_file_content_line_by_line(client_fd) < 0) {
                syslog(LOG_ERR, "Failed to send file content on incomplete packet");
            }
        }

        free(packet_buffer);
        log_client_address(&client_addr, "Closed connection");
        close(client_fd);
    }

    close(sockfd);
    unlink(DATA_FILE);
    closelog();
    return EXIT_SUCCESS;
}
