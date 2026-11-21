#define _POSIX_C_SOURCE 200809L

#include "port_queue.h"
#include "frame.h"
#include "frame_io.h"

#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#define SCHEDULE_LENGTH 9
#define NANOSECONDS_PER_SECOND 1000000000ULL

/*
 * Weighted Round Robin:
 * 5 high, 3 normal, 1 low.
 */
static const int priority_schedule[SCHEDULE_LENGTH] = {
    PORT_QUEUE_HIGH,
    PORT_QUEUE_HIGH,
    PORT_QUEUE_HIGH,
    PORT_QUEUE_HIGH,
    PORT_QUEUE_HIGH,
    PORT_QUEUE_NORMAL,
    PORT_QUEUE_NORMAL,
    PORT_QUEUE_NORMAL,
    PORT_QUEUE_LOW
};

/* Calculate elapsed milliseconds. */
static long elapsed_ms(
    const struct timespec *start,
    const struct timespec *end
)
{
    long seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;

    return seconds * 1000L + nanoseconds / 1000000L;
}

/* Check whether a priority queue is empty. */
static int priority_queue_empty(
    const priority_queue_t *priority_queue
)
{
    return priority_queue->count == 0;
}

/* Insert a frame into a priority queue. */
static void priority_queue_push(
    priority_queue_t *priority_queue,
    shared_frame_t *frame,
    const struct timespec *timestamp,
    size_t capacity
)
{
    priority_queue->items[priority_queue->tail].frame = frame;
    priority_queue->items[priority_queue->tail].enqueued_at =
        *timestamp;

    priority_queue->tail =
        (priority_queue->tail + 1) % capacity;

    priority_queue->count++;
}

/* Remove the oldest frame from a priority queue. */
static shared_frame_t *priority_queue_pop(
    priority_queue_t *priority_queue,
    size_t capacity
)
{
    if (priority_queue->count == 0) {
        return NULL;
    }

    shared_frame_t *frame =
        priority_queue->items[priority_queue->head].frame;

    priority_queue->head =
        (priority_queue->head + 1) % capacity;

    priority_queue->count--;

    return frame;
}

/* Find the oldest eligible aged frame. */
static int find_aged_priority(
    port_queue_t *queue,
    const struct timespec *now
)
{
    int selected = -1;
    long longest_wait = -1;

    for (
        int priority = PORT_QUEUE_NORMAL;
        priority <= PORT_QUEUE_LOW;
        priority++
    ) {
        priority_queue_t *priority_queue =
            &queue->priorities[priority];

        if (priority_queue_empty(priority_queue)) {
            continue;
        }

        struct timespec *enqueued_at =
            &priority_queue->items[
                priority_queue->head
            ].enqueued_at;

        long waiting_time = elapsed_ms(enqueued_at, now);

        if (
            waiting_time >= (long)queue->aging_ms &&
            waiting_time > longest_wait
        ) {
            longest_wait = waiting_time;
            selected = priority;
        }
    }

    return selected;
}

/* Select the next priority using aging and weighted RR. */
static int select_priority(port_queue_t *queue)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }

    int aged_priority = find_aged_priority(queue, &now);

    if (aged_priority != -1) {
        return aged_priority;
    }

    for (int i = 0; i < SCHEDULE_LENGTH; i++) {
        unsigned int position =
            (queue->schedule_position + (unsigned int)i)
            % SCHEDULE_LENGTH;

        int priority = priority_schedule[position];

        if (!priority_queue_empty(&queue->priorities[priority])) {
            queue->schedule_position =
                (position + 1) % SCHEDULE_LENGTH;

            return priority;
        }
    }

    return -1;
}

