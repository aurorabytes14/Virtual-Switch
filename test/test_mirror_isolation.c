#include "frame.h"
#include "frame_io.h"
#include "mirror_queue.h"
#include "port_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_CAPACITY 2

static shared_frame_t *create_test_frame(const char *payload)
{
    size_t payload_length = strlen(payload);
    size_t total_size = FRAME_HEADER_SIZE + payload_length;

    uint8_t *data = calloc(total_size, 1);

    if (data == NULL) {
        return NULL;
    }

    frame_header_t header = {0};

    header.length = (uint16_t)payload_length;
    header.src[5] = 1;
    header.dst[5] = 2;
    header.vlan = 10;
    header.priority = 1;

    frame_serialize_header(&header, data);

    memcpy(data + FRAME_HEADER_SIZE, payload, payload_length);

    shared_frame_t *frame = shared_frame_create(data, total_size);

    free(data);

    return frame;
}

static int receive_and_verify(int fd, const char *expected)
{
    uint8_t *received = NULL;
    size_t received_size = 0;

    if (frame_recv(fd, &received, &received_size) != 1) {
        fprintf(stderr, "Frame receive failed\n");
        free(received);
        return -1;
    }

    size_t expected_length = strlen(expected);

    if (received_size != FRAME_HEADER_SIZE + expected_length ||
        memcmp(received + FRAME_HEADER_SIZE,
               expected, expected_length) != 0) {
        fprintf(stderr, "Unexpected frame received\n");
        free(received);
        return -1;
    }

    free(received);
    return 0;
}

int main(void)
{
    int mirror_sockets[2];
    int port_sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, mirror_sockets) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, port_sockets) != 0) {
        perror("socketpair");
        return EXIT_FAILURE;
    }

    mirror_queue_t mirror_queue;
    port_queue_t port_queue;

    if (mirror_queue_init(
            &mirror_queue,
            mirror_sockets[0],
            TEST_CAPACITY) != 0) {
        perror("mirror_queue_init");
        return EXIT_FAILURE;
    }

    if (port_queue_init(
            &port_queue,
            port_sockets[0],
            TEST_CAPACITY) != 0) {
        fprintf(stderr, "port_queue_init failed\n");
        mirror_queue_destroy(&mirror_queue);
        close(mirror_sockets[0]);
        close(mirror_sockets[1]);
        close(port_sockets[0]);
        close(port_sockets[1]);
        return EXIT_FAILURE;
    }

    printf("\n========== SLOW SNIFFER ISOLATION TEST ==========\n");

    /* Pause the mirror sender to simulate a slow sniffer. */
    mirror_queue_pause(&mirror_queue);

    printf("Mirror sender paused.\n");

    shared_frame_t *frame1 = create_test_frame("MIRROR_ONE");
    shared_frame_t *frame2 = create_test_frame("MIRROR_TWO");
    shared_frame_t *normal_frame = create_test_frame("NORMAL_PORT");

    if (frame1 == NULL || frame2 == NULL || normal_frame == NULL) {
        fprintf(stderr, "Frame allocation failed\n");

        shared_frame_release(frame1);
        shared_frame_release(frame2);
        shared_frame_release(normal_frame);

        mirror_queue_destroy(&mirror_queue);
        port_queue_destroy(&port_queue);

        close(mirror_sockets[0]);
        close(mirror_sockets[1]);
        close(port_sockets[0]);
        close(port_sockets[1]);

        return EXIT_FAILURE;
    }

    /* Fill the mirror queue while its sender is paused. */
    if (mirror_queue_enqueue(&mirror_queue, frame1) != 0 ||
        mirror_queue_enqueue(&mirror_queue, frame2) != 0) {
        fprintf(stderr, "Could not fill mirror queue\n");
        return EXIT_FAILURE;
    }

    printf("Mirror queue filled with two frames.\n");

    /* Normal forwarding must still work independently. */
    if (port_queue_enqueue(&port_queue, normal_frame) != 0) {
        fprintf(stderr, "Normal port enqueue failed\n");
        return EXIT_FAILURE;
    }

    shared_frame_release(frame1);
    shared_frame_release(frame2);
    shared_frame_release(normal_frame);

    if (receive_and_verify(port_sockets[1], "NORMAL_PORT") != 0) {
        fprintf(stderr, "Normal port delivery failed\n");
        return EXIT_FAILURE;
    }

    printf("Normal port delivered its frame while sniffer was paused.\n");

    /* Resume the mirror sender and verify queued mirror copies. */
    mirror_queue_resume(&mirror_queue);

    if (receive_and_verify(mirror_sockets[1], "MIRROR_ONE") != 0 ||
        receive_and_verify(mirror_sockets[1], "MIRROR_TWO") != 0) {
        fprintf(stderr, "Mirror frame delivery failed\n");
        return EXIT_FAILURE;
    }

    printf("Both mirrored frames delivered after resuming.\n");

    mirror_queue_destroy(&mirror_queue);
    port_queue_destroy(&port_queue);

    close(mirror_sockets[0]);
    close(mirror_sockets[1]);
    close(port_sockets[0]);
    close(port_sockets[1]);

    printf("\nSlow sniffer isolation test PASSED.\n");

    return EXIT_SUCCESS;
}