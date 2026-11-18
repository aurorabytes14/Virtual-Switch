#define _POSIX_C_SOURCE 200809L

#include "admin_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void *admin_listener(void *argument)
{
    admin_server_t *server = argument;

    while (1) {
        pthread_mutex_lock(&server->mutex);
        int running = server->running;
        pthread_mutex_unlock(&server->mutex);

        if (!running) {
            break;
        }

        int client_fd = accept(server->server_fd, NULL, NULL);

        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            pthread_mutex_lock(&server->mutex);
            running = server->running;
            pthread_mutex_unlock(&server->mutex);

            if (!running) {
                break;
            }

            perror("admin accept");
            continue;
        }

        char command[ADMIN_COMMAND_SIZE];
        char response[ADMIN_RESPONSE_SIZE];

        ssize_t received = recv(
            client_fd,
            command,
            sizeof(command) - 1,
            0
        );

        if (received <= 0) {
            close(client_fd);
            continue;
        }

        command[received] = '\0';

        memset(response, 0, sizeof(response));

        int result = server->handler(
            command,
            response,
            sizeof(response),
            server->context
        );

        if (result != 0 && response[0] == '\0') {
            snprintf(response, sizeof(response), "ERROR\n");
        }

        size_t length = strlen(response);
        size_t sent = 0;

        while (sent < length) {
            ssize_t amount = send(
                client_fd,
                response + sent,
                length - sent,
                MSG_NOSIGNAL
            );

            if (amount == -1) {
                if (errno == EINTR) {
                    continue;
                }

                break;
            }

            if (amount == 0) {
                break;
            }

            sent += (size_t)amount;
        }

        close(client_fd);
    }

    return NULL;
}

int admin_server_start(
    admin_server_t *server,
    const char *socket_path,
    admin_command_handler_t handler,
    void *context
)
{
    if (server == NULL ||
        socket_path == NULL ||
        handler == NULL ||
        strlen(socket_path) >= sizeof(server->socket_path)) {
        errno = EINVAL;
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->server_fd = -1;

    int result = pthread_mutex_init(&server->mutex, NULL);

    if (result != 0) {
        errno = result;
        return -1;
    }

    server->handler = handler;
    server->context = context;

    snprintf(
        server->socket_path,
        sizeof(server->socket_path),
        "%s",
        socket_path
    );

    server->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (server->server_fd == -1) {
        pthread_mutex_destroy(&server->mutex);
        return -1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));

    address.sun_family = AF_UNIX;

    snprintf(
        address.sun_path,
        sizeof(address.sun_path),
        "%s",
        socket_path
    );

    unlink(socket_path);

    if (bind(
            server->server_fd,
            (struct sockaddr *)&address,
            sizeof(address)) == -1) {

        int saved_errno = errno;
        close(server->server_fd);
        pthread_mutex_destroy(&server->mutex);
        unlink(socket_path);
        errno = saved_errno;
        return -1;
    }

    if (listen(server->server_fd, 5) == -1) {
        int saved_errno = errno;
        close(server->server_fd);
        pthread_mutex_destroy(&server->mutex);
        unlink(socket_path);
        errno = saved_errno;
        return -1;
    }

    server->running = 1;

    result = pthread_create(
        &server->thread,
        NULL,
        admin_listener,
        server
    );

    if (result != 0) {
        server->running = 0;
        close(server->server_fd);
        pthread_mutex_destroy(&server->mutex);
        unlink(socket_path);
        errno = result;
        return -1;
    }

    server->started = 1;

    return 0;
}

void admin_server_stop(admin_server_t *server)
{
    if (server == NULL || !server->started) {
        return;
    }

    pthread_mutex_lock(&server->mutex);
    server->running = 0;
    pthread_mutex_unlock(&server->mutex);

    /*
     * Wake accept() so the listener thread can observe
     * the shutdown flag and exit.
     */
    int wake_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (wake_fd != -1) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));

        address.sun_family = AF_UNIX;

        snprintf(
            address.sun_path,
            sizeof(address.sun_path),
            "%s",
            server->socket_path
        );

        connect(
            wake_fd,
            (struct sockaddr *)&address,
            sizeof(address)
        );

        close(wake_fd);
    }

    pthread_join(server->thread, NULL);

    close(server->server_fd);
    unlink(server->socket_path);

    pthread_mutex_destroy(&server->mutex);

    server->server_fd = -1;
    server->started = 0;
}