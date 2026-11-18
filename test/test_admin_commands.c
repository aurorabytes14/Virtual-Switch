#define _POSIX_C_SOURCE 200809L

#include "admin_commands.h"
#include "admin_server.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    printf("\n========== ADMIN COMMAND TEST ==========\n");

    mac_table_t table;

    if (mac_table_init(&table, 100, 60) != 0) {
        fprintf(stderr, "Failed to initialize MAC table.\n");
        return 1;
    }

    admin_context_t context;
    context.mac_table = &table;

    char response[ADMIN_RESPONSE_SIZE];

    /* Insert two actual MAC addresses. */
    uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};

    mac_table_insert(&table, mac1, 0);
    mac_table_insert(&table, mac2, 1);

    /* Test show mac. */
    printf("\n--- SHOW MAC TEST ---\n");

    memset(response, 0, sizeof(response));

    if (admin_handle_command(
            "show mac",
            response,
            sizeof(response),
            &context) != 0) {

        printf("SHOW MAC FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("%s\n", response);

    if (strstr(response, "AA:BB:CC:DD:EE:01") == NULL ||
        strstr(response, "AA:BB:CC:DD:EE:02") == NULL) {

        printf("SHOW MAC FAILED: Expected addresses not found.\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("SHOW MAC PASSED\n");

    /* Test clear mac. */
    printf("\n--- CLEAR MAC TEST ---\n");

    memset(response, 0, sizeof(response));

    if (admin_handle_command(
            "clear mac",
            response,
            sizeof(response),
            &context) != 0) {

        printf("CLEAR MAC FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("%s\n", response);

    if (mac_table_size(&table) != 0) {
        printf("CLEAR MAC FAILED: Entries remain.\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("CLEAR MAC PASSED\n");

    /* Test unknown command. */
    printf("\n--- INVALID COMMAND TEST ---\n");

    memset(response, 0, sizeof(response));

    if (admin_handle_command(
            "invalid command",
            response,
            sizeof(response),
            &context) == 0) {

        printf("INVALID COMMAND TEST FAILED\n");
        mac_table_destroy(&table);
        return 1;
    }

    printf("%s", response);
    printf("INVALID COMMAND TEST PASSED\n");

    mac_table_destroy(&table);

    printf("\n========== ALL ADMIN COMMAND TESTS PASSED ==========\n");

    return 0;
}