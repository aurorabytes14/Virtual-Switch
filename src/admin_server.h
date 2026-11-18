#ifndef ADMIN_SERVER_H
#define ADMIN_SERVER_H

#include <pthread.h>
#include <stddef.h>

#define ADMIN_SOCKET_PATH "/tmp/vswitch.sock"
#define ADMIN_COMMAND_SIZE 256
#define ADMIN_RESPONSE_SIZE 4096

typedef int (*admin_command_handler_t)(
    const char *command,
    char *response,
    size_t response_size,
    void *context
);

typedef struct {
    int server_fd;
    int running;
    int started;

    char socket_path[108];

    pthread_t thread;
    pthread_mutex_t mutex;

    admin_command_handler_t handler;
    void *context;
} admin_server_t;

int admin_server_start(
    admin_server_t *server,
    const char *socket_path,
    admin_command_handler_t handler,
    void *context
);

void admin_server_stop(admin_server_t *server);

#endif