/* Add nanoseconds to a timestamp. */
static struct timespec add_nanoseconds(
    struct timespec timestamp,
    uint64_t nanoseconds
)
{
    timestamp.tv_sec +=
        (time_t)(nanoseconds / NANOSECONDS_PER_SECOND);

    timestamp.tv_nsec +=
        (long)(nanoseconds % NANOSECONDS_PER_SECOND);

    if (timestamp.tv_nsec >= 1000000000L) {
        timestamp.tv_sec++;
        timestamp.tv_nsec -= 1000000000L;
    }

    return timestamp;
}

/*
 * Timer thread.
 *
 * Replenishes one token at the configured rate.
 * Uses a monotonic-clock timed condition wait.
 */
static void *timer_worker(void *argument)
{
    port_queue_t *queue = argument;

    pthread_mutex_lock(&queue->mutex);

    while (!queue->stopping) {

        while (
            !queue->rate_limit_enabled &&
            !queue->stopping
        ) {
            pthread_cond_wait(
                &queue->timer_cond,
                &queue->mutex
            );
        }

        if (queue->stopping) {
            break;
        }

        unsigned int rate = queue->token_rate;

        if (rate == 0) {
            continue;
        }

        uint64_t interval_ns =
            NANOSECONDS_PER_SECOND / rate;

        if (interval_ns == 0) {
            interval_ns = 1;
        }

        struct timespec deadline;

        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            pthread_cond_wait(
                &queue->timer_cond,
                &queue->mutex
            );

            continue;
        }

        deadline = add_nanoseconds(deadline, interval_ns);

        int result = pthread_cond_timedwait(
            &queue->timer_cond,
            &queue->mutex,
            &deadline
        );

        if (queue->stopping) {
            break;
        }

        if (
            result == ETIMEDOUT &&
            queue->rate_limit_enabled
        ) {
            if (queue->tokens < queue->token_capacity) {
                queue->tokens++;

                pthread_cond_broadcast(
                    &queue->token_available
                );
            }
        }
    }

    pthread_mutex_unlock(&queue->mutex);

    return NULL;
}

/*
 * Dedicated sender thread for one port.
 *
 * A frame is selected only after a token is available.
 */
static void *sender_worker(void *argument)
{
    port_queue_t *queue = argument;

    while (1) {
        pthread_mutex_lock(&queue->mutex);

        /* Wait for frames and an active sender. */
        while (
            (queue->count == 0 || queue->sender_paused) &&
            !queue->stopping
        ) {
            pthread_cond_wait(
                &queue->not_empty,
                &queue->mutex
            );
        }

        /* Exit when shutdown is requested and queue is empty. */
        if (queue->count == 0 && queue->stopping) {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }

        /*
         * Wait for a token.
         * During shutdown, allow remaining frames to drain.
         */
        while (
    queue->rate_limit_enabled &&
    queue->tokens == 0 &&
    !queue->stopping &&
    !queue->sender_paused
) {
    pthread_cond_wait(
        &queue->token_available,
        &queue->mutex
    );
}

/* If paused while waiting for a token, wait again. */
if (queue->sender_paused && !queue->stopping) {
    pthread_mutex_unlock(&queue->mutex);
    continue;
}

        /* Select using the existing QoS scheduler. */
        int priority = select_priority(queue);

        if (priority == -1) {
            pthread_mutex_unlock(&queue->mutex);
            continue;
        }

        shared_frame_t *frame = priority_queue_pop(
            &queue->priorities[priority],
            queue->capacity
        );

        if (frame == NULL) {
            pthread_mutex_unlock(&queue->mutex);
            continue;
        }

        queue->count--;

        /* Consume one token for this frame. */
        if (
            queue->rate_limit_enabled &&
            !queue->stopping
        ) {
            queue->tokens--;
        }

        pthread_mutex_unlock(&queue->mutex);

        int send_result = frame_send(
            queue->socket_fd,
            frame->data,
            frame->size
        );

        pthread_mutex_lock(&queue->mutex);

        if (send_result == 0) {
            queue->sent_frames++;
        } else {
            queue->failed_frames++;
        }

        pthread_mutex_unlock(&queue->mutex);

        shared_frame_release(frame);
    }

    return NULL;
}

