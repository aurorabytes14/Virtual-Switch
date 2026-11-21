#define _POSIX_C_SOURCE 200809L

#include "port_queue.h"
#include "frame.h"
#include "frame_io.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_CAPACITY 32

static shared_frame_t *create_frame(uint8_t priority, const char *payload)
{
    size_t payload_length = strlen(payload);
    size_t total_size = FRAME_HEADER_SIZE + payload_length;

    uint8_t *data = calloc(total_size, 1);
    if (data == NULL) {
        return NULL;
    }

    frame_header_t header = {
        .length = (uint16_t)payload_length,
        .src = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01},
        .dst = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02},
        .vlan = 10,
        .priority = priority
    };

    if (frame_serialize_header(&header, data) != 0) {
        free(data);
        return NULL;
    }

    memcpy(data + FRAME_HEADER_SIZE, payload, payload_length);

    shared_frame_t *frame = shared_frame_create(data, total_size);
    free(data);

    return frame;
}

static int receive_payload(int fd, char *payload, size_t capacity)
{
    uint8_t *frame = NULL;
    size_t frame_size = 0;

    int result = frame_recv(fd, &frame, &frame_size);
    if (result != 1) {
        free(frame);
        return -1;
    }

    frame_header_t header;

    if (frame_deserialize_header(frame, &header) != 0 ||
        frame_size != (size_t)FRAME_HEADER_SIZE + (size_t)header.length ||
(size_t)header.length + 1U > capacity)
{
        free(frame);
        return -1;
    }

    memcpy(payload, frame + FRAME_HEADER_SIZE, header.length);
    payload[header.length] = '\0';

    free(frame);
    return 0;
}

static int test_weighted_round_robin(void)
{
    printf("\n--- WEIGHTED ROUND ROBIN TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], TEST_CAPACITY) != 0) {
        perror("port_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    if (port_queue_pause_sender(&queue) != 0) {
        perror("pause sender");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    /*
     * Enqueue five high, three normal and one low frame.
     * All frames are queued before the sender resumes.
     */
    const char *expected[] = {
        "H0", "H1", "H2", "H3", "H4",
        "N0", "N1", "N2",
        "L0"
    };

    shared_frame_t *frames[9];

    for (int i = 0; i < 9; i++) {
        uint8_t priority;

        if (i < 5) {
            priority = PORT_QUEUE_HIGH;
        } else if (i < 8) {
            priority = PORT_QUEUE_NORMAL;
        } else {
            priority = PORT_QUEUE_LOW;
        }

        frames[i] = create_frame(priority, expected[i]);

        if (frames[i] == NULL ||
            port_queue_enqueue(&queue, frames[i]) != 0) {
            perror("enqueue");
            for (int j = 0; j <= i; j++) {
                if (frames[j] != NULL) {
                    shared_frame_release(frames[j]);
                }
            }
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        shared_frame_release(frames[i]);
    }

    if (port_queue_resume_sender(&queue) != 0) {
        perror("resume sender");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    for (int i = 0; i < 9; i++) {
        char received[32];

        if (receive_payload(sockets[1], received, sizeof(received)) != 0) {
            printf("Could not receive frame %d\n", i);
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        if (strcmp(received, expected[i]) != 0) {
            printf(
                "Scheduling mismatch at position %d: expected %s, got %s\n",
                i, expected[i], received
            );

            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }
    }

    printf("Weighted 5:3:1 scheduling order verified.\n");

    port_queue_destroy(&queue);
    close(sockets[0]);
    close(sockets[1]);

    return 0;
}

static int test_priority_preference(void)
{
    printf("\n--- PRIORITY PREFERENCE TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], TEST_CAPACITY) != 0) {
        perror("port_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    port_queue_pause_sender(&queue);

    shared_frame_t *low = create_frame(PORT_QUEUE_LOW, "LOW");
    shared_frame_t *normal = create_frame(PORT_QUEUE_NORMAL, "NORMAL");
    shared_frame_t *high = create_frame(PORT_QUEUE_HIGH, "HIGH");

    if (low == NULL || normal == NULL || high == NULL) {
        printf("Frame allocation failed.\n");
        shared_frame_release(low);
        shared_frame_release(normal);
        shared_frame_release(high);
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    port_queue_enqueue(&queue, low);
    port_queue_enqueue(&queue, normal);
    port_queue_enqueue(&queue, high);

    shared_frame_release(low);
    shared_frame_release(normal);
    shared_frame_release(high);

    port_queue_resume_sender(&queue);

    const char *expected[] = {"HIGH", "NORMAL", "LOW"};

    for (int i = 0; i < 3; i++) {
        char received[32];

        if (receive_payload(sockets[1], received, sizeof(received)) != 0 ||
            strcmp(received, expected[i]) != 0) {
            printf("Priority preference test FAILED.\n");
            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }
    }

    printf("High-priority frame sent first.\n");

    port_queue_destroy(&queue);
    close(sockets[0]);
    close(sockets[1]);

    return 0;
}

static int test_aging(void)
{
    printf("\n--- PRIORITY AGING TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], TEST_CAPACITY) != 0) {
        perror("port_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    port_queue_pause_sender(&queue);

    if (port_queue_set_aging_ms(&queue, 100) != 0) {
        perror("set aging threshold");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    shared_frame_t *low = create_frame(PORT_QUEUE_LOW, "AGED_LOW");

    if (low == NULL || port_queue_enqueue(&queue, low) != 0) {
        perror("enqueue low frame");
        shared_frame_release(low);
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    shared_frame_release(low);

    /* Keep the sender paused until the low-priority frame ages. */
    struct timespec delay = {
    .tv_sec = 0,
    .tv_nsec = 200000000L
};

nanosleep(&delay, NULL);

    shared_frame_t *high = create_frame(PORT_QUEUE_HIGH, "NEW_HIGH");

    if (high == NULL || port_queue_enqueue(&queue, high) != 0) {
        perror("enqueue high frame");
        shared_frame_release(high);
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    shared_frame_release(high);

    port_queue_resume_sender(&queue);

    char received[32];

    if (receive_payload(sockets[1], received, sizeof(received)) != 0 ||
        strcmp(received, "AGED_LOW") != 0) {
        printf("Aging test FAILED: aged low-priority frame was not selected first.\n");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    if (receive_payload(sockets[1], received, sizeof(received)) != 0 ||
        strcmp(received, "NEW_HIGH") != 0) {
        printf("Aging test FAILED: high-priority frame missing.\n");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("Aged low-priority frame received scheduling preference.\n");

    port_queue_destroy(&queue);
    close(sockets[0]);
    close(sockets[1]);

    return 0;
}

int main(void)
{
    printf("\n========== QoS TEST SUITE ==========\n");

    if (test_weighted_round_robin() != 0) {
        return EXIT_FAILURE;
    }

    if (test_priority_preference() != 0) {
        return EXIT_FAILURE;
    }

    if (test_aging() != 0) {
        return EXIT_FAILURE;
    }

    printf("\nAll QoS tests passed.\n");

    return EXIT_SUCCESS;
}