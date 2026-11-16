#define _POSIX_C_SOURCE 200809L

#include "frame_io.h"
#include "frame.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>



static int test_disconnect_handling(void)
{
    int sockets[2];
    uint8_t *received = NULL;
    size_t received_size = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    close(sockets[0]);

    int result = frame_recv(sockets[1], &received, &received_size);

    if (result != 0) {
        printf("Clean disconnect test FAILED\n");
        close(sockets[1]);
        return -1;
    }

    printf("Clean disconnect test PASSED\n");

    close(sockets[1]);
    return 0;
}

static int test_invalid_length(void)
{
    int sockets[2];
    uint8_t header[FRAME_HEADER_SIZE] = {0};
    uint8_t *received = NULL;
    size_t received_size = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    uint16_t invalid_length = htons(FRAME_MAX_PAYLOAD + 1);
    memcpy(header, &invalid_length, sizeof(invalid_length));

    if (write(sockets[0], header, sizeof(header)) != sizeof(header)) {
        perror("write");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    int result = frame_recv(sockets[1], &received, &received_size);

    if (result != -1) {
        printf("Invalid length test FAILED\n");
        free(received);
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    printf("Invalid length test PASSED\n");

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}


int main(void)
{
    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return 1;
    }

    printf("\n--- FRAME I/O TEST ---\n");

    uint8_t original[FRAME_HEADER_SIZE + 5] = {0};

    uint16_t payload_length = htons(5);

    memcpy(original, &payload_length, sizeof(payload_length));

    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

    uint8_t dst[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};

    memcpy(original + 2, src, 6);
    memcpy(original + 8, dst, 6);

    uint16_t vlan = htons(10);
    memcpy(original + 14, &vlan, sizeof(vlan));

    original[16] = 1;

    memcpy(
        original + FRAME_HEADER_SIZE,
        "HELLO",
        5
    );

    printf("Sending frame...\n");

    if (frame_send(
            sockets[0],
            original,
            sizeof(original)) != 0) {

        perror("frame_send");
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    printf("Frame sent successfully.\n");

    uint8_t *received = NULL;
    size_t received_size = 0;

    printf("Receiving frame...\n");

    int result = frame_recv(
        sockets[1],
        &received,
        &received_size
    );

    if (result != 1) {
        perror("frame_recv");
        close(sockets[0]);
        close(sockets[1]);
        return 1;
    }

    printf("Frame received successfully.\n");

    if (received_size != sizeof(original) ||
        memcmp(received, original, sizeof(original)) != 0) {

        printf("Frame comparison FAILED.\n");

        free(received);
        close(sockets[0]);
        close(sockets[1]);

        return 1;
    }

    printf("Frame comparison PASSED.\n");
    printf("Frame size: %zu bytes\n", received_size);

    free(received);

    close(sockets[0]);
    close(sockets[1]);

    printf("\nAll frame I/O tests completed.\n");

        printf("\n--- ADDITIONAL FRAME I/O TESTS ---\n");

    if (test_disconnect_handling() != 0 ||
        test_invalid_length() != 0) {
        return 1;
    }

    return 0;
}