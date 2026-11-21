#define _POSIX_C_SOURCE 200809L

#include "frame.h"
#include "frame_io.h"
#include "shared_frame.h"
#include "port_queue.h"
#include "mac_table.h"
#include "shm_stats.h"
#include "admin_server.h"
#include "admin_commands.h"
#include "mirror_queue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_CAPACITY 128
#define MAX_MAC_ENTRIES 1024
#define MAC_AGING_SECONDS 300
#define MAX_MACS_PER_PORT 8
#define PORT_WAIT_SECONDS 5
#define MIRROR_QUEUE_CAPACITY 256
#define SNIFFER_SOCKET_PATH "/tmp/vswitch-sniffer.sock"

typedef struct switch_state switch_state_t;

typedef struct {
    switch_state_t *sw;
    int id;
    int fd;
    int active;
    int queue_initialized;
    int thread_started;
    int receiver_done;

    /* VLAN configuration */
    uint16_t access_vlan;
    int is_trunk;
    int mirror_enabled;

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

    /* Dedicated, independent mirror path for one sniffer client. */
    int sniffer_listen_fd;
    int sniffer_fd;
    pthread_t sniffer_thread;
    int sniffer_thread_started;
    pthread_mutex_t mirror_mutex;
    int mirror_mutex_initialized;
    mirror_queue_t mirror_queue;
    int mirror_queue_initialized;
    int sniffer_connected;
};

/* Create the UNIX-domain listener used only by the packet sniffer. */
static void update_stats(
    switch_state_t *sw,
    size_t port,
    uint64_t in,
    uint64_t out,
    uint64_t dropped,
    uint64_t mirrored
);

static int create_sniffer_listener(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    if (strlen(SNIFFER_SOCKET_PATH) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(address.sun_path, SNIFFER_SOCKET_PATH);
    (void)unlink(SNIFFER_SOCKET_PATH);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        int saved_errno = errno;
        close(fd);
        (void)unlink(SNIFFER_SOCKET_PATH);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

/*
 * A sniffer is deliberately separate from the host TCP listener and does
 * not consume a switch port. The queue's sender thread is the only thread
 * that writes mirrored frames to the sniffer.
 */
static void *sniffer_thread_main(void *argument)
{
    switch_state_t *sw = argument;

    while (!atomic_load(&sw->stopping)) {
        struct pollfd listener_poll;
        listener_poll.fd = sw->sniffer_listen_fd;
        listener_poll.events = POLLIN;
        listener_poll.revents = 0;

        int ready = poll(&listener_poll, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!atomic_load(&sw->stopping)) {
                perror("poll sniffer listener");
            }
            break;
        }
        if (ready == 0 || !(listener_poll.revents & POLLIN)) {
            continue;
        }

        int client_fd = accept(sw->sniffer_listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || atomic_load(&sw->stopping)) {
                continue;
            }
            perror("accept sniffer");
            continue;
        }

        pthread_mutex_lock(&sw->mirror_mutex);
        int init_result = mirror_queue_init(
            &sw->mirror_queue, client_fd, MIRROR_QUEUE_CAPACITY);
        if (init_result == 0) {
            sw->sniffer_fd = client_fd;
            sw->mirror_queue_initialized = 1;
            sw->sniffer_connected = 1;
            fprintf(stdout, "Sniffer connected on %s\n", SNIFFER_SOCKET_PATH);
        }
        pthread_mutex_unlock(&sw->mirror_mutex);

        if (init_result != 0) {
            perror("mirror_queue_init");
            close(client_fd);
            continue;
        }

        /* Detect a sniffer disconnect without reading its frame stream. */
        while (!atomic_load(&sw->stopping)) {
            struct pollfd client_poll;
            client_poll.fd = client_fd;
            client_poll.events = POLLIN;
            client_poll.revents = 0;

            ready = poll(&client_poll, 1, 250);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;
            }
            if (client_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                break;
            }
            if (client_poll.revents & POLLIN) {
                char byte;
                ssize_t peeked = recv(client_fd, &byte, 1,
                                      MSG_PEEK | MSG_DONTWAIT);
                if (peeked <= 0) {
                    break;
                }
                /* Sniffer connections are receive-only. */
                break;
            }
        }

        pthread_mutex_lock(&sw->mirror_mutex);
        sw->sniffer_connected = 0;
        if (sw->mirror_queue_initialized) {
            /* destroy() shuts down the socket before joining its sender. */
            mirror_queue_destroy(&sw->mirror_queue);
            sw->mirror_queue_initialized = 0;
        }
        sw->sniffer_fd = -1;
        pthread_mutex_unlock(&sw->mirror_mutex);

        close(client_fd);
        fprintf(stdout, "Sniffer disconnected\n");
    }

    return NULL;
}

