#ifndef FRAME_IO_H
#define FRAME_IO_H
#include <stddef.h>
#include <stdint.h>


int frame_recv(
    int fd,
    uint8_t **frame,
    size_t *frame_size
);

int frame_send(
    int fd,
    const uint8_t *frame,
    size_t frame_size
);

#endif