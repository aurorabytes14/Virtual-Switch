#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Hash a MAC address into one of the table buckets. */
static unsigned int mac_hash(const uint8_t mac[6])
{
    unsigned int hash = 2166136261u;

    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;
    }

    return hash % MAC_TABLE_BUCKETS;
}

/* Generate a unique access number for LRU tracking. */
static uint_fast64_t next_access_number(mac_table_t *table)
{
    return atomic_fetch_add(
        &table->access_clock, 1
    ) + 1;
}

/* Decrease the MAC count for a port. capacity_lock must be held. */
static void decrement_port_count(mac_table_t *table, int port)
{
    if (table->port_mac_counts != NULL &&
        port >= 0 &&
        (size_t)port < table->num_ports &&
        table->port_mac_counts[port] > 0) {

        table->port_mac_counts[port]--;
    }
}

/*
 * Lock every bucket in ascending order.
 * capacity_lock must already be held.
 */
static int lock_all_buckets(mac_table_t *table)
{
    int locked = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        if (pthread_mutex_lock(&table->buckets[i].lock) != 0) {
            for (int j = locked - 1; j >= 0; j--) {
                pthread_mutex_unlock(&table->buckets[j].lock);
            }

            return -1;
        }

        locked++;
    }

    return 0;
}

/* Unlock every bucket in reverse order. */
static void unlock_all_buckets(mac_table_t *table)
{
    for (int i = MAC_TABLE_BUCKETS - 1; i >= 0; i--) {
        pthread_mutex_unlock(&table->buckets[i].lock);
    }
}

/*
 * Remove the least recently used entry.
 * capacity_lock and all bucket locks must be held.
 */
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

                decrement_port_count(table, oldest->port);

                free(oldest);
                table->count--;

                printf("LRU eviction performed\n");
                return;
            }

            current = &(*current)->next;
        }
    }
}

/* Remove expired MAC entries. */
size_t mac_table_age(mac_table_t *table)
{
    if (table == NULL) {
        return 0;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return 0;
    }

    if (lock_all_buckets(table) != 0) {
        pthread_mutex_unlock(&table->capacity_lock);
        return 0;
    }

    time_t now = time(NULL);
    size_t removed = 0;

    if (now == (time_t)-1) {
        unlock_all_buckets(table);
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

                decrement_port_count(table, entry->port);

                free(entry);
                table->count--;
                removed++;
            } else {
                current = &entry->next;
            }
        }
    }

    unlock_all_buckets(table);
    pthread_mutex_unlock(&table->capacity_lock);

    if (removed > 0) {
        printf(
            "Aging removed %zu expired MAC entries\n",
            removed
        );
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

/* Initialize the MAC table and start its aging thread. */
int mac_table_init(
    mac_table_t *table,
    size_t max_entries,
    unsigned int aging_seconds
)
{
    if (table == NULL ||
        max_entries == 0 ||
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
        if (pthread_mutex_init(
                &table->buckets[i].lock, NULL) != 0) {

            for (int j = initialized - 1; j >= 0; j--) {
                pthread_mutex_destroy(
                    &table->buckets[j].lock
                );
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

/* Configure per-port MAC security. */
int mac_table_set_port_security(
    mac_table_t *table,
    size_t num_ports,
    size_t max_macs_per_port,
    port_shutdown_callback_t callback,
    void *callback_context
)
{
    if (table == NULL ||
        num_ports == 0 ||
        max_macs_per_port == 0 ||
        num_ports > SIZE_MAX / sizeof(size_t)) {
        return -1;
    }

    size_t *counts = calloc(num_ports, sizeof(size_t));

    if (counts == NULL) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        free(counts);
        return -1;
    }

    if (lock_all_buckets(table) != 0) {
        pthread_mutex_unlock(&table->capacity_lock);
        free(counts);
        return -1;
    }

    /* Count MAC addresses already present in the table. */
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        mac_entry_t *current = table->buckets[i].entries;

        while (current != NULL) {
            if (current->port < 0 ||
                (size_t)current->port >= num_ports) {

                unlock_all_buckets(table);
                pthread_mutex_unlock(&table->capacity_lock);
                free(counts);
                return -1;
            }

            counts[current->port]++;
            current = current->next;
        }
    }

    /* Reject a configuration that is already over the limit. */
    for (size_t i = 0; i < num_ports; i++) {
        if (counts[i] > max_macs_per_port) {
            unlock_all_buckets(table);
            pthread_mutex_unlock(&table->capacity_lock);
            free(counts);
            return -1;
        }
    }

    free(table->port_mac_counts);

    table->port_mac_counts = counts;
    table->num_ports = num_ports;
    table->max_macs_per_port = max_macs_per_port;
    table->shutdown_callback = callback;
    table->callback_context = callback_context;

    unlock_all_buckets(table);
    pthread_mutex_unlock(&table->capacity_lock);

    return 0;
}

/* Look up a MAC address. */
int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port
)
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
            time_t now = time(NULL);

            if (now == (time_t)-1) {
                pthread_mutex_unlock(&bucket->lock);
                return -1;
            }

            *port = current->port;
            current->last_seen = now;
            current->last_used = next_access_number(table);

            pthread_mutex_unlock(&bucket->lock);
            return 1;
        }

        current = current->next;
    }

    pthread_mutex_unlock(&bucket->lock);

    return 0;
}

