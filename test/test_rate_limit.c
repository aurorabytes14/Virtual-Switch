#define _POSIX_C_SOURCE 200809L

#include "frame.h"
#include "frame_io.h"
#include "port_queue.h"
#include "shared_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>

#define TEST_FRAMES 4

static double elapsed_seconds(
    struct timespec start,
    struct timespec end
)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static shared_frame_t *create_test_frame(uint8_t id)
{
    frame_header_t header = {0};

    header.length = 1;
    header.src[0] = 0xAA;
    header.src[5] = id;

    header.dst[0] = 0xBB;
    header.dst[5] = 0x01;

    header.vlan = 1;
    header.priority = PORT_QUEUE_NORMAL;

    uint8_t buffer[FRAME_HEADER_SIZE + 1];

    if (frame_serialize_header(&header, buffer) != 0) {
        perror("frame_serialize_header");
        return NULL;
    }

    buffer[FRAME_HEADER_SIZE] = id;

    return shared_frame_create(
        buffer,
        sizeof(buffer)
    );
}

static int receive_test_frame(int fd)
{
    uint8_t *frame = NULL;
    size_t frame_size = 0;

    int result = frame_recv(fd, &frame, &frame_size);

    if (result != 1) {
        free(frame);
        return -1;
    }

    int id = frame[FRAME_HEADER_SIZE];

    free(frame);

    return id;
}

static int test_token_bucket(void)
{
    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], 8) != 0) {
        perror("port_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    /*
     * Configure:
     * 2 frames per second.
     * Bucket capacity: 2 frames.
     */
    if (port_queue_set_rate_limit(&queue, 2, 2) != 0) {
        perror("port_queue_set_rate_limit");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("\n========== TOKEN BUCKET TEST ==========\n");

    printf("\nRate: 2 frames/second\n");
    printf("Bucket capacity: 2 frames\n");

    /*
     * Enqueue four frames.
     */
    for (uint8_t i = 1; i <= TEST_FRAMES; i++) {
        shared_frame_t *frame = create_test_frame(i);

        if (frame == NULL) {
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

    struct timespec received_at[TEST_FRAMES];

    /*
     * Receive all four frames and record their arrival times.
     */
    for (int i = 0; i < TEST_FRAMES; i++) {
        int id = receive_test_frame(sockets[1]);

        if (id != i + 1) {
            fprintf(
                stderr,
                "ERROR: Expected frame %d, received frame %d\n",
                i + 1,
                id
            );

            port_queue_destroy(&queue);
            close(sockets[0]);
            close(sockets[1]);
            return -1;
        }

        clock_gettime(CLOCK_MONOTONIC, &received_at[i]);

        printf("Received frame %d\n", id);
    }

    /*
     * The first two frames should use the initial tokens.
     */
    double initial_burst = elapsed_seconds(
        received_at[0],
        received_at[1]
    );

    printf("\nInitial burst interval: %.3f seconds\n", initial_burst);

    if (initial_burst > 0.35) {
        fprintf(stderr, "FAIL: Initial burst was unexpectedly slow.\n");

        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("Initial burst test PASSED.\n");

    /*
     * Subsequent frames should wait for token refills.
     */
    double interval_3 = elapsed_seconds(
        received_at[1],
        received_at[2]
    );

    double interval_4 = elapsed_seconds(
        received_at[2],
        received_at[3]
    );

    printf("Frame 3 refill interval: %.3f seconds\n", interval_3);
    printf("Frame 4 refill interval: %.3f seconds\n", interval_4);

    if (interval_3 < 0.35 || interval_4 < 0.35) {
        fprintf(stderr, "FAIL: Frames were sent too quickly.\n");

        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("Token refill rate test PASSED.\n");

    /*
     * Disable rate limiting and verify the setting succeeds.
     */
    if (port_queue_set_rate_limit(&queue, 0, 0) != 0) {
        perror("Disabling rate limiting");

        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("Rate limiting disabled successfully.\n");

    port_queue_destroy(&queue);

    close(sockets[0]);
    close(sockets[1]);

    printf("\nTOKEN BUCKET TEST PASSED.\n");

    return 0;
}

int main(void)
{
    if (test_token_bucket() != 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}