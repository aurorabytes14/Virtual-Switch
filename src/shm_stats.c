#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHM_STATS_MAGIC 0x56535754

static int calculate_size(size_t num_ports, size_t *size)
{
    if (num_ports == 0 ||
        num_ports > (SIZE_MAX - sizeof(shm_stats_t)) /
                    sizeof(port_stats_t)) {
        errno = EINVAL;
        return -1;
    }

    *size = sizeof(shm_stats_t) +
            num_ports * sizeof(port_stats_t);

    return 0;
}

int shm_stats_create(
    const char *name,
    size_t num_ports,
    shm_stats_t **stats,
    int *fd
)
{
    size_t size;

    if (name == NULL || stats == NULL || fd == NULL) {
        errno = EINVAL;
        return -1;
    }

    *stats = NULL;
    *fd = -1;

    if (calculate_size(num_ports, &size) != 0) {
        return -1;
    }

    int shm_fd = shm_open(
        name,
        O_CREAT | O_EXCL | O_RDWR,
        0600
    );

    if (shm_fd == -1) {
        return -1;
    }

    if (ftruncate(shm_fd, (off_t)size) == -1) {
        int saved_errno = errno;
        close(shm_fd);
        shm_unlink(name);
        errno = saved_errno;
        return -1;
    }

    shm_stats_t *mapping = mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );

    if (mapping == MAP_FAILED) {
        int saved_errno = errno;
        close(shm_fd);
        shm_unlink(name);
        errno = saved_errno;
        return -1;
    }

    memset(mapping, 0, size);

    pthread_mutexattr_t attributes;

    int result = pthread_mutexattr_init(&attributes);

    if (result != 0) {
        munmap(mapping, size);
        close(shm_fd);
        shm_unlink(name);
        errno = result;
        return -1;
    }

    result = pthread_mutexattr_setpshared(
        &attributes,
        PTHREAD_PROCESS_SHARED
    );

    if (result != 0) {
        pthread_mutexattr_destroy(&attributes);
        munmap(mapping, size);
        close(shm_fd);
        shm_unlink(name);
        errno = result;
        return -1;
    }

    result = pthread_mutex_init(
        &mapping->mutex,
        &attributes
    );

    pthread_mutexattr_destroy(&attributes);

    if (result != 0) {
        munmap(mapping, size);
        close(shm_fd);
        shm_unlink(name);
        errno = result;
        return -1;
    }

    mapping->num_ports = (uint32_t)num_ports;
    mapping->magic = SHM_STATS_MAGIC;

    *stats = mapping;
    *fd = shm_fd;

    return 0;
}

int shm_stats_attach(
    const char *name,
    shm_stats_t **stats,
    int *fd
)
{
    if (name == NULL || stats == NULL || fd == NULL) {
        errno = EINVAL;
        return -1;
    }

    *stats = NULL;
    *fd = -1;

    int shm_fd = shm_open(name, O_RDWR, 0);

    if (shm_fd == -1) {
        return -1;
    }

    struct stat information;

    if (fstat(shm_fd, &information) == -1) {
        int saved_errno = errno;
        close(shm_fd);
        errno = saved_errno;
        return -1;
    }

    if (information.st_size < (off_t)sizeof(shm_stats_t)) {
        close(shm_fd);
        errno = EINVAL;
        return -1;
    }

    size_t size = (size_t)information.st_size;

    shm_stats_t *mapping = mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );

    if (mapping == MAP_FAILED) {
        int saved_errno = errno;
        close(shm_fd);
        errno = saved_errno;
        return -1;
    }

    size_t expected_size;

    if (mapping->magic != SHM_STATS_MAGIC ||
        mapping->num_ports == 0 ||
        calculate_size(mapping->num_ports, &expected_size) != 0 ||
        expected_size != size) {

        munmap(mapping, size);
        close(shm_fd);
        errno = EINVAL;
        return -1;
    }

    *stats = mapping;
    *fd = shm_fd;

    return 0;
}

int shm_stats_update(
    shm_stats_t *stats,
    size_t port,
    uint64_t frames_in,
    uint64_t frames_out,
    uint64_t frames_dropped,
    uint64_t frames_mirrored
)
{
    if (stats == NULL || port >= stats->num_ports) {
        errno = EINVAL;
        return -1;
    }

    int result = pthread_mutex_lock(&stats->mutex);

    if (result != 0) {
        errno = result;
        return -1;
    }

    stats->ports[port].frames_in += frames_in;
    stats->ports[port].frames_out += frames_out;
    stats->ports[port].frames_dropped += frames_dropped;
    stats->ports[port].frames_mirrored += frames_mirrored;

    result = pthread_mutex_unlock(&stats->mutex);

    if (result != 0) {
        errno = result;
        return -1;
    }

    return 0;
}

int shm_stats_read(
    shm_stats_t *stats,
    size_t port,
    port_stats_t *result_stats
)
{
    if (stats == NULL ||
        result_stats == NULL ||
        port >= stats->num_ports) {

        errno = EINVAL;
        return -1;
    }

    int result = pthread_mutex_lock(&stats->mutex);

    if (result != 0) {
        errno = result;
        return -1;
    }

    *result_stats = stats->ports[port];

    result = pthread_mutex_unlock(&stats->mutex);

    if (result != 0) {
        errno = result;
        return -1;
    }

    return 0;
}

void shm_stats_detach(shm_stats_t *stats, int fd)
{
    if (stats != NULL) {
        size_t size = sizeof(shm_stats_t) +
                      stats->num_ports * sizeof(port_stats_t);

        munmap(stats, size);
    }

    if (fd >= 0) {
        close(fd);
    }
}

int shm_stats_unlink(const char *name)
{
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    return shm_unlink(name);
}
