#define _POSIX_C_SOURCE 200809L

#include "frame_io.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int test_mid_frame_disconnect(void)
{
    printf("\n--- MID-FRAME DISCONNECTION TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    /*
     * A frame announces a 10-byte payload,
     * but the sender transmits only 3 bytes.
     */
    uint8_t partial_frame[] = {
        0x00, 0x0A,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x0A,
        0x01,
        'A', 'B', 'C'
    };

    size_t partial_size = sizeof(partial_frame);

    ssize_t written = write(sockets[0], partial_frame, partial_size);

    if (written != (ssize_t)partial_size) {
        perror("write");
        close(sockets[0]);
        close(sockets[1]);
        return -1;
    }

    /* Disconnect before completing the payload. */
    close(sockets[0]);

    uint8_t *received = NULL;
    size_t received_size = 0;

    int result = frame_recv(sockets[1], &received, &received_size);

    free(received);
    close(sockets[1]);

    if (result != -1 || errno != ECONNRESET) {
        fprintf(stderr,
                "Expected ECONNRESET, got result=%d errno=%d\n",
                result, errno);
        return -1;
    }

    printf("Partial frame disconnect handled correctly.\n");

    return 0;
}

static int test_epipe_handling(void)
{
    printf("\n--- EPIPE HANDLING TEST ---\n");

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return -1;
    }

    /*
     * Close the receiving side before sending.
     * frame_send uses MSG_NOSIGNAL, so SIGPIPE
     * should not terminate the process.
     */
    close(sockets[1]);

    uint8_t frame[17] = {
    0x00, 0x00,             /* Payload length: 0 */
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01, /* Source MAC */
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, /* Destination MAC */
    0x00, 0x01,             /* VLAN: 1 */
    0x00                    /* Priority: 0 */
};

    errno = 0;

    int result = frame_send(sockets[0], frame, sizeof(frame));
    int saved_errno = errno;

    close(sockets[0]);

    if (result != -1 || saved_errno != EPIPE) {
        fprintf(stderr,
                "Expected EPIPE, got result=%d errno=%d\n",
                result, saved_errno);
        return -1;
    }

    printf("EPIPE handled without process termination.\n");

    return 0;
}

int main(void)
{
    printf("\n========== DISCONNECTION TEST SUITE ==========\n");

    /*
     * Ignore SIGPIPE as an additional safeguard.
     * frame_send should also use MSG_NOSIGNAL.
     */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    if (test_mid_frame_disconnect() != 0) {
        return EXIT_FAILURE;
    }

    if (test_epipe_handling() != 0) {
        return EXIT_FAILURE;
    }

    printf("\nAll disconnection tests passed.\n");

    return EXIT_SUCCESS;
}