#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_ITERATIONS 10000

int main(void)
{
    const char *name = "/virtual_switch_stats_test";

    shm_stats_t *stats = NULL;
    int fd = -1;

    /* Remove an old test segment, if one exists. */
    shm_stats_unlink(name);

    printf("--- SHARED MEMORY TEST ---\n");

    /* Parent creates the shared memory. */
    if (shm_stats_create(name, 2, &stats, &fd) != 0) {
        perror("shm_stats_create");
        return 1;
    }

    printf("Parent created shared memory.\n");

    pid_t child = fork();

    if (child == -1) {
        perror("fork");
        shm_stats_detach(stats, fd);
        shm_stats_unlink(name);
        return 1;
    }

    if (child == 0) {
        /* Child attaches to the existing shared memory. */
        shm_stats_t *child_stats = NULL;
        int child_fd = -1;

        if (shm_stats_attach(name, &child_stats, &child_fd) != 0) {
            perror("Child: shm_stats_attach");
            _exit(1);
        }

        printf("Child attached to shared memory.\n");

        /* Child updates port 0 repeatedly. */
        for (int i = 0; i < TEST_ITERATIONS; i++) {
            if (shm_stats_update(
                    child_stats, 0, 1, 0, 0, 0) != 0) {

                perror("Child: shm_stats_update");
                shm_stats_detach(child_stats, child_fd);
                _exit(1);
            }
        }

        shm_stats_detach(child_stats, child_fd);
        _exit(0);
    }

    /* Parent updates the same counter concurrently. */
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        if (shm_stats_update(stats, 0, 1, 0, 0, 0) != 0) {
            perror("Parent: shm_stats_update");

            waitpid(child, NULL, 0);
            shm_stats_detach(stats, fd);
            shm_stats_unlink(name);

            return 1;
        }
    }

    int status;

    if (waitpid(child, &status, 0) == -1) {
        perror("waitpid");
        shm_stats_detach(stats, fd);
        shm_stats_unlink(name);
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("Child process failed.\n");
        shm_stats_detach(stats, fd);
        shm_stats_unlink(name);
        return 1;
    }

    /* Read the final counter. */
    port_stats_t result;

    if (shm_stats_read(stats, 0, &result) != 0) {
        perror("shm_stats_read");
        shm_stats_detach(stats, fd);
        shm_stats_unlink(name);
        return 1;
    }

    uint64_t expected = 2ULL * TEST_ITERATIONS;

    printf("\n--- RESULTS ---\n");
    printf("Expected frames_in: %llu\n",
        (unsigned long long)expected);

    printf("Actual frames_in:   %llu\n",
        (unsigned long long)result.frames_in);

    if (result.frames_in != expected) {
        printf("SHARED MEMORY TEST FAILED\n");

        shm_stats_detach(stats, fd);
        shm_stats_unlink(name);

        return 1;
    }

    printf("\nSHARED MEMORY TEST PASSED\n");
    printf("Both processes updated the same counter safely.\n");

    shm_stats_detach(stats, fd);

    if (shm_stats_unlink(name) != 0) {
        perror("shm_stats_unlink");
        return 1;
    }

    printf("Shared memory cleaned up successfully.\n");

    return 0;
}
