#ifndef MIRROR_QUEUE_H
#define MIRROR_QUEUE_H

#include "shared_frame.h"

#include <stddef.h>
#include <pthread.h>

typedef struct {
    shared_frame_t **items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    size_t dropped_frames;
    size_t sent_frames;
    size_t failed_frames;

    int socket_fd;
    int stopping;
    int started;
    int paused;


    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_t sender_thread;
} mirror_queue_t;

int mirror_queue_init(
    mirror_queue_t *queue,
    int socket_fd,
    size_t capacity
);

int mirror_queue_enqueue(
    mirror_queue_t *queue,
    shared_frame_t *frame
);

void mirror_queue_destroy(mirror_queue_t *queue);
void mirror_queue_pause(mirror_queue_t *queue);
void mirror_queue_resume(mirror_queue_t *queue);

#endif
