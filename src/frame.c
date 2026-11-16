#include "frame.h"

#include <string.h>
#include <arpa/inet.h>

int frame_serialize_header(
    const frame_header_t *header,
    uint8_t buffer[FRAME_HEADER_SIZE]
) {
    if (header == NULL || buffer == NULL) {
        return -1;
    }

    if (header->length > FRAME_MAX_PAYLOAD ||
        header->vlan < 1 || header->vlan > 4094 ||
        header->priority > 2) {
        return -1;
    }

    uint16_t network_length = htons(header->length);
    uint16_t network_vlan = htons(header->vlan);

    size_t offset = 0;

    memcpy(buffer + offset, &network_length, 2);
    offset += 2;

    memcpy(buffer + offset, header->src, 6);
    offset += 6;

    memcpy(buffer + offset, header->dst, 6);
    offset += 6;

    memcpy(buffer + offset, &network_vlan, 2);
    offset += 2;

    buffer[offset] = header->priority;

    return 0;
}

int frame_deserialize_header(
    const uint8_t buffer[FRAME_HEADER_SIZE],
    frame_header_t *header
) {
    if (buffer == NULL || header == NULL) {
        return -1;
    }

    uint16_t network_length;
    uint16_t network_vlan;

    size_t offset = 0;

    memcpy(&network_length, buffer + offset, 2);
    offset += 2;

    memcpy(header->src, buffer + offset, 6);
    offset += 6;

    memcpy(header->dst, buffer + offset, 6);
    offset += 6;

    memcpy(&network_vlan, buffer + offset, 2);
    offset += 2;

    header->priority = buffer[offset];

    header->length = ntohs(network_length);
    header->vlan = ntohs(network_vlan);

    if (header->length > FRAME_MAX_PAYLOAD ||
        header->vlan < 1 || header->vlan > 4094 ||
        header->priority > 2) {
        return -1;
    }

    return 0;
}