/*
 * Initialize priority queues, synchronization objects,
 * timer thread and sender thread.
 */
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

    queue->capacity = capacity;
    queue->count = 0;

    queue->socket_fd = socket_fd;
    queue->stopping = 0;
    queue->sender_paused = 0;
    queue->started = 0;

    queue->dropped_frames = 0;
    queue->sent_frames = 0;
    queue->failed_frames = 0;

    queue->aging_ms = PORT_QUEUE_DEFAULT_AGING_MS;
    queue->schedule_position = 0;

    /* Rate limiting disabled by default. */
    queue->token_rate = 0;
    queue->token_capacity = 0;
    queue->tokens = 0;
    queue->rate_limit_enabled = 0;
    queue->timer_started = 0;

    for (int i = 0; i < PORT_QUEUE_PRIORITY_COUNT; i++) {
        queue->priorities[i].items =
            calloc(capacity, sizeof(queue_item_t));

        if (queue->priorities[i].items == NULL) {
            for (int j = 0; j < i; j++) {
                free(queue->priorities[j].items);
                queue->priorities[j].items = NULL;
            }

            return -1;
        }

        queue->priorities[i].head = 0;
        queue->priorities[i].tail = 0;
        queue->priorities[i].count = 0;
    }

    int result = pthread_mutex_init(&queue->mutex, NULL);

    if (result != 0) {
        goto cleanup_arrays;
    }

    result = pthread_cond_init(&queue->not_empty, NULL);

    if (result != 0) {
        goto cleanup_mutex;
    }

    result = pthread_cond_init(&queue->token_available, NULL);

    if (result != 0) {
        goto cleanup_not_empty;
    }

    pthread_condattr_t attributes;

    result = pthread_condattr_init(&attributes);

    if (result != 0) {
        goto cleanup_token_available;
    }

    result = pthread_condattr_setclock(
        &attributes,
        CLOCK_MONOTONIC
    );

    if (result != 0) {
        pthread_condattr_destroy(&attributes);
        goto cleanup_token_available;
    }

    result = pthread_cond_init(
        &queue->timer_cond,
        &attributes
    );

    pthread_condattr_destroy(&attributes);

    if (result != 0) {
        goto cleanup_token_available;
    }

    /* Start timer thread. */
    result = pthread_create(
        &queue->timer_thread,
        NULL,
        timer_worker,
        queue
    );

    if (result != 0) {
        goto cleanup_timer_cond;
    }

    queue->timer_started = 1;

    /* Start sender thread. */
    result = pthread_create(
        &queue->sender_thread,
        NULL,
        sender_worker,
        queue
    );

    if (result != 0) {
        pthread_mutex_lock(&queue->mutex);

        queue->stopping = 1;

        pthread_cond_broadcast(&queue->timer_cond);

        pthread_mutex_unlock(&queue->mutex);

        pthread_join(queue->timer_thread, NULL);

        queue->timer_started = 0;

        goto cleanup_timer_cond;
    }

    queue->started = 1;

    return 0;

cleanup_timer_cond:
    pthread_cond_destroy(&queue->timer_cond);

cleanup_token_available:
    pthread_cond_destroy(&queue->token_available);

cleanup_not_empty:
    pthread_cond_destroy(&queue->not_empty);

cleanup_mutex:
    pthread_mutex_destroy(&queue->mutex);

cleanup_arrays:
    for (int i = 0; i < PORT_QUEUE_PRIORITY_COUNT; i++) {
        free(queue->priorities[i].items);
        queue->priorities[i].items = NULL;
    }

    errno = result;

    return -1;
}

