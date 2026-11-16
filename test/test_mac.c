#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdint.h>
#include <time.h>

int main(void)
{
    mac_table_t table;

    if (mac_table_init(&table, 3, 3) != 0) {
        printf("Initialization failed\n");
        return 1;
    }

    uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    uint8_t mac3[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
    uint8_t mac4[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04};

    printf("\n--- INSERTION TEST ---\n");

    if (mac_table_insert(&table, mac1, 0) != 0 ||
        mac_table_insert(&table, mac2, 1) != 0 ||
        mac_table_insert(&table, mac3, 2) != 0) {
        printf("Insertion failed\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Entries: %zu\n", table.count);

    printf("\n--- LRU EVICTION TEST ---\n");

    int port;

    if (mac_table_lookup(&table, mac1, &port) != 1) {
        printf("MAC 1 lookup failed\n");
        mac_table_destroy(&table);
        return 1;
    }

    if (mac_table_insert(&table, mac4, 3) != 0) {
        printf("MAC 4 insertion failed\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Entries after eviction: %zu\n", table.count);

    printf("\n--- LRU VERIFICATION ---\n");

    int mac1_result = mac_table_lookup(&table, mac1, &port);
    int mac2_result = mac_table_lookup(&table, mac2, &port);

    printf("MAC 1: %s\n",
        mac1_result == 1 ? "FOUND" : "NOT FOUND");

    printf("MAC 2: %s\n",
        mac2_result == 1 ? "FOUND" : "NOT FOUND");

    if (mac1_result != 1 || mac2_result != 0) {
        printf("LRU test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("LRU test PASSED\n");

    printf("\n--- AUTOMATIC AGING TEST ---\n");

    printf("Waiting for automatic aging...\n");

    struct timespec delay = {
        .tv_sec = 5,
        .tv_nsec = 0
    };

    if (nanosleep(&delay, NULL) != 0) {
        perror("nanosleep");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Remaining entries: %zu\n", table.count);

    if (table.count != 0) {
        printf("Automatic aging test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Automatic aging test PASSED\n");

    mac_table_destroy(&table);

    printf("\nAll tests completed successfully.\n");

    return 0;
}