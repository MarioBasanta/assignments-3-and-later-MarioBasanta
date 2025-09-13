#include "threading.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

void* threadfunc(void *thread_param)
{
    struct thread_data *data = (struct thread_data *)thread_param;
    if (!data)
        return NULL;

    // Wait before attempting to obtain the mutex
    usleep(data->wait_to_obtain_ms * 1000);

    // Lock the mutex
    if (pthread_mutex_lock(data->mutex) != 0) {
        data->thread_complete_success = false;
        return data;
    }

    // Hold mutex for the specified time
    usleep(data->wait_to_release_ms * 1000);

    // Unlock the mutex
    if (pthread_mutex_unlock(data->mutex) != 0) {
        data->thread_complete_success = false;
        return data;
    }

    data->thread_complete_success = true;
    return data;
}

bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,
                                  int wait_to_obtain_ms, int wait_to_release_ms)
{
    if (!thread || !mutex)
        return false;

    struct thread_data *data = malloc(sizeof(struct thread_data));
    if (!data)
        return false;

    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false;

    int ret = pthread_create(thread, NULL, threadfunc, data);
    if (ret != 0) {
        free(data);
        return false;
    }

    return true;
}


