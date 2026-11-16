#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned int mac_hash(const uint8_t mac[6])
{
    unsigned int hash = 2166136261u;

    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;
    }

    return hash % MAC_TABLE_BUCKETS;
}

/* Get a unique access number for LRU tracking. */

static uint_fast64_t next_access_number(mac_table_t *table)
{
    return atomic_fetch_add(
        &table->access_clock, 1
    ) + 1;
}

/* Remove the least recently used entry.
   Caller must hold every bucket lock. */

static void evict_lru(mac_table_t *table)
{
    mac_entry_t *oldest = NULL;
    mac_bucket_t *oldest_bucket = NULL;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        mac_entry_t *current = table->buckets[i].entries;

        while (current != NULL) {

            if (oldest == NULL ||
                current->last_used < oldest->last_used) {

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

/* Remove expired entries. */

size_t mac_table_age(mac_table_t *table)
{
    if (table == NULL) {
        return 0;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return 0;
    }

    int locked = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        if (pthread_mutex_lock(&table->buckets[i].lock) != 0) {

            for (int j = locked - 1; j >= 0; j--) {
                pthread_mutex_unlock(&table->buckets[j].lock);
            }

            pthread_mutex_unlock(&table->capacity_lock);
            return 0;
        }

        locked++;
    }

    time_t now = time(NULL);
    size_t removed = 0;

    if (now == (time_t)-1) {
        for (int i = MAC_TABLE_BUCKETS - 1; i >= 0; i--) {
            pthread_mutex_unlock(&table->buckets[i].lock);
        }

        pthread_mutex_unlock(&table->capacity_lock);
        return 0;
    }

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        mac_entry_t **current = &table->buckets[i].entries;

        while (*current != NULL) {

            mac_entry_t *entry = *current;

            if (now >= entry->last_seen &&
                now - entry->last_seen >=
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

    if (removed > 0) {
        printf("Aging removed %zu expired MAC entries\n", removed);
    }

    return removed;
}

/* Background aging thread. */

static void *aging_worker(void *argument)
{
    mac_table_t *table = argument;

    pthread_mutex_lock(&table->aging_lock);

    while (!table->aging_stop) {

        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            break;
        }

        deadline.tv_sec += table->aging_seconds;

        int result = pthread_cond_timedwait(
            &table->aging_cond,
            &table->aging_lock,
            &deadline
        );

        if (table->aging_stop) {
            break;
        }

        if (result == ETIMEDOUT) {

            pthread_mutex_unlock(&table->aging_lock);

            mac_table_age(table);

            pthread_mutex_lock(&table->aging_lock);

        } else if (result != 0) {
            break;
        }
    }

    pthread_mutex_unlock(&table->aging_lock);

    return NULL;
}

/* Initialize MAC table and start aging thread. */

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

    atomic_init(&table->access_clock, 0);

    if (pthread_mutex_init(&table->capacity_lock, NULL) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&table->aging_lock, NULL) != 0) {
        pthread_mutex_destroy(&table->capacity_lock);
        return -1;
    }

    if (pthread_cond_init(&table->aging_cond, NULL) != 0) {
        pthread_mutex_destroy(&table->aging_lock);
        pthread_mutex_destroy(&table->capacity_lock);
        return -1;
    }

    int initialized = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

        if (pthread_mutex_init(&table->buckets[i].lock, NULL) != 0) {

            for (int j = initialized - 1; j >= 0; j--) {
                pthread_mutex_destroy(&table->buckets[j].lock);
            }

            pthread_cond_destroy(&table->aging_cond);
            pthread_mutex_destroy(&table->aging_lock);
            pthread_mutex_destroy(&table->capacity_lock);

            return -1;
        }

        initialized++;
    }

    table->max_entries = max_entries;
    table->aging_seconds = aging_seconds;
    table->count = 0;

    if (pthread_create(
            &table->aging_thread,
            NULL,
            aging_worker,
            table) != 0) {

        for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
            pthread_mutex_destroy(&table->buckets[i].lock);
        }

        pthread_cond_destroy(&table->aging_cond);
        pthread_mutex_destroy(&table->aging_lock);
        pthread_mutex_destroy(&table->capacity_lock);

        return -1;
    }

    table->aging_started = 1;

    return 0;
}

/* Lookup MAC address. */

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
            current->last_used = next_access_number(table);

            pthread_mutex_unlock(&bucket->lock);

            return 1;
        }

        current = current->next;
    }

    pthread_mutex_unlock(&bucket->lock);

    return 0;
}

/* Insert or update MAC address. */

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
            current->last_used = next_access_number(table);

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

        int locked = 0;

        for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {

            if (pthread_mutex_lock(&table->buckets[i].lock) != 0) {

                for (int j = locked - 1; j >= 0; j--) {
                    pthread_mutex_unlock(&table->buckets[j].lock);
                }

                pthread_mutex_unlock(&table->capacity_lock);
                return -1;
            }

            locked++;
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

    time_t now = time(NULL);

    if (now == (time_t)-1) {
        free(new_entry);
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    memcpy(new_entry->mac, mac, 6);

    new_entry->port = port;
    new_entry->last_seen = now;
    new_entry->last_used = next_access_number(table);

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

/* Stop the aging thread and destroy the table. */

void mac_table_destroy(mac_table_t *table)
{
    if (table == NULL) {
        return;
    }

    if (table->aging_started) {

        pthread_mutex_lock(&table->aging_lock);

        table->aging_stop = 1;

        pthread_cond_signal(&table->aging_cond);

        pthread_mutex_unlock(&table->aging_lock);

        pthread_join(table->aging_thread, NULL);

        table->aging_started = 0;
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

    pthread_cond_destroy(&table->aging_cond);
    pthread_mutex_destroy(&table->aging_lock);
    pthread_mutex_destroy(&table->capacity_lock);

    table->count = 0;
}