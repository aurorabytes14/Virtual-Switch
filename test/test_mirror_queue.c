#include "mirror_queue.h"
#include "frame.h"
#include "frame_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define TEST_CAPACITY 2

static void create_test_frame(
    uint8_t *buffer,
    size_t *size,
    const char *message
)
{
    frame_header_t header = {0};

    header.length = (uint16_t)strlen(message);
    header.src[5] = 1;
    header.dst[5] = 2;
    header.vlan = 10;
    header.priority = 1;

    frame_serialize_header(&header, buffer);
    memcpy(buffer + FRAME_HEADER_SIZE, message, header.length);

    *size = FRAME_HEADER_SIZE + header.length;
}

int main(void)
{
    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return 1;
    }

    mirror_queue_t queue;

    if (mirror_queue_init(&queue, sockets[0], TEST_CAPACITY) != 0) {
        perror("mirror_queue_init");
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    /* Pause sender to prevent it from removing queued frames. */
    mirror_queue_pause(&queue);

    uint8_t data[FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD];
    size_t size;

    create_test_frame(data, &size, "Mirror test one");
    shared_frame_t *frame1 = shared_frame_create(data, size);

    create_test_frame(data, &size, "Mirror test two");
    shared_frame_t *frame2 = shared_frame_create(data, size);

    create_test_frame(data, &size, "Mirror test three");
    shared_frame_t *frame3 = shared_frame_create(data, size);

    if (!frame1 || !frame2 || !frame3) {
        fprintf(stderr, "Frame allocation failed\n");
        return 1;
    }

    if (mirror_queue_enqueue(&queue, frame1) != 0) {
        fprintf(stderr, "First enqueue failed\n");
        return 1;
    }

    if (mirror_queue_enqueue(&queue, frame2) != 0) {
        fprintf(stderr, "Second enqueue failed\n");
        return 1;
    }

    printf("First two frames enqueued successfully.\n");

    errno = 0;

    if (mirror_queue_enqueue(&queue, frame3) != -1 ||
        errno != ENOBUFS) {
        fprintf(stderr, "Expected third frame to be dropped.\n");
        return 1;
    }

    printf("Third frame correctly dropped: queue is full.\n");

    pthread_mutex_lock(&queue.mutex);
    size_t dropped = queue.dropped_frames;
    size_t queued = queue.count;
    pthread_mutex_unlock(&queue.mutex);

    if (dropped != 1 || queued != TEST_CAPACITY) {
        fprintf(stderr, "Queue overflow counters are incorrect.\n");
        return 1;
    }

    printf("Dropped frames: %zu\n", dropped);

    shared_frame_release(frame1);
    shared_frame_release(frame2);
    shared_frame_release(frame3);

    /* Resume sender so it can deliver the two accepted frames. */
    mirror_queue_resume(&queue);

    for (int i = 0; i < TEST_CAPACITY; i++) {
        uint8_t *received = NULL;
        size_t received_size = 0;

        if (frame_recv(sockets[1], &received, &received_size) != 1) {
            fprintf(stderr, "Frame receive failed.\n");
            return 1;
        }

        frame_header_t header;

        if (frame_deserialize_header(received, &header) != 0) {
            fprintf(stderr, "Invalid received frame.\n");
            free(received);
            return 1;
        }

        printf(
            "Received mirrored frame %d, VLAN %u\n",
            i + 1,
            header.vlan
        );

        free(received);
    }

    mirror_queue_destroy(&queue);

    close(sockets[0]);
    close(sockets[1]);

    printf("Mirror queue overflow test passed.\n");

    return 0;
}