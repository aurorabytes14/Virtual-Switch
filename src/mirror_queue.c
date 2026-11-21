#include "mirror_queue.h"
#include "frame_io.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

static void *mirror_sender_worker(void *argument)
{
    mirror_queue_t *queue = argument;

    while (1) {
        pthread_mutex_lock(&queue->mutex);

        while ((queue->count == 0 || queue->paused) &&
               !queue->stopping) {
            pthread_cond_wait(
                &queue->not_empty,
                &queue->mutex
            );
        }

        if (queue->stopping) {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }

        shared_frame_t *frame = queue->items[queue->head];

        queue->items[queue->head] = NULL;
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;

        pthread_mutex_unlock(&queue->mutex);

        int result = frame_send(
            queue->socket_fd,
            frame->data,
            frame->size
        );

        pthread_mutex_lock(&queue->mutex);

        if (result == 0) {
            queue->sent_frames++;
        } else {
            queue->failed_frames++;
        }

        pthread_mutex_unlock(&queue->mutex);

        shared_frame_release(frame);
    }

    return NULL;
}

int mirror_queue_init(
    mirror_queue_t *queue,
    int socket_fd,
    size_t capacity
)
{
    if (queue == NULL || socket_fd < 0 || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    queue->items = calloc(capacity, sizeof(*queue->items));

    if (queue->items == NULL) {
        return -1;
    }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    queue->dropped_frames = 0;
    queue->sent_frames = 0;
    queue->failed_frames = 0;

    queue->socket_fd = socket_fd;
    queue->stopping = 0;
    queue->started = 0;
    queue->paused = 0;

    int result = pthread_mutex_init(&queue->mutex, NULL);

    if (result != 0) {
        errno = result;
        free(queue->items);
        queue->items = NULL;
        return -1;
    }

    result = pthread_cond_init(&queue->not_empty, NULL);

    if (result != 0) {
        errno = result;
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        queue->items = NULL;
        return -1;
    }

    result = pthread_create(
        &queue->sender_thread,
        NULL,
        mirror_sender_worker,
        queue
    );

    if (result != 0) {
        errno = result;
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        queue->items = NULL;
        return -1;
    }

    queue->started = 1;

    return 0;
}

int mirror_queue_enqueue(
    mirror_queue_t *queue,
    shared_frame_t *frame
)
{
    if (queue == NULL || frame == NULL || !queue->started) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    if (queue->stopping) {
        pthread_mutex_unlock(&queue->mutex);
        errno = ESHUTDOWN;
        return -1;
    }

    if (queue->count == queue->capacity) {
        queue->dropped_frames++;

        pthread_mutex_unlock(&queue->mutex);

        errno = ENOBUFS;
        return -1;
    }

    shared_frame_retain(frame);

    queue->items[queue->tail] = frame;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

void mirror_queue_pause(mirror_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->paused = 1;

    pthread_mutex_unlock(&queue->mutex);
}

void mirror_queue_resume(mirror_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->paused = 0;

    pthread_cond_broadcast(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);
}

void mirror_queue_destroy(mirror_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->stopping = 1;

    pthread_cond_broadcast(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    shutdown(queue->socket_fd, SHUT_RDWR);

    pthread_join(queue->sender_thread, NULL);

    pthread_mutex_lock(&queue->mutex);

    while (queue->count > 0) {
        shared_frame_t *frame = queue->items[queue->head];

        queue->items[queue->head] = NULL;
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;

        shared_frame_release(frame);
    }

    pthread_mutex_unlock(&queue->mutex);

    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);

    free(queue->items);

    queue->items = NULL;
    queue->started = 0;
}