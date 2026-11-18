#include "shared_frame.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

shared_frame_t *shared_frame_create(
    const uint8_t *data,
    size_t size
)
{
    if (data == NULL || size == 0 ||
        size > SIZE_MAX - sizeof(shared_frame_t)) {
        errno = EINVAL;
        return NULL;
    }

    shared_frame_t *frame = malloc(sizeof(*frame) + size);

    if (frame == NULL) {
        return NULL;
    }

    atomic_init(&frame->references, 1);
    frame->size = size;

    memcpy(frame->data, data, size);

    return frame;
}

void shared_frame_retain(shared_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    atomic_fetch_add(&frame->references, 1);
}

void shared_frame_release(shared_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    if (atomic_fetch_sub(&frame->references, 1) == 1) {
        free(frame);
    }
}