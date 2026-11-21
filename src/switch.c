#define _POSIX_C_SOURCE 200809L

#include "frame.h"
#include "frame_io.h"
#include "shared_frame.h"
#include "port_queue.h"
#include "mac_table.h"
#include "shm_stats.h"
#include "admin_server.h"
#include "admin_commands.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_CAPACITY 128
#define MAX_MAC_ENTRIES 1024
#define MAC_AGING_SECONDS 300
#define MAX_MACS_PER_PORT 8
#define PORT_WAIT_SECONDS 5

typedef struct switch_state switch_state_t;

typedef struct {
    switch_state_t *sw;
    int id;
    int fd;
    int active;
    int queue_initialized;
    int thread_started;
    int receiver_done;

    pthread_t receiver_thread;
    port_queue_t queue;
} switch_port_t;

struct switch_state {
    int listen_fd;
    size_t num_ports;

    switch_port_t *ports;

    pthread_mutex_t ports_mutex;
    sem_t free_ports;

    mac_table_t mac_table;

    shm_stats_t *stats;
    int stats_fd;

    admin_server_t admin_server;
    int admin_started;

    atomic_int stopping;

    sigset_t signal_set;
    pthread_t signal_thread;
    int signal_thread_started;
};

/*
 * Dedicated signal thread. All handled signals are blocked in every
 * other thread and are consumed here using sigwait().
 */
static void *signal_thread_main(void *argument)
{
    switch_state_t *sw = argument;

    for (;;) {
        int signal_number = 0;
        int result = sigwait(&sw->signal_set, &signal_number);

        if (result != 0) {
            fprintf(stderr, "sigwait: %s\n", strerror(result));
            continue;
        }

        if (signal_number == SIGUSR1) {
            fprintf(stdout, "\n--- MAC address table ---\n");
            mac_table_print(&sw->mac_table, stdout);
            continue;
        }

        if (signal_number == SIGHUP) {
            /*
             * This project version has no configuration-file parser.
             * Report the request honestly instead of pretending a reload
             * took place.
             */
            fprintf(stderr,
                    "SIGHUP received: no configuration file is configured; "
                    "reload skipped.\n");
            continue;
        }

        if (signal_number == SIGINT || signal_number == SIGTERM) {
            fprintf(stderr, "\nShutdown signal received.\n");
            atomic_store(&sw->stopping, 1);

            /*
             * shutdown() wakes the main thread if it is blocked in accept().
             * The main thread remains responsible for closing the descriptor.
             */
            if (sw->listen_fd >= 0) {
                shutdown(sw->listen_fd, SHUT_RDWR);
            }

            return NULL;
        }
    }
}

static int is_broadcast_mac(const uint8_t mac[6])
{
    static const uint8_t broadcast[6] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    return memcmp(mac, broadcast, 6) == 0;
}

static void update_stats(
    switch_state_t *sw,
    size_t port,
    uint64_t in,
    uint64_t out,
    uint64_t dropped,
    uint64_t mirrored
)
{
    if (sw->stats != NULL) {
        if (shm_stats_update(
                sw->stats,
                port,
                in,
                out,
                dropped,
                mirrored
            ) != 0) {
            perror("shm_stats_update");
        }
    }
}

/*
 * Called by the MAC table when a port exceeds
 * its maximum allowed number of learned MACs.
 *
 * The receiver thread performs final cleanup.
 */
static void shutdown_port_callback(int port_id, void *context)
{
    switch_state_t *sw = context;

    if (sw == NULL || port_id < 0 ||
        (size_t)port_id >= sw->num_ports) {
        return;
    }

    pthread_mutex_lock(&sw->ports_mutex);

    switch_port_t *port = &sw->ports[port_id];

    if (port->active && port->fd >= 0) {
        fprintf(
            stderr,
            "Port %d shut down: MAC security limit exceeded\n",
            port_id
        );

        shutdown(port->fd, SHUT_RDWR);
    }

    pthread_mutex_unlock(&sw->ports_mutex);
}

/*
 * Forward a frame to every active port except
 * the incoming port.
 */
