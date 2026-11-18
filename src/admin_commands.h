#ifndef ADMIN_COMMANDS_H
#define ADMIN_COMMANDS_H

#include <stddef.h>

#include "mac_table.h"
#include "shm_stats.h"

typedef struct {
    mac_table_t *mac_table;
    shm_stats_t *stats;
} admin_context_t;

int admin_handle_command(
    const char *command,
    char *response,
    size_t response_size,
    void *context
);

#endif