#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#define FRAME_HEADER_SIZE 17
#define FRAME_MAX_PAYLOAD 1500

typedef struct 
{
    uint16_t length;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t vlan;
    uint8_t priority;
} frame_header_t;

int frame_serialize_header(
    const frame_header_t *header,
    uint8_t buffer[FRAME_HEADER_SIZE]
);

int frame_deserialize_header(
    const uint8_t buffer[FRAME_HEADER_SIZE],
    frame_header_t *header
);

#endif