static void flood_frame(
    switch_state_t *sw,
    int incoming_port,
    shared_frame_t *frame
)
{
    pthread_mutex_lock(&sw->ports_mutex);

    for (size_t i = 0; i < sw->num_ports; i++) {
        switch_port_t *port = &sw->ports[i];

        if (!port->active ||
            (int)i == incoming_port ||
            !port->queue_initialized) {
            continue;
        }

        int result = port_queue_enqueue(
            &port->queue,
            frame
        );

        if (result == 0) {
            update_stats(sw, i, 0, 1, 0, 0);
        } else {
            update_stats(sw, i, 0, 0, 1, 0);
        }
    }

    pthread_mutex_unlock(&sw->ports_mutex);
}

/*
 * Forward to a known destination port.
 * If the destination is unknown, flood instead.
 */
static void forward_frame(
    switch_state_t *sw,
    int incoming_port,
    shared_frame_t *frame,
    const frame_header_t *header
)
{
    int destination_port = -1;

    int found = mac_table_lookup(
        &sw->mac_table,
        header->dst,
        &destination_port
    );

    if (is_broadcast_mac(header->dst) ||
        found != 1||
        destination_port == incoming_port) {

        flood_frame(sw, incoming_port, frame);
        return;
    }

    pthread_mutex_lock(&sw->ports_mutex);

    if (destination_port >= 0 &&
        (size_t)destination_port < sw->num_ports) {

        switch_port_t *port = &sw->ports[destination_port];

        if (port->active && port->queue_initialized) {
            int result = port_queue_enqueue(
                &port->queue,
                frame
            );

            if (result == 0) {
                update_stats(
                    sw,
                    (size_t)destination_port,
                    0, 1, 0, 0
                );
            } else {
                update_stats(
                    sw,
                    (size_t)destination_port,
                    0, 0, 1, 0
                );
            }
        }
    }

    pthread_mutex_unlock(&sw->ports_mutex);
}

/*
 * Receiver thread for one connected host.
 */
static void *receiver_thread(void *argument)
{
    switch_port_t *port = argument;
    switch_state_t *sw = port->sw;

    printf("Receiver started for port %d\n", port->id);

    while (!atomic_load(&sw->stopping)) {
        uint8_t *data = NULL;
        size_t size = 0;

        int result = frame_recv(
            port->fd,
            &data,
            &size
        );

        if (result == 0) {
            printf("Host disconnected from port %d\n", port->id);
            break;
        }

        if (result < 0) {
            if (!atomic_load(&sw->stopping)) {
                fprintf(
                    stderr,
                    "Frame receive error on port %d: %s\n",
                    port->id,
                    strerror(errno)
                );
            }

            free(data);
            break;
        }

        frame_header_t header;

        if (frame_deserialize_header(data, &header) != 0) {
            fprintf(stderr, "Invalid frame on port %d\n", port->id);
            free(data);
            break;
        }

        update_stats(sw, (size_t)port->id, 1, 0, 0, 0);

        /*
         * Learn the source MAC address.
         */
        if (mac_table_insert(
                &sw->mac_table,
                header.src,
                port->id
            ) == MAC_TABLE_PORT_LIMIT) {

            free(data);
            break;
        }

        shared_frame_t *frame = shared_frame_create(
            data,
            size
        );

        free(data);

        if (frame == NULL) {
            perror("shared_frame_create");
            continue;
        }

        forward_frame(
            sw,
            port->id,
            frame,
            &header
        );

        /*
         * Release the receiver's reference.
         * Queued ports hold their own references.
         */
        shared_frame_release(frame);
    }

    /*
     * Cleanup must happen while holding ports_mutex.
     * First shut down the socket to unblock the sender.
     * Then destroy the queue and join its sender thread.
     * Finally close the socket.
     */
    pthread_mutex_lock(&sw->ports_mutex);

    /*
     * The receiver owns final cleanup. Do not make this slot
     * reusable until its thread has finished and been joined.
     */
    port->active = 0;

    if (port->fd >= 0) {
        if (shutdown(port->fd, SHUT_RDWR) != 0 && errno != ENOTCONN) {
            perror("shutdown");
        }
    }

    if (port->queue_initialized) {
        port_queue_destroy(&port->queue);
        port->queue_initialized = 0;
    }

    if (port->fd >= 0) {
        if (close(port->fd) != 0) {
            perror("close");
        }
        port->fd = -1;
    }

    port->receiver_done = 1;

    if (sem_post(&sw->free_ports) != 0) {
        perror("sem_post");
    }

    printf("Port %d released\n", port->id);

    pthread_mutex_unlock(&sw->ports_mutex);

    return NULL;
}