/* Caller may hold ports_mutex; this helper never acquires it. */
static void mirror_frame(switch_state_t *sw, size_t port_id,
                         shared_frame_t *frame)
{
    int queued = 0;

    pthread_mutex_lock(&sw->mirror_mutex);
    if (sw->sniffer_connected && sw->mirror_queue_initialized) {
        if (mirror_queue_enqueue(&sw->mirror_queue, frame) == 0) {
            queued = 1;
        }
    }
    pthread_mutex_unlock(&sw->mirror_mutex);

    if (queued) {
        update_stats(sw, port_id, 0, 0, 0, 1);
    }
}

static int set_mirror_callback(void *context, size_t port_id, int enabled)
{
    switch_state_t *sw = context;
    if (sw == NULL || port_id >= sw->num_ports) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&sw->ports_mutex);
    sw->ports[port_id].mirror_enabled = enabled != 0;
    pthread_mutex_unlock(&sw->ports_mutex);
    return 0;
}

static int show_mirror_callback(void *context, char *response,
                                size_t response_size)
{
    switch_state_t *sw = context;
    if (sw == NULL || response == NULL || response_size == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t used = 0;
    pthread_mutex_lock(&sw->ports_mutex);
    pthread_mutex_lock(&sw->mirror_mutex);

    int written = snprintf(response, response_size,
                           "Sniffer: %s\n",
                           sw->sniffer_connected ? "connected" : "disconnected");
    if (written < 0 || (size_t)written >= response_size) {
        pthread_mutex_unlock(&sw->mirror_mutex);
        pthread_mutex_unlock(&sw->ports_mutex);
        errno = ENOSPC;
        return -1;
    }
    used = (size_t)written;

    size_t queued = 0, dropped = 0, sent = 0, failed = 0;
    if (sw->mirror_queue_initialized) {
        pthread_mutex_lock(&sw->mirror_queue.mutex);
        queued = sw->mirror_queue.count;
        dropped = sw->mirror_queue.dropped_frames;
        sent = sw->mirror_queue.sent_frames;
        failed = sw->mirror_queue.failed_frames;
        pthread_mutex_unlock(&sw->mirror_queue.mutex);
    }

    written = snprintf(response + used, response_size - used,
                       "Queue: %zu | sent: %zu | dropped: %zu | failed: %zu\n",
                       queued, sent, dropped, failed);
    if (written < 0 || (size_t)written >= response_size - used) {
        pthread_mutex_unlock(&sw->mirror_mutex);
        pthread_mutex_unlock(&sw->ports_mutex);
        errno = ENOSPC;
        return -1;
    }
    used += (size_t)written;

    for (size_t i = 0; i < sw->num_ports; i++) {
        written = snprintf(response + used, response_size - used,
                           "Port %zu: %s\n", i,
                           sw->ports[i].mirror_enabled ? "mirroring on" : "mirroring off");
        if (written < 0 || (size_t)written >= response_size - used) {
            pthread_mutex_unlock(&sw->mirror_mutex);
            pthread_mutex_unlock(&sw->ports_mutex);
            errno = ENOSPC;
            return -1;
        }
        used += (size_t)written;
    }

    pthread_mutex_unlock(&sw->mirror_mutex);
    pthread_mutex_unlock(&sw->ports_mutex);
    return 0;
}

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
 * Access ports accept only their configured VLAN.
 * Trunk ports carry every valid VLAN.
 */
static int port_accepts_vlan(const switch_port_t *port, uint16_t vlan)
{
    return port->is_trunk || port->access_vlan == vlan;
}

/*
 * Flood a frame only to active ports that carry its VLAN.
 * The frame's VLAN field is preserved, including on trunk egress.
 */
static void flood_frame(
    switch_state_t *sw,
    int incoming_port,
    shared_frame_t *frame,
    uint16_t vlan
)
{
    pthread_mutex_lock(&sw->ports_mutex);

    for (size_t i = 0; i < sw->num_ports; i++) {
        switch_port_t *port = &sw->ports[i];

        if (!port->active ||
            (int)i == incoming_port ||
            !port->queue_initialized ||
            !port_accepts_vlan(port, vlan)) {
            continue;
        }

        int result = port_queue_enqueue(&port->queue, frame);

        if (result == 0) {
            update_stats(sw, i, 0, 1, 0, 0);
            if (port->mirror_enabled) {
                mirror_frame(sw, i, frame);
            }
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

    int found = mac_table_lookup_vlan(
        &sw->mac_table,
        header->vlan,
        header->dst,
        &destination_port
    );

    if (is_broadcast_mac(header->dst) || found != 1) {
        flood_frame(sw, incoming_port, frame, header->vlan);
        return;
    }

    /*
     * The destination is already reachable through the ingress port.
     * Filter the frame instead of unnecessarily flooding it.
     */
    if (destination_port == incoming_port) {
        return;
    }

    pthread_mutex_lock(&sw->ports_mutex);

    if (destination_port >= 0 &&
        (size_t)destination_port < sw->num_ports) {

        switch_port_t *port = &sw->ports[destination_port];

        if (port->active &&
            port->queue_initialized &&
            port_accepts_vlan(port, header->vlan)) {
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
                if (port->mirror_enabled) {
                    mirror_frame(sw, (size_t)destination_port, frame);
                }
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
         * Snapshot this port's VLAN mode while holding ports_mutex.
         * Access ports classify all ingress traffic into their configured
         * VLAN. Trunk ports retain the VLAN carried in the frame.
         */
        pthread_mutex_lock(&sw->ports_mutex);
        int is_trunk = port->is_trunk;
        int mirror_enabled = port->mirror_enabled;
        uint16_t access_vlan = port->access_vlan;
        pthread_mutex_unlock(&sw->ports_mutex);

        if (!is_trunk) {
            header.vlan = access_vlan;

            uint8_t serialized_header[FRAME_HEADER_SIZE];
            if (frame_serialize_header(&header, serialized_header) != 0) {
                fprintf(stderr, "Could not serialize VLAN header on port %d\n",
                        port->id);
                free(data);
                continue;
            }

            memcpy(data, serialized_header, FRAME_HEADER_SIZE);
        }

        /*
         * Learn the source MAC using both VLAN and MAC address.
         */
        int learn_result = mac_table_insert_vlan(
            &sw->mac_table,
            header.vlan,
            header.src,
            port->id
        );

        if (learn_result == MAC_TABLE_PORT_LIMIT) {
            free(data);
            break;
        }

        if (learn_result < 0) {
            fprintf(stderr, "Could not learn source MAC on port %d: %s\n",
                    port->id, strerror(errno));
            free(data);
            continue;
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

        /* Mirror ingress after access-VLAN classification. */
        if (mirror_enabled) {
            mirror_frame(sw, (size_t)port->id, frame);
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

/*
 * Optional port configuration is selected by port-slot index:
 *
 *   SWITCH_PORT_VLANS=10,10,20
 *       slots 0 and 1 are access VLAN 10; slot 2 is access VLAN 20.
 *
 *   SWITCH_TRUNK_PORT=2
 *       slot 2 is a trunk and carries all VLANs.
 *
 * Unspecified access VLANs default to VLAN 1. Configuration is applied
 * before accepting hosts, so a slot keeps its configuration across
 * disconnect/reconnect cycles.
 */
static int configure_vlan_ports(switch_state_t *sw)
{
    for (size_t i = 0; i < sw->num_ports; i++) {
        sw->ports[i].access_vlan = 1;
        sw->ports[i].is_trunk = 0;
    }

    const char *vlan_list = getenv("SWITCH_PORT_VLANS");
    if (vlan_list != NULL && *vlan_list != '\0') {
        char *copy = strdup(vlan_list);
        if (copy == NULL) {
            return -1;
        }

        char *saveptr = NULL;
        char *token = strtok_r(copy, ",", &saveptr);
        size_t index = 0;

        while (token != NULL && index < sw->num_ports) {
            char *end = NULL;
            long vlan = strtol(token, &end, 10);

            if (end == token || *end != '\0' || vlan < 1 || vlan > 4094) {
                fprintf(stderr, "Invalid VLAN in SWITCH_PORT_VLANS: %s\n",
                        token);
                free(copy);
                return -1;
            }

            sw->ports[index].access_vlan = (uint16_t)vlan;
            index++;
            token = strtok_r(NULL, ",", &saveptr);
        }

        if (token != NULL) {
            fprintf(stderr, "Too many entries in SWITCH_PORT_VLANS\n");
            free(copy);
            return -1;
        }

        free(copy);
    }

    const char *trunk_text = getenv("SWITCH_TRUNK_PORT");
    if (trunk_text != NULL && *trunk_text != '\0') {
        char *end = NULL;
        long trunk_port = strtol(trunk_text, &end, 10);

        if (end == trunk_text || *end != '\0' ||
            trunk_port < 0 || (size_t)trunk_port >= sw->num_ports) {
            fprintf(stderr, "Invalid SWITCH_TRUNK_PORT value\n");
            return -1;
        }

        sw->ports[trunk_port].is_trunk = 1;
        fprintf(stdout, "Port slot %ld configured as VLAN trunk\n",
                trunk_port);
    }

    for (size_t i = 0; i < sw->num_ports; i++) {
        if (!sw->ports[i].is_trunk) {
            fprintf(stdout, "Port slot %zu configured as access VLAN %u\n",
                    i, (unsigned int)sw->ports[i].access_vlan);
        }
    }

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
    int was_stopping = atomic_exchange(&sw->stopping, 1);

    if (sw->signal_thread_started) {

        /*
         * If cleanup was not triggered by a shutdown signal,
         * wake the signal thread if it is still blocked in sigwait().
         */
        if (!was_stopping) {
            int wake_result = pthread_kill(
                sw->signal_thread,
                SIGTERM
            );

            if (wake_result != 0) {
                fprintf(stderr,
                        "pthread_kill for signal thread: %s\n",
                        strerror(wake_result));
            }
        }

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

    /* The sniffer thread notices stopping within its 250 ms poll interval. */
    if (sw->sniffer_thread_started) {
        int result = pthread_join(sw->sniffer_thread, NULL);
        if (result != 0) {
            fprintf(stderr, "pthread_join for sniffer thread: %s\n",
                    strerror(result));
        } else {
            sw->sniffer_thread_started = 0;
        }
    }

    if (sw->sniffer_listen_fd >= 0) {
        close(sw->sniffer_listen_fd);
        sw->sniffer_listen_fd = -1;
    }
    (void)unlink(SNIFFER_SOCKET_PATH);

    if (sw->mirror_mutex_initialized) {
        pthread_mutex_destroy(&sw->mirror_mutex);
        sw->mirror_mutex_initialized = 0;
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
    sw.sniffer_listen_fd = -1;
    sw.sniffer_fd = -1;
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

    if (configure_vlan_ports(&sw) != 0) {
        free(sw.ports);
        return EXIT_FAILURE;
    }

    if (pthread_mutex_init(&sw.ports_mutex, NULL) != 0) {
        fprintf(stderr, "Could not initialize ports mutex\n");
        free(sw.ports);
        return EXIT_FAILURE;
    }

    int mirror_mutex_result = pthread_mutex_init(&sw.mirror_mutex, NULL);
    if (mirror_mutex_result != 0) {
        fprintf(stderr, "Could not initialize mirror mutex: %s\n",
                strerror(mirror_mutex_result));
        pthread_mutex_destroy(&sw.ports_mutex);
        free(sw.ports);
        return EXIT_FAILURE;
    }
    sw.mirror_mutex_initialized = 1;

    if (sem_init(&sw.free_ports, 0, (unsigned int)sw.num_ports) != 0) {
        perror("sem_init");
        pthread_mutex_destroy(&sw.mirror_mutex);
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
        pthread_mutex_destroy(&sw.mirror_mutex);
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
        pthread_mutex_destroy(&sw.mirror_mutex);
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
        pthread_mutex_destroy(&sw.mirror_mutex);
        pthread_mutex_destroy(&sw.ports_mutex);
        free(sw.ports);
        return EXIT_FAILURE;
    }

    sw.sniffer_listen_fd = create_sniffer_listener();
    if (sw.sniffer_listen_fd < 0) {
        perror("create_sniffer_listener");
        cleanup_switch(&sw);
        return EXIT_FAILURE;
    }

    int sniffer_result = pthread_create(
        &sw.sniffer_thread, NULL, sniffer_thread_main, &sw);
    if (sniffer_result != 0) {
        fprintf(stderr, "Could not start sniffer thread: %s\n",
                strerror(sniffer_result));
        cleanup_switch(&sw);
        return EXIT_FAILURE;
    }
    sw.sniffer_thread_started = 1;

    admin_context_t admin_context = {
        .mac_table = &sw.mac_table,
        .stats = sw.stats,
        .mirror_context = &sw,
        .num_ports = sw.num_ports,
        .set_mirror = set_mirror_callback,
        .show_mirror = show_mirror_callback
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
    printf("Sniffer socket: %s\n", SNIFFER_SOCKET_PATH);

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
