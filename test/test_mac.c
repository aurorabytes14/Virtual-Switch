#include "mac_table.h"

#include <stdio.h>
#include <stdint.h>

int main(void)
{
    mac_table_t table;

    if (mac_table_init(&table, 3, 10) != 0) {
        printf("Initialization failed\n");
        return 1;
    }

    uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    uint8_t mac3[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
    uint8_t mac4[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04};

    printf("\n--- INSERTION TEST ---\n");

    mac_table_insert(&table, mac1, 0);
    mac_table_insert(&table, mac2, 1);
    mac_table_insert(&table, mac3, 2);

    printf("Entries: %zu\n", table.count);

    printf("\n--- LRU EVICTION TEST ---\n");

    int port;

    mac_table_lookup(&table, mac1, &port);

    mac_table_insert(&table, mac4, 3);

    printf("Entries after eviction: %zu\n", table.count);

    printf("\n--- LOOKUP TEST ---\n");

    printf("MAC 1: %s\n",
        mac_table_lookup(&table, mac1, &port) == 1
        ? "FOUND" : "NOT FOUND");

    printf("MAC 2: %s\n",
        mac_table_lookup(&table, mac2, &port) == 1
        ? "FOUND" : "NOT FOUND");

    printf("\n--- AGING TEST ---\n");

    table.aging_seconds = 1;

    /* Give the entries time to expire. */

    struct timespec delay = {
        .tv_sec = 2,
        .tv_nsec = 0
    };

    if (nanosleep(&delay, NULL) != 0) {
        perror("nanosleep");
        mac_table_destroy(&table);
        return 1;
    }

    size_t removed = mac_table_age(&table);

    printf("Expired entries removed: %zu\n", removed);
    printf("Remaining entries: %zu\n", table.count);

    mac_table_destroy(&table);

    printf("\nAll tests completed.\n");

    return 0;
}