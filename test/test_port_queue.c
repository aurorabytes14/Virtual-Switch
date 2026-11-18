#define _POSIX_C_SOURCE 200809L

#include "port_queue.h"
#include "frame.h"
#include "frame_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_QUEUE_CAPACITY 2
#define TEST_FRAME_COUNT 3

static shared_frame_t *create_test_frame(const char *payload)
{
    size_t payload_length = strlen(payload);
    size_t total_size = FRAME_HEADER_SIZE + payload_length;

    uint8_t *data = calloc(total_size, 1);

    if (data == NULL) {
        return NULL;
    }

    uint16_t length = htons((uint16_t)payload_length);
    memcpy(data, &length, sizeof(length));

    uint16_t vlan = htons(10);
    memcpy(data + 14, &vlan, sizeof(vlan));

    data[16] = 1;

    memcpy(data + FRAME_HEADER_SIZE, payload, payload_length);

    shared_frame_t *frame = shared_frame_create(data, total_size);

    free(data);

    return frame;
}

static int test_delivery_and_order(void)
{
    printf("\n--- DELIVERY AND ORDER TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], 5) != 0) {
        perror("port_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    const char *payloads[] = {"FIRST", "SECOND", "THIRD"};

    for (int i = 0; i < TEST_FRAME_COUNT; i++) {
        shared_frame_t *frame = create_test_frame(payloads[i]);

        if (frame == NULL) {
            perror("create_test_frame");
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        if (port_queue_enqueue(&queue, frame) != 0) {
            perror("port_queue_enqueue");
            shared_frame_release(frame);
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        shared_frame_release(frame);
    }

    for (int i = 0; i < TEST_FRAME_COUNT; i++) {
        uint8_t *received = NULL;
        size_t received_size = 0;

        if (frame_recv(sockets[1], &received, &received_size) != 1) {
            perror("frame_recv");
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        size_t expected_length = strlen(payloads[i]);

        if (received_size != FRAME_HEADER_SIZE + expected_length ||
            memcmp(
                received + FRAME_HEADER_SIZE,
                payloads[i],
                expected_length
            ) != 0) {
            printf("Frame order test FAILED\n");
            free(received);
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        free(received);
    }

    printf("All three frames delivered in order.\n");

    port_queue_destroy(&queue);
    close(sockets[0]);
    close(sockets[1]);

    return 0;
}

static int test_queue_overflow(void)
{
    printf("\n--- QUEUE OVERFLOW TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], TEST_QUEUE_CAPACITY) != 0) {
        perror("port_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    if (port_queue_pause_sender(&queue) != 0) {
        perror("port_queue_pause_sender");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    shared_frame_t *frames[TEST_FRAME_COUNT];

    for (int i = 0; i < TEST_FRAME_COUNT; i++) {
        frames[i] = create_test_frame("QUEUE");

        if (frames[i] == NULL) {
            perror("create_test_frame");

            for (int j = 0; j < i; j++) {
                shared_frame_release(frames[j]);
            }

            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }
    }

    for (int i = 0; i < TEST_QUEUE_CAPACITY; i++) {
        if (port_queue_enqueue(&queue, frames[i]) != 0) {
            printf("Unexpected enqueue failure\n");

            for (int j = 0; j < TEST_FRAME_COUNT; j++) {
                shared_frame_release(frames[j]);
            }

            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }
    }

    errno = 0;

    int result = port_queue_enqueue(
        &queue,
        frames[TEST_QUEUE_CAPACITY]
    );

    if (result != -1 || errno != ENOBUFS) {
        printf("Queue overflow test FAILED\n");

        for (int i = 0; i < TEST_FRAME_COUNT; i++) {
            shared_frame_release(frames[i]);
        }

        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    pthread_mutex_lock(&queue.mutex);
    size_t dropped = queue.dropped_frames;
    size_t queued = queue.count;
    pthread_mutex_unlock(&queue.mutex);

    if (dropped != 1 || queued != TEST_QUEUE_CAPACITY) {
        printf("Queue counters incorrect\n");

        for (int i = 0; i < TEST_FRAME_COUNT; i++) {
            shared_frame_release(frames[i]);
        }

        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("Queue remained bounded at %d frames.\n",
           TEST_QUEUE_CAPACITY);
    printf("Overflow frame dropped successfully.\n");

    for (int i = 0; i < TEST_FRAME_COUNT; i++) {
        shared_frame_release(frames[i]);
    }

    if (port_queue_resume_sender(&queue) != 0) {
        perror("port_queue_resume_sender");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    for (int i = 0; i < TEST_QUEUE_CAPACITY; i++) {
        uint8_t *received = NULL;
        size_t received_size = 0;

        if (frame_recv(sockets[1], &received, &received_size) != 1) {
            perror("frame_recv");
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        free(received);
    }

    port_queue_destroy(&queue);
    close(sockets[0]);
    close(sockets[1]);

    return 0;
}

static int test_shared_reference_count(void)
{
    printf("\n--- SHARED FRAME REFERENCE TEST ---\n");

    int sockets1[2];
    int sockets2[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets1) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sockets2) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue1;
    port_queue_t queue2;

    if (port_queue_init(&queue1, sockets1[0], 5) != 0) {
        perror("queue1 init");
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return -1;
    }

    if (port_queue_init(&queue2, sockets2[0], 5) != 0) {
        perror("queue2 init");
        port_queue_destroy(&queue1);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return -1;
    }

    shared_frame_t *frame = create_test_frame("SHARED");

    if (frame == NULL) {
        perror("create_test_frame");
        port_queue_destroy(&queue1);
        port_queue_destroy(&queue2);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return -1;
    }

    if (port_queue_pause_sender(&queue1) != 0 ||
        port_queue_pause_sender(&queue2) != 0) {
        perror("pause sender");
        shared_frame_release(frame);
        port_queue_destroy(&queue1);
        port_queue_destroy(&queue2);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return -1;
    }

    if (port_queue_enqueue(&queue1, frame) != 0 ||
        port_queue_enqueue(&queue2, frame) != 0) {
        perror("enqueue shared frame");
        shared_frame_release(frame);
        port_queue_destroy(&queue1);
        port_queue_destroy(&queue2);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return -1;
    }

    unsigned int references =
        atomic_load(&frame->references);

    if (references != 3) {
        printf("Expected 3 references, got %u\n", references);
        shared_frame_release(frame);
        port_queue_destroy(&queue1);
        port_queue_destroy(&queue2);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return -1;
    }

    printf("Shared frame reference count: %u\n", references);

    shared_frame_release(frame);

    port_queue_destroy(&queue1);
    port_queue_destroy(&queue2);

    close(sockets1[0]);
    close(sockets1[1]);
    close(sockets2[0]);
    close(sockets2[1]);

    printf("Shared frame released safely.\n");

    return 0;
}

int main(void)
{
    printf("\n========== PORT QUEUE TEST SUITE ==========\n");

    if (test_delivery_and_order() != 0) {
        return 1;
    }

    if (test_queue_overflow() != 0) {
        return 1;
    }

    if (test_shared_reference_count() != 0) {
        return 1;
    }

    printf("\nAll port queue tests passed.\n");

    return 0;
}