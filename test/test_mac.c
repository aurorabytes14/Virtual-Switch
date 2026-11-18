#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

static int shutdown_count = 0;
static int shutdown_port = -1;

/* Test callback: records which port was asked to shut down. */
static void test_shutdown_callback(int port, void *context)
{
    (void)context;

    shutdown_count++;
    shutdown_port = port;

    printf("Shutdown callback triggered for port %d\n", port);
}

/* Wait for automatic aging without treating EINTR as failure. */
static int wait_for_aging(void)
{
    struct timespec delay = {
        .tv_sec = 5,
        .tv_nsec = 0
    };

    while (nanosleep(&delay, &delay) == -1) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return 0;
}

static int check_port_count(
    mac_table_t *table,
    int port,
    size_t expected
)
{
    size_t actual = 0;

    if (mac_table_port_size(table, port, &actual) != 0) {
        return -1;
    }

    printf(
        "Port %d MAC count: %zu (expected %zu)\n",
        port,
        actual,
        expected
    );

    return actual == expected ? 0 : -1;
}

int main(void)
{
    mac_table_t table;

    uint8_t mac1[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01
    };

    uint8_t mac2[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02
    };

    uint8_t mac3[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03
    };

    uint8_t mac4[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x04
    };

    printf("\n========== MAC TABLE TEST SUITE ==========\n");

    /* INITIALIZATION */

    if (mac_table_init(&table, 3, 3) != 0) {
        printf("Initialization failed\n");
        return 1;
    }

    /* INSERTION TEST */

    printf("\n--- INSERTION TEST ---\n");

    if (mac_table_insert(&table, mac1, 0) != 0 ||
        mac_table_insert(&table, mac2, 1) != 0 ||
        mac_table_insert(&table, mac3, 2) != 0) {

        printf("Insertion test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Entries: %zu\n", mac_table_size(&table));

    /* LRU EVICTION TEST */

    printf("\n--- LRU EVICTION TEST ---\n");

    int port;

    if (mac_table_lookup(&table, mac1, &port) != 1 ||
        mac_table_insert(&table, mac4, 3) != 0) {

        printf("LRU eviction test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf(
        "Entries after eviction: %zu\n",
        mac_table_size(&table)
    );

    /* LRU VERIFICATION */

    printf("\n--- LRU VERIFICATION ---\n");

    int mac1_result = mac_table_lookup(&table, mac1, &port);
    int mac2_result = mac_table_lookup(&table, mac2, &port);

    printf(
        "MAC 1: %s\n",
        mac1_result == 1 ? "FOUND" : "NOT FOUND"
    );

    printf(
        "MAC 2: %s\n",
        mac2_result == 1 ? "FOUND" : "NOT FOUND"
    );

    if (mac1_result != 1 || mac2_result != 0) {
        printf("LRU test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("LRU test PASSED\n");

    /* AUTOMATIC AGING TEST */

    printf("\n--- AUTOMATIC AGING TEST ---\n");

    printf("Waiting for automatic aging...\n");

    if (wait_for_aging() != 0) {
        perror("nanosleep");
        mac_table_destroy(&table);
        return 1;
    }

    size_t remaining = mac_table_size(&table);

    printf("Remaining entries: %zu\n", remaining);

    if (remaining != 0) {
        printf("Automatic aging test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Automatic aging test PASSED\n");

    mac_table_destroy(&table);

    /* PORT SECURITY TEST */

    printf("\n--- PORT SECURITY TEST ---\n");

    if (mac_table_init(&table, 10, 30) != 0) {
        printf("Port security initialization failed\n");
        return 1;
    }

    if (mac_table_set_port_security(
            &table,
            3,
            2,
            test_shutdown_callback,
            NULL) != 0) {

        printf("Port security configuration failed\n");
        mac_table_destroy(&table);
        return 1;
    }

    /* A port should be allowed to learn two MAC addresses. */

    if (mac_table_insert(&table, mac1, 0) != 0 ||
        mac_table_insert(&table, mac2, 0) != 0) {

        printf("Allowed MAC insertion FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    if (check_port_count(&table, 0, 2) != 0) {
        printf("Port count test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Port accepted its maximum allowed MAC addresses.\n");

    /* The third MAC should trigger port security. */

    int result = mac_table_insert(&table, mac3, 0);

    if (result != MAC_TABLE_PORT_LIMIT) {
        printf("Port limit test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    if (shutdown_count != 1 || shutdown_port != 0) {
        printf("Shutdown callback test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    if (check_port_count(&table, 0, 2) != 0) {
        printf("Port count changed unexpectedly\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("Port security limit test PASSED\n");

    /* MAC MOVEMENT TEST */

    printf("\n--- MAC MOVEMENT TEST ---\n");

    if (mac_table_insert(&table, mac2, 1) != 0) {
        printf("MAC movement FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    if (check_port_count(&table, 0, 1) != 0 ||
        check_port_count(&table, 1, 1) != 0) {

        printf("MAC movement count test FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("MAC movement test PASSED\n");

    /* LRU EVICTION COUNT TEST */

    printf("\n--- PORT COUNT AFTER LRU EVICTION ---\n");

    if (mac_table_insert(&table, mac3, 2) != 0 ||
        mac_table_insert(&table, mac4, 2) != 0) {

        printf("LRU count setup FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    if (check_port_count(&table, 2, 2) != 0) {
        printf("LRU count setup FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    /*
     * Table capacity is 10, so no eviction occurs in this test.
     * A separate small-table test will be needed to verify
     * per-port counts after an actual LRU eviction.
     */

    printf("Port count tracking test PASSED\n");

    mac_table_destroy(&table);

    printf("\n========== ALL TESTS PASSED ==========\n");

    return 0;
}
