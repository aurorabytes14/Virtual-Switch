#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int mac_hash(const uint8_t mac[6]) {

    unsigned int hash = 2166136261u;

    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;
    }

    return hash % MAC_TABLE_BUCKETS;
}

int mac_table_init(mac_table_t *table) {

    if (table == NULL) {
        return -1;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        table->buckets[i].entries = NULL;

        int result = pthread_mutex_init(
            &table->buckets[i].lock,
            NULL
        );

        if (result != 0) {

            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(
                    &table->buckets[j].lock
                );
            }

            return -1;
        }
    }

    return 0;
}

void mac_table_destroy(mac_table_t *table) {

    if (table == NULL) {
        return;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        mac_entry_t *current = table->buckets[i].entries;

        while (current != NULL) {

            mac_entry_t *next = current->next;

            free(current);

            current = next;
        }

        table->buckets[i].entries = NULL;

        pthread_mutex_destroy(
            &table->buckets[i].lock
        );
    }
}

int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port
) {

    if (table == NULL || mac == NULL || port == NULL) {
        return -1;
    }

    unsigned int index = mac_hash(mac);

    mac_bucket_t *bucket = &table->buckets[index];

    pthread_mutex_lock(&bucket->lock);

    mac_entry_t *current = bucket->entries;

    while (current != NULL) {

        if (memcmp(current->mac, mac, 6) == 0) {

            *port = current->port;

            pthread_mutex_unlock(&bucket->lock);

            return 1;
        }

        current = current->next;
    }

    pthread_mutex_unlock(&bucket->lock);

    return 0;
}

int mac_table_insert(
    mac_table_t *table,
    const uint8_t mac[6],
    int port
) {

    if (table == NULL || mac == NULL || port < 0) {
        return -1;
    }

    unsigned int index = mac_hash(mac);

    mac_bucket_t *bucket = &table->buckets[index];

    pthread_mutex_lock(&bucket->lock);

    mac_entry_t *current = bucket->entries;

    while (current != NULL) {

        if (memcmp(current->mac, mac, 6) == 0) {

            int old_port = current->port;

            current->port = port;

            pthread_mutex_unlock(&bucket->lock);

            if (old_port != port) {
                fprintf(
                    stderr,
                    "MAC moved: port %d -> port %d\n",
                    old_port,
                    port
                );
            }

            return 0;
        }

        current = current->next;
    }

    mac_entry_t *new_entry = malloc(sizeof(mac_entry_t));

    if (new_entry == NULL) {

        pthread_mutex_unlock(&bucket->lock);

        return -1;
    }

    memcpy(new_entry->mac, mac, 6);

    new_entry->port = port;

    new_entry->next = bucket->entries;

    bucket->entries = new_entry;

    pthread_mutex_unlock(&bucket->lock);

    return 0;
}