#define _POSIX_C_SOURCE 200809L

#include "admin_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Remove leading and trailing whitespace.
 */
static void trim_command(char *command)
{
    char *start = command;

    while (*start != '\0' &&
           isspace((unsigned char)*start)) {
        start++;
    }

    if (start != command) {
        memmove(command, start, strlen(start) + 1);
    }

    size_t length = strlen(command);

    while (length > 0 &&
           isspace((unsigned char)command[length - 1])) {
        command[length - 1] = '\0';
        length--;
    }
}

/*
 * Display the actual MAC table.
 */
static int handle_show_mac(
    mac_table_t *table,
    char *response,
    size_t response_size
)
{
    char *buffer = NULL;
    size_t buffer_size = 0;

    FILE *stream = open_memstream(&buffer, &buffer_size);

    if (stream == NULL) {
        snprintf(
            response,
            response_size,
            "ERROR: Could not create MAC table output.\n"
        );
        return -1;
    }

    int result = mac_table_print(table, stream);

    if (fclose(stream) != 0) {
        free(buffer);

        snprintf(
            response,
            response_size,
            "ERROR: Could not generate MAC table output.\n"
        );

        return -1;
    }

    if (result != 0) {
        free(buffer);

        snprintf(
            response,
            response_size,
            "ERROR: Could not read MAC table.\n"
        );

        return -1;
    }

    if (buffer_size >= response_size) {
        memcpy(response, buffer, response_size - 1);
        response[response_size - 1] = '\0';

        free(buffer);
        return 0;
    }

    memcpy(response, buffer, buffer_size + 1);

    free(buffer);

    return 0;
}

/*
 * Clear the actual MAC table.
 */
static int handle_clear_mac(
    mac_table_t *table,
    char *response,
    size_t response_size
)
{
    size_t previous_entries = mac_table_size(table);

    if (mac_table_clear(table) != 0) {
        snprintf(
            response,
            response_size,
            "ERROR: Could not clear MAC table.\n"
        );

        return -1;
    }

    snprintf(
        response,
        response_size,
        "MAC table cleared successfully.\n"
        "Entries removed: %zu\n",
        previous_entries
    );

    return 0;
}

/*
 * Display live statistics for every switch port.
 */
static int handle_show_ports(
    shm_stats_t *stats,
    char *response,
    size_t response_size
)
{
    if (stats == NULL) {
        snprintf(
            response,
            response_size,
            "ERROR: Port statistics are unavailable.\n"
        );

        return -1;
    }

    size_t used = 0;

    int written = snprintf(
        response,
        response_size,
        "\n========== SWITCH PORT STATISTICS ==========\n"
    );

    if (written < 0 || (size_t)written >= response_size) {
        return -1;
    }

    used = (size_t)written;

    for (size_t i = 0; i < stats->num_ports; i++) {

        port_stats_t port_stats;

        if (shm_stats_read(stats, i, &port_stats) != 0) {
            snprintf(
                response + used,
                response_size - used,
                "\nERROR: Could not read statistics for port %zu.\n",
                i
            );

            return -1;
        }

        size_t remaining = response_size - used;

        written = snprintf(
            response + used,
            remaining,
            "\nPort %zu\n"
            "  Frames received:  %llu\n"
            "  Frames transmitted: %llu\n"
            "  Frames dropped:   %llu\n"
            "  Frames mirrored:  %llu\n",
            i,
            (unsigned long long)port_stats.frames_in,
            (unsigned long long)port_stats.frames_out,
            (unsigned long long)port_stats.frames_dropped,
            (unsigned long long)port_stats.frames_mirrored
        );

        if (written < 0 || (size_t)written >= remaining) {
            const char truncated[] = "\n[Output truncated]\n";
            size_t length = sizeof(truncated) - 1;

            if (remaining > length) {
                memcpy(response + response_size - length - 1,
                       truncated,
                       length);
                response[response_size - 1] = '\0';
            } else {
                response[response_size - 1] = '\0';
            }

            return 0;
        }

        used += (size_t)written;
    }

    return 0;
}

/*
 * Main admin command dispatcher.
 */
int admin_handle_command(
    const char *command,
    char *response,
    size_t response_size,
    void *context
)
{
    if (command == NULL ||
        response == NULL ||
        response_size == 0 ||
        context == NULL) {
        return -1;
    }

    admin_context_t *admin = context;

    char command_copy[256];

    snprintf(
        command_copy,
        sizeof(command_copy),
        "%s",
        command
    );

    trim_command(command_copy);

    if (strcmp(command_copy, "show mac") == 0) {

        if (admin->mac_table == NULL) {
            snprintf(
                response,
                response_size,
                "ERROR: MAC table is unavailable.\n"
            );

            return -1;
        }

        return handle_show_mac(
            admin->mac_table,
            response,
            response_size
        );
    }

    if (strcmp(command_copy, "clear mac") == 0) {

        if (admin->mac_table == NULL) {
            snprintf(
                response,
                response_size,
                "ERROR: MAC table is unavailable.\n"
            );

            return -1;
        }

        return handle_clear_mac(
            admin->mac_table,
            response,
            response_size
        );
    }

    if (strcmp(command_copy, "show ports") == 0) {

        return handle_show_ports(
            admin->stats,
            response,
            response_size
        );
    }

    snprintf(
        response,
        response_size,
        "ERROR: Unknown command: %s\n",
        command_copy
    );

    return -1;
}