#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define MAC_TABLE_BUCKETS 16

typedef struct mac_entry {
    uint8_t mac[6];
    int port;
    time_t last_seen;
    struct mac_entry *next;
} mac_entry_t;

typedef struct {
    mac_entry_t *entries;
    pthread_mutex_t lock;
} mac_bucket_t;

typedef struct {
    mac_bucket_t buckets[MAC_TABLE_BUCKETS];

    pthread_mutex_t capacity_lock;

    size_t count;
    size_t max_entries;

    unsigned int aging_seconds;
} mac_table_t;

int mac_table_init(
    mac_table_t *table,
    size_t max_entries,
    unsigned int aging_seconds
);

void mac_table_destroy(mac_table_t *table);

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

#endif