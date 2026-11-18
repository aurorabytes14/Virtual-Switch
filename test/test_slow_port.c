#define _POSIX_C_SOURCE 200809L

#include "frame.h"
#include "frame_io.h"
#include "port_queue.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static shared_frame_t *create_test_frame(const char *payload)
{
    size_t payload_length = strlen(payload);
    size_t total_size = FRAME_HEADER_SIZE + payload_length;

    uint8_t *data = calloc(total_size, 1);

    if (data == NULL) {
        return NULL;
    }

    uint16_t length = htons((uint16_t)payload_length);
    uint16_t vlan = htons(10);

    memcpy(data, &length, sizeof(length));
    memcpy(data + 14, &vlan, sizeof(vlan));

    data[16] = 1;

    memcpy(data + FRAME_HEADER_SIZE, payload, payload_length);

    shared_frame_t *frame = shared_frame_create(data, total_size);

    free(data);

    return frame;
}

static int receive_and_verify(int fd, const char *expected)
{
    uint8_t *received = NULL;
    size_t received_size = 0;

    int result = frame_recv(fd, &received, &received_size);

    if (result != 1) {
        fprintf(stderr, "Failed to receive frame\n");
        free(received);
        return -1;
    }

    size_t expected_length = strlen(expected);

    if (received_size != FRAME_HEADER_SIZE + expected_length ||
        memcmp(received + FRAME_HEADER_SIZE,
               expected, expected_length) != 0) {
        fprintf(stderr, "Received unexpected frame\n");
        free(received);
        return -1;
    }

    free(received);
    return 0;
}

int main(void)
{
    int sockets1[2];
    int sockets2[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets1) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sockets2) != 0) {
        perror("socketpair");
        return EXIT_FAILURE;
    }

    port_queue_t port1;
    port_queue_t port2;

    if (port_queue_init(&port1, sockets1[0], 2) != 0) {
        fprintf(stderr, "Port 1 initialization failed\n");
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return EXIT_FAILURE;
    }

    if (port_queue_init(&port2, sockets2[0], 2) != 0) {
        fprintf(stderr, "Port 2 initialization failed\n");
        port_queue_destroy(&port1);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return EXIT_FAILURE;
    }

    printf("\n========== SLOW PORT ISOLATION TEST ==========\n");

    /* Pause Port 1's sender to simulate a slow host. */
    if (port_queue_pause_sender(&port1) != 0) {
        fprintf(stderr, "Could not pause Port 1\n");
        port_queue_destroy(&port1);
        port_queue_destroy(&port2);
        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);
        return EXIT_FAILURE;
    }

    printf("Port 1 sender paused.\n");

    shared_frame_t *slow_frame1 = create_test_frame("SLOW_ONE");
    shared_frame_t *slow_frame2 = create_test_frame("SLOW_TWO");
    shared_frame_t *fast_frame = create_test_frame("FAST_PORT");

    if (slow_frame1 == NULL ||
        slow_frame2 == NULL ||
        fast_frame == NULL) {
        fprintf(stderr, "Frame creation failed\n");

        shared_frame_release(slow_frame1);
        shared_frame_release(slow_frame2);
        shared_frame_release(fast_frame);

        port_queue_destroy(&port1);
        port_queue_destroy(&port2);

        close(sockets1[0]);
        close(sockets1[1]);
        close(sockets2[0]);
        close(sockets2[1]);

        return EXIT_FAILURE;
    }

    /* Fill Port 1's queue. */
    if (port_queue_enqueue(&port1, slow_frame1) != 0 ||
        port_queue_enqueue(&port1, slow_frame2) != 0) {
        fprintf(stderr, "Could not enqueue Port 1 frames\n");
        goto cleanup_failure;
    }

    printf("Port 1 queue filled with two frames.\n");

    /*
     * Port 2 must still deliver its frame even though
     * Port 1's sender is paused.
     */
    if (port_queue_enqueue(&port2, fast_frame) != 0) {
        fprintf(stderr, "Could not enqueue Port 2 frame\n");
        goto cleanup_failure;
    }

    shared_frame_release(slow_frame1);
    shared_frame_release(slow_frame2);
    shared_frame_release(fast_frame);

    slow_frame1 = NULL;
    slow_frame2 = NULL;
    fast_frame = NULL;

    if (receive_and_verify(sockets2[1], "FAST_PORT") != 0) {
        fprintf(stderr, "Port 2 delivery failed\n");
        goto cleanup;
    }

    printf("Port 2 delivered its frame while Port 1 was paused.\n");

    /* Resume Port 1 and verify its queued frames. */
    if (port_queue_resume_sender(&port1) != 0) {
        fprintf(stderr, "Could not resume Port 1\n");
        goto cleanup;
    }

    if (receive_and_verify(sockets1[1], "SLOW_ONE") != 0 ||
        receive_and_verify(sockets1[1], "SLOW_TWO") != 0) {
        fprintf(stderr, "Port 1 delivery failed\n");
        goto cleanup;
    }

    printf("Port 1 delivered both queued frames after resuming.\n");

    port_queue_destroy(&port1);
    port_queue_destroy(&port2);

    close(sockets1[0]);
    close(sockets1[1]);
    close(sockets2[0]);
    close(sockets2[1]);

    printf("\nSlow port isolation test PASSED.\n");

    return EXIT_SUCCESS;

cleanup_failure:
    shared_frame_release(slow_frame1);
    shared_frame_release(slow_frame2);
    shared_frame_release(fast_frame);

cleanup:
    port_queue_destroy(&port1);
    port_queue_destroy(&port2);

    close(sockets1[0]);
    close(sockets1[1]);
    close(sockets2[0]);
    close(sockets2[1]);

    return EXIT_FAILURE;
}