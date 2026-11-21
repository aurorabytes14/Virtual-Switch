#ifndef PORT_QUEUE_H
#define PORT_QUEUE_H

#include "shared_frame.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define PORT_QUEUE_HIGH 0
#define PORT_QUEUE_NORMAL 1
#define PORT_QUEUE_LOW 2

#define PORT_QUEUE_PRIORITY_COUNT 3
#define PORT_QUEUE_DEFAULT_AGING_MS 1000

typedef struct {
    shared_frame_t *frame;
    struct timespec enqueued_at;
} queue_item_t;

typedef struct {
    queue_item_t *items;

    size_t head;
    size_t tail;
    size_t count;
} priority_queue_t;

typedef struct {
    priority_queue_t priorities[PORT_QUEUE_PRIORITY_COUNT];

    size_t capacity;
    size_t count;

    int socket_fd;
    int stopping;
    int sender_paused;
    int started;

    size_t dropped_frames;
    size_t sent_frames;
    size_t failed_frames;

    unsigned int aging_ms;
    unsigned int schedule_position;

    /* Token bucket rate limiting. */
    unsigned int token_rate;
    unsigned int token_capacity;
    unsigned int tokens;

    int rate_limit_enabled;
    int timer_started;

    pthread_t timer_thread;

    pthread_cond_t token_available;
    pthread_cond_t timer_cond;

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

int port_queue_set_aging_ms(
    port_queue_t *queue,
    unsigned int aging_ms
);

/* Configure the token bucket. */
int port_queue_set_rate_limit(
    port_queue_t *queue,
    unsigned int frames_per_second,
    unsigned int bucket_capacity
);

void port_queue_destroy(port_queue_t *queue);

#endif