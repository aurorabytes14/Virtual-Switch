#include "frame.h"
#include "frame_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int socket_fd;
    volatile sig_atomic_t running;
} host_context_t;

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s <switch_ip> <tcp_port> <mac>\n", program);
}

static int parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int values[6];

    char extra;

    if (sscanf(
            text,
            "%2x:%2x:%2x:%2x:%2x:%2x%c",
            &values[0], &values[1], &values[2],
            &values[3], &values[4], &values[5],
            &extra
        ) != 6) {
        return -1;
    }

    for (int i = 0; i < 6; i++) {
        if (values[i] > 255) {
            return -1;
        }

        mac[i] = (uint8_t)values[i];
    }

    return 0;
}

static void print_mac(const uint8_t mac[6])
{
    printf(
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]
    );
}

/*
 * Read the switch's connection response.
 *
 * "OK\n"           means accepted.
 * "switch full\n"  means rejected.
 */
static int receive_connection_response(int socket_fd)
{
    char response[32];
    size_t received = 0;

    while (received < sizeof(response) - 1) {
        ssize_t result = recv(
            socket_fd,
            &response[received],
            1,
            0
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (result == 0) {
            return -1;
        }

        if (response[received] == '\n') {
            received++;
            response[received] = '\0';
            break;
        }

        received++;
    }

    if (received == 0 || response[received - 1] != '\n') {
        return -1;
    }

    if (strcmp(response, "OK\n") == 0) {
        return 1;
    }

    if (strcmp(response, "switch full\n") == 0) {
        return 0;
    }

    return -1;
}

/*
 * Receive frames from the switch.
 */
static void *receiver_thread(void *argument)
{
    host_context_t *context = argument;

    printf("Listening for incoming frames...\n");

    while (context->running) {
        uint8_t *frame = NULL;
        size_t frame_size = 0;

        int result = frame_recv(
            context->socket_fd,
            &frame,
            &frame_size
        );

        if (result == 0) {
            printf("\nSwitch disconnected.\n");
            break;
        }

        if (result < 0) {
            if (context->running) {
                fprintf(
                    stderr,
                    "\nReceive error: %s\n",
                    strerror(errno)
                );
            }

            break;
        }

        if (frame_size < FRAME_HEADER_SIZE) {
            fprintf(stderr, "Received invalid frame.\n");
            free(frame);
            continue;
        }

        frame_header_t header;

        if (frame_deserialize_header(frame, &header) != 0) {
            fprintf(stderr, "Could not decode frame header.\n");
            free(frame);
            continue;
        }

        printf("\nReceived frame:\n");

        printf("Source MAC:      ");
        print_mac(header.src);
        printf("\n");

        printf("Destination MAC: ");
        print_mac(header.dst);
        printf("\n");

        printf("VLAN:            %u\n", header.vlan);
        printf("Priority:        %u\n", header.priority);
        printf("Payload length:  %u\n", header.length);

        printf("Payload:         ");

        size_t available_payload = frame_size - FRAME_HEADER_SIZE;
        size_t payload_length = header.length;

        if (payload_length > available_payload) {
            payload_length = available_payload;
        }

        for (size_t i = 0; i < payload_length; i++) {
            putchar(frame[FRAME_HEADER_SIZE + i]);
        }

        printf("\n\n");

        fflush(stdout);
        free(frame);
    }

    context->running = 0;

    return NULL;
}

/*
 * Construct and send a serialized frame.
 */
static int send_user_frame(
    int socket_fd,
    const uint8_t source_mac[6],
    const uint8_t destination_mac[6],
    const char *message
)
{
    size_t payload_length = strlen(message);

    if (payload_length > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "Payload exceeds 1500 bytes.\n");
        return -1;
    }

    frame_header_t header;

    memset(&header, 0, sizeof(header));

    header.length = (uint16_t)payload_length;
    memcpy(header.src, source_mac, 6);
    memcpy(header.dst, destination_mac, 6);

    header.vlan = 1;
    header.priority = 1;

    size_t frame_size = FRAME_HEADER_SIZE + payload_length;

    uint8_t *frame = malloc(frame_size);

    if (frame == NULL) {
        perror("malloc");
        return -1;
    }

    if (frame_serialize_header(&header, frame) != 0) {
        fprintf(stderr, "Could not serialize frame header.\n");
        free(frame);
        return -1;
    }

    memcpy(
        frame + FRAME_HEADER_SIZE,
        message,
        payload_length
    );

    int result = frame_send(
        socket_fd,
        frame,
        frame_size
    );

    free(frame);

    if (result != 0) {
        fprintf(
            stderr,
            "Failed to send frame: %s\n",
            strerror(errno)
        );

        return -1;
    }

    printf("Frame sent successfully.\n");

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *switch_ip = argv[1];

    char *end_pointer = NULL;

    long port_number = strtol(argv[2], &end_pointer, 10);

    if (
        end_pointer == argv[2] ||
        *end_pointer != '\0' ||
        port_number < 1 ||
        port_number > 65535
    ) {
        fprintf(stderr, "Invalid TCP port.\n");
        return EXIT_FAILURE;
    }

    uint8_t source_mac[6];

    if (parse_mac(argv[3], source_mac) != 0) {
        fprintf(stderr, "Invalid MAC address.\n");
        return EXIT_FAILURE;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in switch_address;

    memset(&switch_address, 0, sizeof(switch_address));

    switch_address.sin_family = AF_INET;
    switch_address.sin_port = htons((uint16_t)port_number);

    if (inet_pton(
            AF_INET,
            switch_ip,
            &switch_address.sin_addr
        ) != 1) {

        fprintf(stderr, "Invalid switch IP address.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    printf("Connecting to switch...\n");

    if (connect(
            socket_fd,
            (struct sockaddr *)&switch_address,
            sizeof(switch_address)
        ) != 0) {

        perror("connect");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    int connection_status =
        receive_connection_response(socket_fd);

    if (connection_status == 0) {
        printf("Connection rejected: switch is full.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    if (connection_status < 0) {
        fprintf(stderr, "Invalid response from switch.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    printf("Connected to switch successfully.\n");

    printf("Your MAC address: ");
    print_mac(source_mac);
    printf("\n");

    host_context_t context;

    context.socket_fd = socket_fd;
    context.running = 1;

    pthread_t receiver;

    int thread_result = pthread_create(
        &receiver,
        NULL,
        receiver_thread,
        &context
    );

    if (thread_result != 0) {
        fprintf(
            stderr,
            "pthread_create: %s\n",
            strerror(thread_result)
        );

        close(socket_fd);
        return EXIT_FAILURE;
    }

    printf("\nAvailable commands:\n");
    printf("  send <destination_mac> <message>\n");
    printf("  help\n");
    printf("  quit\n\n");

    char input[2048];

    while (context.running) {
        printf("> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, "quit") == 0) {
            break;
        }

        if (strcmp(input, "help") == 0) {
            printf("\nAvailable commands:\n");
            printf("  send <destination_mac> <message>\n");
            printf("  help\n");
            printf("  quit\n\n");

            continue;
        }

        if (strncmp(input, "send ", 5) == 0) {
            char *destination_text = input + 5;
            char *message = strchr(destination_text, ' ');

            if (message == NULL) {
                printf(
                    "Usage: send <destination_mac> <message>\n"
                );
                continue;
            }

            *message = '\0';
            message++;

            uint8_t destination_mac[6];

            if (parse_mac(
                    destination_text,
                    destination_mac
                ) != 0) {

                printf("Invalid destination MAC address.\n");
                continue;
            }

            if (*message == '\0') {
                printf("Message cannot be empty.\n");
                continue;
            }

            send_user_frame(
                socket_fd,
                source_mac,
                destination_mac,
                message
            );

            continue;
        }

        if (input[0] != '\0') {
            printf("Unknown command. Type 'help' for commands.\n");
        }
    }

    context.running = 0;

    shutdown(socket_fd, SHUT_RDWR);

    pthread_join(receiver, NULL);

    close(socket_fd);

    printf("Host disconnected.\n");

    return EXIT_SUCCESS;
}