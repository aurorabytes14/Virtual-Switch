#define _POSIX_C_SOURCE 200809L

#include "frame_io.h"
#include "frame.h"

#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int recv_full(int fd, uint8_t *buffer, size_t length)
{
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(
            fd,
            buffer + received,
            length - received,
            0
        );

        if (result > 0) {
            received += (size_t)result;
        } else if (result == 0) {
            if (received == 0) {
                return 0;
            }

            errno = ECONNRESET;
            return -1;
        } else {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }
    }

    return 1;
}

static int send_full(int fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(
            fd,
            buffer + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (result > 0) {
            sent += (size_t)result;
        } else if (result == 0) {
            errno = EPIPE;
            return -1;
        } else {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }
    }

    return 0;
}

int frame_recv(int fd, uint8_t **frame, size_t *frame_size)
{
    if (fd < 0 || frame == NULL || frame_size == NULL) {
        errno = EINVAL;
        return -1;
    }

    *frame = NULL;
    *frame_size = 0;

    uint8_t header_buffer[FRAME_HEADER_SIZE];

    int result = recv_full(
        fd,
        header_buffer,
        FRAME_HEADER_SIZE
    );

    if (result <= 0) {
        return result;
    }

    frame_header_t header;

    if (frame_deserialize_header(header_buffer, &header) != 0) {
        errno = EPROTO;
        return -1;
    }

    size_t total_size =
        FRAME_HEADER_SIZE + (size_t)header.length;

    uint8_t *buffer = malloc(total_size);

    if (buffer == NULL) {
        return -1;
    }

    memcpy(buffer, header_buffer, FRAME_HEADER_SIZE);

    if (header.length > 0) {
        result = recv_full(
            fd,
            buffer + FRAME_HEADER_SIZE,
            header.length
        );

        if (result != 1) {
            free(buffer);

            if (result == 0) {
                errno = ECONNRESET;
            }

            return -1;
        }
    }

    *frame = buffer;
    *frame_size = total_size;

    return 1;
}

int frame_send(int fd, const uint8_t *frame, size_t frame_size)
{
    if (fd < 0 || frame == NULL ||
        frame_size < FRAME_HEADER_SIZE) {
        errno = EINVAL;
        return -1;
    }

    frame_header_t header;

    if (frame_deserialize_header(frame, &header) != 0) {
        errno = EPROTO;
        return -1;
    }

    size_t expected_size =
        FRAME_HEADER_SIZE + (size_t)header.length;

    if (frame_size != expected_size) {
        errno = EINVAL;
        return -1;
    }

    return send_full(fd, frame, frame_size);
}
