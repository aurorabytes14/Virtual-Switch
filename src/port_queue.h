#ifndef PORT_QUEUE_H
#define PORT_QUEUE_H

#include "shared_frame.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
    shared_frame_t **items;

    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    int socket_fd;
    int stopping;
    int sender_paused;
    int started;

   size_t dropped_frames;
   size_t sent_frames;
   size_t failed_frames;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_t sender_thread;
} port_queue_t;

int port_queue_init(
    port_queue_t *queue,
    int socket_fd,
    size_t capacity
);

int port_queue_enqueue(
    port_queue_t *queue,
    shared_frame_t *frame
);

int port_queue_pause_sender(port_queue_t *queue);

int port_queue_resume_sender(port_queue_t *queue);

void port_queue_destroy(port_queue_t *queue);

#endif