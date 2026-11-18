#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define SHM_STATS_NAME "/virtual_switch_stats"

typedef struct {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t frames_dropped;
    uint64_t frames_mirrored;
} port_stats_t;

typedef struct {
    uint32_t magic;
    uint32_t num_ports;

    pthread_mutex_t mutex;

    port_stats_t ports[];
} shm_stats_t;

/* Create and initialize a shared memory segment. */
int shm_stats_create(
    const char *name,
    size_t num_ports,
    shm_stats_t **stats,
    int *fd
);

/* Attach to an existing shared memory segment. */
int shm_stats_attach(
    const char *name,
    shm_stats_t **stats,
    int *fd
);

/* Safely update a port's counters. */
int shm_stats_update(
    shm_stats_t *stats,
    size_t port,
    uint64_t frames_in,
    uint64_t frames_out,
    uint64_t frames_dropped,
    uint64_t frames_mirrored
);

/* Safely retrieve a port's counters. */
int shm_stats_read(
    shm_stats_t *stats,
    size_t port,
    port_stats_t *result
);

/* Detach the current process from shared memory. */
void shm_stats_detach(shm_stats_t *stats, int fd);

/* Remove the shared memory object. */
int shm_stats_unlink(const char *name);

#endif