/*
 * Find an available port slot.
 */
/*
 * Called with ports_mutex held. A receiver sets receiver_done while
 * holding this same mutex, so joining a completed receiver here is safe:
 * it will not need ports_mutex again before returning.
 */
static void reap_finished_port_threads(switch_state_t *sw)
{
    for (size_t i = 0; i < sw->num_ports; i++) {
        switch_port_t *port = &sw->ports[i];

        if (port->thread_started && port->receiver_done) {
            int result = pthread_join(port->receiver_thread, NULL);

            if (result != 0) {
                fprintf(stderr, "pthread_join for port %zu: %s\n",
                        i, strerror(result));
            } else {
                port->thread_started = 0;
            }
        }
    }
}

/* Called with ports_mutex held, after reaping completed receiver threads. */
static int find_free_port(switch_state_t *sw)
{
    for (size_t i = 0; i < sw->num_ports; i++) {
        switch_port_t *port = &sw->ports[i];

        if (!port->active && !port->thread_started &&
            !port->queue_initialized && port->fd < 0) {
            return (int)i;
        }
    }

    return -1;
}

static int start_port(
    switch_state_t *sw,
    int client_fd,
    int port_id
)
{
    switch_port_t *port = &sw->ports[port_id];

    port->sw = sw;
    port->id = port_id;
    port->fd = client_fd;
    port->active = 1;
    port->queue_initialized = 0;
    port->thread_started = 0;
    port->receiver_done = 0;

    if (port_queue_init(
            &port->queue,
            client_fd,
            QUEUE_CAPACITY
        ) != 0) {

        fprintf(stderr, "Could not initialize port queue\n");

        port->active = 0;
        port->fd = -1;

        return -1;
    }

    port->queue_initialized = 1;
    /* Tell the host that its connection has been accepted. */
const char response[] = "OK\n";
size_t sent = 0;

while (sent < sizeof(response) - 1) {
    ssize_t result = send(
        client_fd,
        response + sent,
        sizeof(response) - 1 - sent,
        MSG_NOSIGNAL
    );

    if (result < 0) {
        if (errno == EINTR) {
            continue;
        }

        port_queue_destroy(&port->queue);
        port->queue_initialized = 0;
        port->active = 0;
        port->fd = -1;
        return -1;
    }

    if (result == 0) {
        port_queue_destroy(&port->queue);
        port->queue_initialized = 0;
        port->active = 0;
        port->fd = -1;
        return -1;
    }

    sent += (size_t)result;
}

    int result = pthread_create(
        &port->receiver_thread,
        NULL,
        receiver_thread,
        port
    );

    if (result != 0) {
        fprintf(
            stderr,
            "pthread_create: %s\n",
            strerror(result)
        );

        shutdown(client_fd, SHUT_RDWR);
        port_queue_destroy(&port->queue);
        port->queue_initialized = 0;

        port->active = 0;
        port->fd = -1;

        return -1;
    }

    port->thread_started = 1;

    printf("Host connected on switch port %d\n", port_id);

    return 0;
}

