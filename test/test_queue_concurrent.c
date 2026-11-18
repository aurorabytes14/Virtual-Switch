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

#define PRODUCER_COUNT 4
#define FRAMES_PER_PRODUCER 100
#define TOTAL_FRAMES (PRODUCER_COUNT * FRAMES_PER_PRODUCER)
#define QUEUE_CAPACITY 512

typedef struct {
    port_queue_t *queue;
    int producer_id;
    int failed;
} producer_args_t;

typedef struct {
    int socket_fd;
    int seen[TOTAL_FRAMES];
    int received;
    int failed;
} receiver_args_t;

static shared_frame_t *create_frame(uint32_t id)
{
    uint8_t data[FRAME_HEADER_SIZE + 4] = {0};

    uint16_t length = htons(4);
    uint16_t vlan = htons(10);
    uint32_t network_id = htonl(id);

    memcpy(data, &length, sizeof(length));
    memcpy(data + 14, &vlan, sizeof(vlan));

    data[16] = 1;

    memcpy(data + FRAME_HEADER_SIZE, &network_id, sizeof(network_id));

    return shared_frame_create(data, sizeof(data));
}

static void *producer_worker(void *argument)
{
    producer_args_t *args = argument;

    int start = args->producer_id * FRAMES_PER_PRODUCER;
    int end = start + FRAMES_PER_PRODUCER;

    for (int id = start; id < end; id++) {
        shared_frame_t *frame = create_frame((uint32_t)id);

        if (frame == NULL) {
            args->failed = 1;
            return NULL;
        }

        if (port_queue_enqueue(args->queue, frame) != 0) {
            fprintf(stderr, "Failed to enqueue frame %d\n", id);
            shared_frame_release(frame);
            args->failed = 1;
            return NULL;
        }

        shared_frame_release(frame);
    }

    return NULL;
}

static void *receiver_worker(void *argument)
{
    receiver_args_t *args = argument;

    while (1) {
        uint8_t *frame = NULL;
        size_t frame_size = 0;

        int result = frame_recv(args->socket_fd, &frame, &frame_size);

        if (result == 0) {
            break;
        }

        if (result < 0) {
            perror("frame_recv");
            args->failed = 1;
            break;
        }

        if (frame_size != FRAME_HEADER_SIZE + 4) {
            fprintf(stderr, "Invalid frame size: %zu\n", frame_size);
            args->failed = 1;
            free(frame);
            break;
        }

        uint32_t network_id;
        memcpy(&network_id, frame + FRAME_HEADER_SIZE, sizeof(network_id));

        uint32_t id = ntohl(network_id);

        if (id >= TOTAL_FRAMES) {
            fprintf(stderr, "Invalid frame ID: %u\n", id);
            args->failed = 1;
            free(frame);
            break;
        }

        if (args->seen[id]) {
            fprintf(stderr, "Duplicate frame ID: %u\n", id);
            args->failed = 1;
            free(frame);
            break;
        }

        args->seen[id] = 1;
        args->received++;

        free(frame);
    }

    return NULL;
}

int main(void)
{
    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return EXIT_FAILURE;
    }

    port_queue_t queue;

    if (port_queue_init(&queue, sockets[0], QUEUE_CAPACITY) != 0) {
        fprintf(stderr, "Queue initialization failed\n");
        close(sockets[0]);
        close(sockets[1]);
        return EXIT_FAILURE;
    }

    receiver_args_t receiver_args = {
        .socket_fd = sockets[1],
        .received = 0,
        .failed = 0
    };

    memset(receiver_args.seen, 0, sizeof(receiver_args.seen));

    pthread_t receiver_thread;

    if (pthread_create(&receiver_thread, NULL,
                       receiver_worker, &receiver_args) != 0) {
        fprintf(stderr, "Could not create receiver thread\n");
        port_queue_destroy(&queue);
        close(sockets[0]);
        close(sockets[1]);
        return EXIT_FAILURE;
    }

    pthread_t producers[PRODUCER_COUNT];
    producer_args_t producer_args[PRODUCER_COUNT];

    int created = 0;
    int producer_failed = 0;

    for (int i = 0; i < PRODUCER_COUNT; i++) {
        producer_args[i].queue = &queue;
        producer_args[i].producer_id = i;
        producer_args[i].failed = 0;

        if (pthread_create(&producers[i], NULL,
                           producer_worker, &producer_args[i]) != 0) {
            fprintf(stderr, "Could not create producer %d\n", i);
            producer_failed = 1;
            break;
        }

        created++;
    }

    for (int i = 0; i < created; i++) {
        pthread_join(producers[i], NULL);

        if (producer_args[i].failed) {
            producer_failed = 1;
        }
    }

    /* Wait for the sender to transmit all queued frames. */
    port_queue_destroy(&queue);

    /* Signal the receiver that no more frames will arrive. */
    close(sockets[0]);

    pthread_join(receiver_thread, NULL);
    close(sockets[1]);

    int missing = 0;

    for (int i = 0; i < TOTAL_FRAMES; i++) {
        if (!receiver_args.seen[i]) {
            missing++;
        }
    }

    printf("\n========== CONCURRENT PRODUCER TEST ==========\n");
    printf("Producers: %d\n", PRODUCER_COUNT);
    printf("Frames per producer: %d\n", FRAMES_PER_PRODUCER);
    printf("Expected frames: %d\n", TOTAL_FRAMES);
    printf("Received frames: %d\n", receiver_args.received);
    printf("Missing frames: %d\n", missing);
    printf("Sender completed frames: %zu\n", queue.sent_frames);

    if (producer_failed ||
        receiver_args.failed ||
        receiver_args.received != TOTAL_FRAMES ||
        missing != 0 ||
        queue.sent_frames != TOTAL_FRAMES) {
        printf("\nConcurrent producer test FAILED.\n");
        return EXIT_FAILURE;
    }

    printf("\nAll 400 frames delivered exactly once.\n");
    printf("Concurrent producer test PASSED.\n");

    return EXIT_SUCCESS;
}