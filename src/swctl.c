#define _POSIX_C_SOURCE 200809L

#include "admin_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(
            fd,
            data + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (result == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)result;
    }

    return 0;
}

static int receive_response(int fd)
{
    char buffer[1024];

    while (1) {
        ssize_t received = recv(
            fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (received == 0) {
            break;
        }

        if (received == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("recv");
            return -1;
        }

        if (fwrite(buffer, 1, (size_t)received, stdout) !=
            (size_t)received) {
            perror("fwrite");
            return -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(
            stderr,
            "Usage: ./swctl <command>\n"
            "Examples:\n"
            "  ./swctl show mac\n"
            "  ./swctl show ports\n"
            "  ./swctl clear mac\n"
        );

        return 1;
    }

    char command[ADMIN_COMMAND_SIZE];
    size_t used = 0;

    for (int i = 1; i < argc; i++) {
        size_t length = strlen(argv[i]);

        if (used + length + 2 > sizeof(command)) {
            fprintf(stderr, "Command is too long.\n");
            return 1;
        }

        if (i > 1) {
            command[used++] = ' ';
        }

        memcpy(command + used, argv[i], length);
        used += length;
    }

    command[used++] = '\n';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un address;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    snprintf(
        address.sun_path,
        sizeof(address.sun_path),
        "%s",
        ADMIN_SOCKET_PATH
    );

    if (connect(
            fd,
            (struct sockaddr *)&address,
            sizeof(address)) == -1) {

        perror("Could not connect to switch admin socket");
        close(fd);
        return 1;
    }

    if (send_all(fd, command, used) != 0) {
        perror("send");
        close(fd);
        return 1;
    }

    shutdown(fd, SHUT_WR);

    int result = receive_response(fd);

    close(fd);

    return result == 0 ? 0 : 1;
}