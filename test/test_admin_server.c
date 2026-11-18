#define _POSIX_C_SOURCE 200809L

#include "admin_server.h"
#include "admin_commands.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

int main(void)
{
    printf("\n========== REAL ADMIN SERVER TEST ==========\n");

    mac_table_t mac_table;

    if (mac_table_init(&mac_table, 100, 300) != 0) {
        perror("mac_table_init");
        return 1;
    }

    uint8_t mac1[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01
    };

    uint8_t mac2[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02
    };

    uint8_t mac3[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03
    };

    if (mac_table_insert(&mac_table, mac1, 0) != 0 ||
        mac_table_insert(&mac_table, mac2, 1) != 0 ||
        mac_table_insert(&mac_table, mac3, 2) != 0) {

        fprintf(stderr, "Could not insert test MAC addresses.\n");
        mac_table_destroy(&mac_table);
        return 1;
    }

    /* Create shared-memory statistics for three ports. */
    shm_stats_t *stats = NULL;
    int shm_fd = -1;

    if (shm_stats_create(
            SHM_STATS_NAME,
            3,
            &stats,
            &shm_fd) != 0) {

        perror("shm_stats_create");

        mac_table_destroy(&mac_table);
        return 1;
    }

    /* Populate sample statistics. */
    shm_stats_update(stats, 0, 120, 95, 2, 0);
    shm_stats_update(stats, 1, 85, 110, 1, 0);
    shm_stats_update(stats, 2, 60, 55, 0, 3);

    /* Connect both components to the admin handler. */
    admin_context_t admin_context;

    admin_context.mac_table = &mac_table;
    admin_context.stats = stats;

    /* Handle Ctrl+C gracefully. */
    struct sigaction action = {0};

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {

        perror("sigaction");

        shm_stats_detach(stats, shm_fd);
        shm_stats_unlink(SHM_STATS_NAME);
        mac_table_destroy(&mac_table);

        return 1;
    }

    /* Start the actual admin server. */
    admin_server_t server;

    if (admin_server_start(
            &server,
            ADMIN_SOCKET_PATH,
            admin_handle_command,
            &admin_context) != 0) {

        perror("admin_server_start");

        shm_stats_detach(stats, shm_fd);
        shm_stats_unlink(SHM_STATS_NAME);
        mac_table_destroy(&mac_table);

        return 1;
    }

    printf("\nAdmin server started successfully.\n");
    printf("Socket: %s\n", ADMIN_SOCKET_PATH);
    printf("Shared memory: %s\n", SHM_STATS_NAME);

    printf("\nAvailable commands:\n");
    printf("  ./swctl show mac\n");
    printf("  ./swctl clear mac\n");
    printf("  ./swctl show ports\n");

    printf("\nPress Ctrl+C to stop.\n\n");
    fflush(stdout);

    while (!stop_requested) {
        pause();
    }

    printf("\nStopping admin server...\n");

    admin_server_stop(&server);

    shm_stats_detach(stats, shm_fd);
    shm_stats_unlink(SHM_STATS_NAME);

    mac_table_destroy(&mac_table);

    printf("Admin server stopped.\n");
    printf("Shared memory cleaned up.\n");
    printf("MAC table destroyed.\n");

    return 0;
}