static int create_listener(uint16_t tcp_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    int enabled = 1;

    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            sizeof(enabled)
        ) < 0) {

        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(tcp_port);

    if (bind(
            fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0) {

        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 64) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void cleanup_switch(switch_state_t *sw)
{
    atomic_store(&sw->stopping, 1);

    if (sw->signal_thread_started) {
        int result = pthread_join(sw->signal_thread, NULL);

        if (result != 0) {
            fprintf(stderr, "pthread_join for signal thread: %s\n",
                    strerror(result));
        } else {
            sw->signal_thread_started = 0;
        }
    }

    if (sw->listen_fd >= 0) {
        close(sw->listen_fd);
        sw->listen_fd = -1;
    }

    if (sw->admin_started) {
        admin_server_stop(&sw->admin_server);
        sw->admin_started = 0;
    }

    pthread_mutex_lock(&sw->ports_mutex);

    for (size_t i = 0; i < sw->num_ports; i++) {
        switch_port_t *port = &sw->ports[i];

        if (port->active && port->fd >= 0) {
            if (shutdown(port->fd, SHUT_RDWR) != 0 && errno != ENOTCONN) {
                perror("shutdown");
            }
        }
    }

    pthread_mutex_unlock(&sw->ports_mutex);

    /*
     * Join receiver threads. They perform their own
     * queue and socket cleanup.
     */
    for (size_t i = 0; i < sw->num_ports; i++) {
        switch_port_t *port = &sw->ports[i];

        if (port->thread_started) {
            int result = pthread_join(port->receiver_thread, NULL);

            if (result != 0) {
                fprintf(stderr, "pthread_join for port %zu: %s\n",
                        i, strerror(result));
            } else {
                port->thread_started = 0;
            }
        }
    }

    mac_table_destroy(&sw->mac_table);

    if (sw->stats != NULL) {
        shm_stats_detach(sw->stats, sw->stats_fd);
        sw->stats = NULL;
    }

    shm_stats_unlink(SHM_STATS_NAME);

    sem_destroy(&sw->free_ports);
    pthread_mutex_destroy(&sw->ports_mutex);

    free(sw->ports);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(
            stderr,
            "Usage: ./switch <tcp_port> <num_ports>\n"
        );

        return EXIT_FAILURE;
    }

    char *end = NULL;

    long tcp_port = strtol(argv[1], &end, 10);

    if (*end != '\0' || tcp_port < 1 || tcp_port > 65535) {
        fprintf(stderr, "Invalid TCP port\n");
        return EXIT_FAILURE;
    }

    end = NULL;

    long num_ports = strtol(argv[2], &end, 10);

    if (*end != '\0' || num_ports < 1 || num_ports > 256) {
        fprintf(stderr, "Invalid number of ports\n");
        return EXIT_FAILURE;
    }

    /*
     * Block control signals before creating ANY threads. All subsequently
     * created threads inherit this mask. The dedicated signal thread will
     * receive these signals synchronously through sigwait().
     */
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);
    sigaddset(&signal_set, SIGHUP);
    sigaddset(&signal_set, SIGUSR1);

    int mask_result = pthread_sigmask(SIG_BLOCK, &signal_set, NULL);

    if (mask_result != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(mask_result));
        return EXIT_FAILURE;
    }

    switch_state_t sw;

    memset(&sw, 0, sizeof(sw));

    sw.listen_fd = -1;
    sw.stats_fd = -1;
    sw.num_ports = (size_t)num_ports;
    sw.signal_set = signal_set;

    atomic_init(&sw.stopping, 0);

    sw.ports = calloc(sw.num_ports, sizeof(*sw.ports));

    if (sw.ports == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < sw.num_ports; i++) {
        sw.ports[i].fd = -1;
    }

    if (pthread_mutex_init(&sw.ports_mutex, NULL) != 0) {
        fprintf(stderr, "Could not initialize ports mutex\n");
        free(sw.ports);
        return EXIT_FAILURE;
    }

    if (sem_init(&sw.free_ports, 0, (unsigned int)sw.num_ports) != 0) {
        perror("sem_init");
        pthread_mutex_destroy(&sw.ports_mutex);
        free(sw.ports);
        return EXIT_FAILURE;
    }

    if (mac_table_init(
            &sw.mac_table,
            MAX_MAC_ENTRIES,
            MAC_AGING_SECONDS
        ) != 0) {

        fprintf(stderr, "Could not initialize MAC table\n");
        sem_destroy(&sw.free_ports);
        pthread_mutex_destroy(&sw.ports_mutex);
        free(sw.ports);
        return EXIT_FAILURE;
    }

    if (mac_table_set_port_security(
            &sw.mac_table,
            sw.num_ports,
            MAX_MACS_PER_PORT,
            shutdown_port_callback,
            &sw
        ) != 0) {

        fprintf(stderr, "Could not configure MAC port security\n");
        mac_table_destroy(&sw.mac_table);
        sem_destroy(&sw.free_ports);
        pthread_mutex_destroy(&sw.ports_mutex);
        free(sw.ports);
        return EXIT_FAILURE;
    }

    if (shm_stats_create(
            SHM_STATS_NAME,
            sw.num_ports,
            &sw.stats,
            &sw.stats_fd
        ) != 0) {

        perror("shm_stats_create");
        mac_table_destroy(&sw.mac_table);
        sem_destroy(&sw.free_ports);
        pthread_mutex_destroy(&sw.ports_mutex);
        free(sw.ports);
        return EXIT_FAILURE;
    }

    admin_context_t admin_context = {
        .mac_table = &sw.mac_table,
        .stats = sw.stats
    };

    if (admin_server_start(
            &sw.admin_server,
            ADMIN_SOCKET_PATH,
            admin_handle_command,
            &admin_context
        ) != 0) {

        fprintf(stderr, "Could not start admin server\n");
        cleanup_switch(&sw);
        return EXIT_FAILURE;
    }

    sw.admin_started = 1;

    sw.listen_fd = create_listener((uint16_t)tcp_port);

    if (sw.listen_fd < 0) {
        perror("create_listener");
        cleanup_switch(&sw);
        return EXIT_FAILURE;
    }

    int signal_result = pthread_create(
        &sw.signal_thread,
        NULL,
        signal_thread_main,
        &sw
    );

    if (signal_result != 0) {
        fprintf(stderr, "Could not start signal thread: %s\n",
                strerror(signal_result));
        cleanup_switch(&sw);
        return EXIT_FAILURE;
    }

    sw.signal_thread_started = 1;

    printf("Software switch started\n");
    printf("TCP port: %ld\n", tcp_port);
    printf("Maximum switch ports: %ld\n", num_ports);
    printf("Admin socket: %s\n", ADMIN_SOCKET_PATH);

    while (!atomic_load(&sw.stopping)) {
        struct sockaddr_in client_address;
        socklen_t address_length = sizeof(client_address);

        int client_fd = accept(
            sw.listen_fd,
            (struct sockaddr *)&client_address,
            &address_length
        );

        if (client_fd < 0) {
            if (atomic_load(&sw.stopping) ||
                errno == EBADF || errno == EINVAL ||
                errno == ESHUTDOWN) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            continue;
        }

        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            perror("clock_gettime");
            close(client_fd);
            continue;
        }

        deadline.tv_sec += PORT_WAIT_SECONDS;

        int sem_result;

        do {
            sem_result = sem_timedwait(
                &sw.free_ports,
                &deadline
            );
        } while (sem_result != 0 && errno == EINTR &&
                 !atomic_load(&sw.stopping));

        if (sem_result != 0) {
            const char message[] = "switch full\n";

            if (errno == ETIMEDOUT) {
                size_t sent = 0;
                size_t message_length = sizeof(message) - 1;

                while (sent < message_length) {
                    ssize_t result = send(
                        client_fd,
                        message + sent,
                        message_length - sent,
                        MSG_NOSIGNAL
                    );

                    if (result < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }

                    if (result == 0) {
                        break;
                    }

                    sent += (size_t)result;
                }
            }

            shutdown(client_fd, SHUT_WR);
            close(client_fd);
            continue;
        }

        if (atomic_load(&sw.stopping)) {
            close(client_fd);
            sem_post(&sw.free_ports);
            break;
        }

        pthread_mutex_lock(&sw.ports_mutex);

        reap_finished_port_threads(&sw);
        int port_id = find_free_port(&sw);

        if (port_id < 0 ||
            start_port(&sw, client_fd, port_id) != 0) {

            pthread_mutex_unlock(&sw.ports_mutex);
            close(client_fd);
            sem_post(&sw.free_ports);
            continue;
        }

        pthread_mutex_unlock(&sw.ports_mutex);
    }

    printf("\nShutting down switch...\n");

    cleanup_switch(&sw);

    printf("Switch shutdown complete.\n");

    return EXIT_SUCCESS;
}