/* Enqueue a frame according to its priority field. */
int port_queue_enqueue(
    port_queue_t *queue,
    shared_frame_t *frame
)
{
    if (queue == NULL || frame == NULL || !queue->started) {
        errno = EINVAL;
        return -1;
    }

    if (frame->size < FRAME_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }

    frame_header_t header;

    if (frame_deserialize_header(frame->data, &header) != 0) {
        errno = EINVAL;
        return -1;
    }

    int priority = header.priority;

    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
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

    priority_queue_push(
        &queue->priorities[priority],
        frame,
        &timestamp,
        queue->capacity
    );

    queue->count++;

    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

/* Pause the sender thread. */
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

/* Wake sender if it is waiting for a token. */
    pthread_cond_broadcast(&queue->token_available);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

/* Resume the sender thread. */
int port_queue_resume_sender(port_queue_t *queue)
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

    queue->sender_paused = 0;

    pthread_cond_broadcast(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

/* Change priority aging threshold. */
int port_queue_set_aging_ms(
    port_queue_t *queue,
    unsigned int aging_ms
)
{
    if (queue == NULL || !queue->started || aging_ms == 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    if (queue->stopping) {
        pthread_mutex_unlock(&queue->mutex);
        errno = ESHUTDOWN;
        return -1;
    }

    queue->aging_ms = aging_ms;

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

/*
 * Configure token bucket rate limiting.
 *
 * frames_per_second = 0 disables rate limiting.
 */
int port_queue_set_rate_limit(
    port_queue_t *queue,
    unsigned int frames_per_second,
    unsigned int bucket_capacity
)
{
    if (queue == NULL || !queue->started) {
        errno = EINVAL;
        return -1;
    }

    if (frames_per_second > 0 && bucket_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);

    if (queue->stopping) {
        pthread_mutex_unlock(&queue->mutex);
        errno = ESHUTDOWN;
        return -1;
    }

    queue->token_rate = frames_per_second;
    queue->token_capacity = bucket_capacity;

    if (frames_per_second == 0) {
        queue->rate_limit_enabled = 0;
        queue->tokens = 0;
    } else {
        queue->rate_limit_enabled = 1;

        /* Start with a full bucket. */
        queue->tokens = bucket_capacity;
    }

    /* Notify timer about configuration changes. */
    pthread_cond_broadcast(&queue->timer_cond);

    /* Wake sender if tokens are available or limit is disabled. */
    pthread_cond_broadcast(&queue->token_available);
    pthread_cond_broadcast(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

/*
 * Stop both threads, release remaining frames,
 * and destroy synchronization resources.
 */
void port_queue_destroy(port_queue_t *queue)
{
    if (queue == NULL || !queue->started) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);

    queue->stopping = 1;
    queue->sender_paused = 0;

    /* Wake sender and timer threads. */
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->token_available);
    pthread_cond_broadcast(&queue->timer_cond);

    pthread_mutex_unlock(&queue->mutex);

    /* Wait for sender to finish draining the queue. */
    pthread_join(queue->sender_thread, NULL);

    /* Wait for timer thread to exit. */
    if (queue->timer_started) {
        pthread_join(queue->timer_thread, NULL);
        queue->timer_started = 0;
    }

    pthread_mutex_lock(&queue->mutex);

    /* Release any frames remaining in the queues. */
    for (
        int priority = 0;
        priority < PORT_QUEUE_PRIORITY_COUNT;
        priority++
    ) {
        priority_queue_t *priority_queue =
            &queue->priorities[priority];

        while (priority_queue->count > 0) {
            shared_frame_t *frame = priority_queue_pop(
                priority_queue,
                queue->capacity
            );

            shared_frame_release(frame);
        }
    }

    queue->count = 0;

    pthread_mutex_unlock(&queue->mutex);

    pthread_cond_destroy(&queue->timer_cond);
    pthread_cond_destroy(&queue->token_available);
    pthread_cond_destroy(&queue->not_empty);

    pthread_mutex_destroy(&queue->mutex);

    for (int i = 0; i < PORT_QUEUE_PRIORITY_COUNT; i++) {
        free(queue->priorities[i].items);
        queue->priorities[i].items = NULL;
    }

    queue->started = 0;
}