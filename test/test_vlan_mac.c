#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "mac_table.h"

int main(void)
{
    mac_table_t table;

    uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};

    int port;

    assert(mac_table_init(&table, 100, 60) == 0);

    printf("\n=== VLAN MAC TABLE TEST ===\n");

    /* Learn the same MAC on VLAN 10 and VLAN 20 */
    assert(mac_table_insert_vlan(&table, 10, mac, 0) == 0);
    assert(mac_table_insert_vlan(&table, 20, mac, 2) == 0);

    /* VLAN 10 must resolve to port 0 */
    assert(mac_table_lookup_vlan(&table, 10, mac, &port) == 1);
    assert(port == 0);

    /* VLAN 20 must resolve to port 2 */
    assert(mac_table_lookup_vlan(&table, 20, mac, &port) == 1);
    assert(port == 2);

    /* VLAN 30 must not find this MAC */
    assert(mac_table_lookup_vlan(&table, 30, mac, &port) == 0);

    /* Both entries must exist */
    assert(mac_table_size(&table) == 2);

    printf("Same MAC on VLAN 10 and VLAN 20: PASSED\n");
    printf("VLAN 10 -> Port 0: PASSED\n");
    printf("VLAN 20 -> Port 2: PASSED\n");
    printf("VLAN 30 isolation: PASSED\n");
    printf("Total entries: %zu\n", mac_table_size(&table));

    mac_table_destroy(&table);

    printf("\nALL VLAN MAC TESTS PASSED\n");

    return 0;
}