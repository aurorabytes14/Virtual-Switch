#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned int mac_hash(const uint8_t mac[6])
{
    unsigned int hash = 2166136261u;

    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;
    }

    return hash % MAC_TABLE_BUCKETS;
}

/* Initialize MAC table */

int mac_table_init(
    mac_table_t *table,
    size_t max_entries,
    unsigned int aging_seconds)
{
    if (table == NULL || max_entries == 0 ||
        aging_seconds == 0) {
        return -1;
    }

    memset(table, 0, sizeof(*table));

    if (pthread_mutex_init(&table->capacity_lock, NULL) != 0) {
        return -1;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        if (pthread_mutex_init(
                &table->buckets[i].lock, NULL) != 0) {

            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(
                    &table->buckets[j].lock);
            }

            pthread_mutex_destroy(&table->capacity_lock);

            return -1;
        }
    }

    table->max_entries = max_entries;
    table->aging_seconds = aging_seconds;
    table->count = 0;

    return 0;
}

/* Lookup MAC address */

int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port)
{
    if (table == NULL || mac == NULL || port == NULL) {
        return -1;
    }

    unsigned int index = mac_hash(mac);

    mac_bucket_t *bucket = &table->buckets[index];

    if (pthread_mutex_lock(&bucket->lock) != 0) {
        return -1;
    }

    mac_entry_t *current = bucket->entries;

    while (current != NULL) {

        if (memcmp(current->mac, mac, 6) == 0) {

            *port = current->port;
            current->last_seen = time(NULL);

            pthread_mutex_unlock(&bucket->lock);

            return 1;
        }

        current = current->next;
    }

    pthread_mutex_unlock(&bucket->lock);

    return 0;
}

/* Remove least recently used entry */

static void evict_lru(mac_table_t *table)
{
    mac_entry_t *oldest = NULL;
    mac_bucket_t *oldest_bucket = NULL;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        mac_entry_t *current = table->buckets[i].entries;

        while (current != NULL) {

            if (oldest == NULL ||
                current->last_seen < oldest->last_seen) {

                oldest = current;
                oldest_bucket = &table->buckets[i];
            }

            current = current->next;
        }
    }

    if (oldest != NULL && oldest_bucket != NULL) {

        mac_entry_t **current = &oldest_bucket->entries;

        while (*current != NULL) {

            if (*current == oldest) {

                *current = oldest->next;

                free(oldest);

                table->count--;

                printf("LRU eviction performed\n");

                return;
            }

            current = &(*current)->next;
        }
    }
}

/* Insert MAC address */

int mac_table_insert(
    mac_table_t *table,
    const uint8_t mac[6],
    int port)
{
    if (table == NULL || mac == NULL || port < 0) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    unsigned int index = mac_hash(mac);

    mac_bucket_t *bucket = &table->buckets[index];

    if (pthread_mutex_lock(&bucket->lock) != 0) {
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    mac_entry_t *current = bucket->entries;

    while (current != NULL) {

        if (memcmp(current->mac, mac, 6) == 0) {

            int old_port = current->port;

            current->port = port;
            current->last_seen = time(NULL);

            pthread_mutex_unlock(&bucket->lock);
            pthread_mutex_unlock(&table->capacity_lock);

            if (old_port != port) {
                printf(
                    "MAC moved: port %d -> port %d\n",
                    old_port,
                    port
                );
            }

            return 0;
        }

        current = current->next;
    }

    pthread_mutex_unlock(&bucket->lock);

    if (table->count >= table->max_entries) {

        /*
         * Acquire every bucket lock in ascending order.
         * This prevents lookups or aging from changing
         * entries while LRU eviction is performed.
         */

        for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

            if (pthread_mutex_lock(
                    &table->buckets[i].lock) != 0) {

                for (int j = i - 1; j >= 0; j--) {
                    pthread_mutex_unlock(
                        &table->buckets[j].lock);
                }

                pthread_mutex_unlock(&table->capacity_lock);

                return -1;
            }
        }

        evict_lru(table);

        for (int i = MAC_TABLE_BUCKETS - 1; i >= 0; i--) {
            pthread_mutex_unlock(&table->buckets[i].lock);
        }
    }

    mac_entry_t *new_entry = malloc(sizeof(*new_entry));

    if (new_entry == NULL) {
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    memcpy(new_entry->mac, mac, 6);

    new_entry->port = port;
    new_entry->last_seen = time(NULL);

    if (pthread_mutex_lock(&bucket->lock) != 0) {
        free(new_entry);
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    new_entry->next = bucket->entries;
    bucket->entries = new_entry;

    table->count++;

    pthread_mutex_unlock(&bucket->lock);
    pthread_mutex_unlock(&table->capacity_lock);

    return 0;
}

/* Remove entries that have expired */

size_t mac_table_age(mac_table_t *table)
{
    if (table == NULL) {
        return 0;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return 0;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        if (pthread_mutex_lock(&table->buckets[i].lock) != 0) {

            for (int j = i - 1; j >= 0; j--) {
                pthread_mutex_unlock(&table->buckets[j].lock);
            }

            pthread_mutex_unlock(&table->capacity_lock);

            return 0;
        }
    }

    time_t now = time(NULL);

    size_t removed = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        mac_entry_t **current = &table->buckets[i].entries;

        while (*current != NULL) {

            mac_entry_t *entry = *current;

            if (now - entry->last_seen >=
                (time_t)table->aging_seconds) {

                *current = entry->next;

                free(entry);

                table->count--;
                removed++;
            } else {
                current = &entry->next;
            }
        }
    }

    for (int i = MAC_TABLE_BUCKETS - 1; i >= 0; i--) {
        pthread_mutex_unlock(&table->buckets[i].lock);
    }

    pthread_mutex_unlock(&table->capacity_lock);

    return removed;
}

/* Destroy MAC table */

void mac_table_destroy(mac_table_t *table)
{
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

        pthread_mutex_destroy(&table->buckets[i].lock);
    }

    pthread_mutex_destroy(&table->capacity_lock);

    table->count = 0;
}