#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct {
    atomic_uint references;
    size_t size;
    uint8_t data[];
} shared_frame_t;

shared_frame_t *shared_frame_create(
    const uint8_t *data,
    size_t size
);

void shared_frame_retain(shared_frame_t *frame);

void shared_frame_release(shared_frame_t *frame);

#endif