#include "port_queue.h"
#include "frame_io.h"

#include <stdlib.h>
#include <errno.h>

static void *sender_worker(void *argument)
{
    port_queue_t *queue = argument;

    while (1) {
        pthread_mutex_lock(&queue->mutex);

        while (
            (queue->count == 0 || queue->sender_paused) &&
            !queue->stopping
        ) {
            pthread_cond_wait(
                &queue->not_empty,
                &queue->mutex
            );
        }

        if (queue->count == 0 && queue->stopping) {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }

        shared_frame_t *frame = queue->items[queue->head];

        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;

        pthread_mutex_unlock(&queue->mutex);

        if (frame_send(
                queue->socket_fd,
                frame->data,
                frame->size
            ) == 0) {

            pthread_mutex_lock(&queue->mutex);
            queue->sent_frames++;
            pthread_mutex_unlock(&queue->mutex);
        }

        shared_frame_release(frame);
    }

    return NULL;
}

int port_queue_init(
    port_queue_t *queue,
    int socket_fd,
    size_t capacity
)
{
    if (queue == NULL || socket_fd < 0 || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    queue->items = calloc(
        capacity,
        sizeof(*queue->items)
    );

    if (queue->items == NULL) {
        return -1;
    }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    queue->socket_fd = socket_fd;
    queue->stopping = 0;
    queue->sender_paused = 0;
    queue->started = 0;

    queue->dropped_frames = 0;
    queue->sent_frames = 0;

    int result = pthread_mutex_init(
        &queue->mutex,
        NULL
    );

    if (result != 0) {
        free(queue->items);
        errno = result;
        return -1;
    }

    result = pthread_cond_init(
        &queue->not_empty,
        NULL
    );

    if (result != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        errno = result;
        return -1;
    }

    result = pthread_create(
        &queue->sender_thread,
        NULL,
        sender_worker,
        queue
    );

    if (result != 0) {
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        errno = result;
        return -1;
    }

    queue->started = 1;

    return 0;
}

int port_queue_enqueue(
    port_queue_t *queue,
    shared_frame_t *frame
)
{
    if (queue == NULL ||
        frame == NULL ||
        !queue->started) {

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

    queue->tail =
        (queue->tail + 1) % queue->capacity;

    queue->count++;

    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

int port_queue_pause_sender(port_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    if (queue->stopping) {
        pthread_mutex_unlock(&queue->mutex);
        errno = ESHUTDOWN;
        return -1;
    }

    queue->sender_paused = 1;

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

int port_queue_resume_sender(port_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->sender_paused = 0;

    pthread_cond_broadcast(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

void port_queue_destroy(port_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->stopping = 1;

    pthread_cond_broadcast(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    pthread_join(
        queue->sender_thread,
        NULL
    );

    pthread_mutex_lock(&queue->mutex);

    while (queue->count > 0) {
        shared_frame_t *frame =
            queue->items[queue->head];

        queue->head =
            (queue->head + 1) % queue->capacity;

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