/* Insert or update a MAC address. */
int mac_table_insert(
    mac_table_t *table,
    const uint8_t mac[6],
    int port
)
{
    if (table == NULL || mac == NULL || port < 0) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    /* Validate the port when port security is enabled. */
    if (table->port_mac_counts != NULL &&
        (size_t)port >= table->num_ports) {

        pthread_mutex_unlock(&table->capacity_lock);
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

            if (old_port != port &&
                table->port_mac_counts != NULL &&
                table->port_mac_counts[port] >=
                    table->max_macs_per_port) {

                port_shutdown_callback_t callback =
                    table->shutdown_callback;

                void *context = table->callback_context;

                pthread_mutex_unlock(&bucket->lock);
                pthread_mutex_unlock(&table->capacity_lock);

                printf(
                    "PORT SECURITY VIOLATION: port %d exceeded MAC limit\n",
                    port
                );

                if (callback != NULL) {
                    callback(port, context);
                }

                return MAC_TABLE_PORT_LIMIT;
            }

            time_t now = time(NULL);

            if (now == (time_t)-1) {
                pthread_mutex_unlock(&bucket->lock);
                pthread_mutex_unlock(&table->capacity_lock);
                return -1;
            }

            current->port = port;
            current->last_seen = now;
            current->last_used = next_access_number(table);

            if (old_port != port &&
                table->port_mac_counts != NULL) {

                decrement_port_count(table, old_port);
                table->port_mac_counts[port]++;
            }

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

    /* Check the per-port limit before adding a new MAC. */
    if (table->port_mac_counts != NULL &&
        table->port_mac_counts[port] >=
            table->max_macs_per_port) {

        port_shutdown_callback_t callback =
            table->shutdown_callback;

        void *context = table->callback_context;

        pthread_mutex_unlock(&bucket->lock);
        pthread_mutex_unlock(&table->capacity_lock);

        printf(
            "PORT SECURITY VIOLATION: port %d exceeded MAC limit\n",
            port
        );

        if (callback != NULL) {
            callback(port, context);
        }

        return MAC_TABLE_PORT_LIMIT;
    }

    pthread_mutex_unlock(&bucket->lock);

    /* Enforce the global table capacity using LRU eviction. */
    if (table->count >= table->max_entries) {
        if (lock_all_buckets(table) != 0) {
            pthread_mutex_unlock(&table->capacity_lock);
            return -1;
        }

        evict_lru(table);

        unlock_all_buckets(table);
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

    if (table->port_mac_counts != NULL) {
        table->port_mac_counts[port]++;
    }

    pthread_mutex_unlock(&bucket->lock);
    pthread_mutex_unlock(&table->capacity_lock);

    return 0;
}

/* Read the total number of entries safely. */
size_t mac_table_size(mac_table_t *table)
{
    if (table == NULL) {
        return 0;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return 0;
    }

    size_t result = table->count;

    pthread_mutex_unlock(&table->capacity_lock);

    return result;
}

/* Read the number of MAC addresses learned on a port. */
int mac_table_port_size(
    mac_table_t *table,
    int port,
    size_t *count
)
{
    if (table == NULL || count == NULL || port < 0) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    if (table->port_mac_counts == NULL ||
        (size_t)port >= table->num_ports) {

        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    *count = table->port_mac_counts[port];

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

    free(table->port_mac_counts);
    table->port_mac_counts = NULL;

    pthread_cond_destroy(&table->aging_cond);
    pthread_mutex_destroy(&table->aging_lock);
    pthread_mutex_destroy(&table->capacity_lock);

    table->count = 0;
}

/* Display all learned MAC addresses and their ports. */
int mac_table_print(mac_table_t *table, FILE *output)
{
    if (table == NULL || output == NULL) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    if (lock_all_buckets(table) != 0) {
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    fprintf(output, "\n========== MAC ADDRESS TABLE ==========\n");
    fprintf(output, "%-20s %-10s\n", "MAC ADDRESS", "PORT");
    fprintf(output, "---------------------------------------\n");

    size_t displayed = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        mac_entry_t *current = table->buckets[i].entries;

        while (current != NULL) {
            fprintf(
                output,
                "%02X:%02X:%02X:%02X:%02X:%02X   %-10d\n",
                current->mac[0],
                current->mac[1],
                current->mac[2],
                current->mac[3],
                current->mac[4],
                current->mac[5],
                current->port
            );

            displayed++;
            current = current->next;
        }
    }

    fprintf(output, "---------------------------------------\n");
    fprintf(output, "Total entries: %zu\n", displayed);

    unlock_all_buckets(table);
    pthread_mutex_unlock(&table->capacity_lock);

    return ferror(output) ? -1 : 0;
}

/* Remove every MAC entry and reset per-port counters. */
int mac_table_clear(mac_table_t *table)
{
    if (table == NULL) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    if (lock_all_buckets(table) != 0) {
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    size_t removed = 0;

    for (int i = 0; i < MAC_TABLE_BUCKETS; i++) {
        mac_entry_t *current = table->buckets[i].entries;

        while (current != NULL) {
            mac_entry_t *next = current->next;
            free(current);
            current = next;
            removed++;
        }

        table->buckets[i].entries = NULL;
    }

    table->count = 0;

    if (table->port_mac_counts != NULL) {
        for (size_t i = 0; i < table->num_ports; i++) {
            table->port_mac_counts[i] = 0;
        }
    }

    unlock_all_buckets(table);
    pthread_mutex_unlock(&table->capacity_lock);

    printf("MAC table cleared. Removed %zu entries.\n", removed);

    return 0;
}
