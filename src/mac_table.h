#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>

#define MAC_TABLE_BUCKETS 16
#define MAC_TABLE_PORT_LIMIT -2

typedef void (*port_shutdown_callback_t)(int port, void *context);

typedef struct mac_entry
{
    uint16_t vlan;
    uint8_t mac[6];
    int port;
    time_t last_seen;
    uint_fast64_t last_used;
    struct mac_entry *next;
} mac_entry_t;

typedef struct {
    mac_entry_t *entries;
    pthread_mutex_t lock;
} mac_bucket_t;

typedef struct {
    mac_bucket_t buckets[MAC_TABLE_BUCKETS];

    pthread_mutex_t capacity_lock;

    size_t *port_mac_counts;
    size_t num_ports;
    size_t max_macs_per_port;

    port_shutdown_callback_t shutdown_callback;
    void *callback_context;

    pthread_mutex_t aging_lock;
    pthread_cond_t aging_cond;
    pthread_t aging_thread;

    int aging_stop;
    int aging_started;

    atomic_uint_fast64_t access_clock;

    size_t count;
    size_t max_entries;
    unsigned int aging_seconds;
} mac_table_t;

int mac_table_init(
    mac_table_t *table,
    size_t max_entries,
    unsigned int aging_seconds
);

int mac_table_set_port_security(
    mac_table_t *table,
    size_t num_ports,
    size_t max_macs_per_port,
    port_shutdown_callback_t callback,
    void *callback_context
);

void mac_table_destroy(mac_table_t *table);

/* VLAN-aware MAC table operations. */
int mac_table_lookup_vlan(
    mac_table_t *table,
    uint16_t vlan,
    const uint8_t mac[6],
    int *port
);

int mac_table_insert_vlan(
    mac_table_t *table,
    uint16_t vlan,
    const uint8_t mac[6],
    int port
);

/* Backward-compatible operations using VLAN 1. */
int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port
);

int mac_table_insert(
    mac_table_t *table,
    const uint8_t mac[6],
    int port
);

size_t mac_table_age(mac_table_t *table);
size_t mac_table_size(mac_table_t *table);

int mac_table_port_size(
    mac_table_t *table,
    int port,
    size_t *count
);

/* Admin-plane operations. */
int mac_table_print(mac_table_t *table, FILE *output);
int mac_table_clear(mac_table_t *table);

#endif
