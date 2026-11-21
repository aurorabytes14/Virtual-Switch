#include "frame.h"
#include "frame_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int socket_fd;
    atomic_int running;
} host_context_t;

static void print_usage(const char *program)
{
    fprintf(
        stderr,
        "Usage: %s <switch_ip> <tcp_port> <mac> [vlan]\n",
        program
    );
}

/*
 * Parse a MAC address in the exact format:
 * AA:BB:CC:DD:EE:FF
 */
static int parse_mac(const char *text, uint8_t mac[6])
{
    if (text == NULL || strlen(text) != 17) {
        return -1;
    }

    for (int i = 0; i < 6; i++) {
        int position = i * 3;

        if (
            !(
                (text[position] >= '0' && text[position] <= '9') ||
                (text[position] >= 'a' && text[position] <= 'f') ||
                (text[position] >= 'A' && text[position] <= 'F')
            )
        ) {
            return -1;
        }

        if (
            !(
                (text[position + 1] >= '0' &&
                 text[position + 1] <= '9') ||
                (text[position + 1] >= 'a' &&
                 text[position + 1] <= 'f') ||
                (text[position + 1] >= 'A' &&
                 text[position + 1] <= 'F')
            )
        ) {
            return -1;
        }

        if (i < 5 && text[position + 2] != ':') {
            return -1;
        }

        char byte_text[3] = {
            text[position],
            text[position + 1],
            '\0'
        };

        char *end = NULL;
        unsigned long value = strtoul(byte_text, &end, 16);

        if (end == byte_text || *end != '\0' || value > 255) {
            return -1;
        }

        mac[i] = (uint8_t)value;
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
 * "OK\n"          means accepted.
 * "switch full\n" means rejected.
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

    while (atomic_load(&context->running)) {
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
            if (atomic_load(&context->running)) {
                fprintf(
                    stderr,
                    "\nReceive error: %s\n",
                    strerror(errno)
                );
            }

            free(frame);
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

        for (size_t i = 0; i < header.length; i++) {
            putchar(frame[FRAME_HEADER_SIZE + i]);
        }

        printf("\n\n");
        fflush(stdout);

        free(frame);
    }

    atomic_store(&context->running, 0);

    return NULL;
}

/*
 * Construct and send a serialized frame.
 */
static int send_user_frame(
    int socket_fd,
    const uint8_t source_mac[6],
    const uint8_t destination_mac[6],
    const char *message,
    uint16_t vlan
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

    header.vlan = vlan;
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

    printf("Frame sent successfully (VLAN %u).\n", vlan);

    return 0;
}

static void print_commands(void)
{
    printf("\nAvailable commands:\n");
    printf("  send <destination_mac> <message>\n");
    printf("  help\n");
    printf("  quit\n\n");
}

int main(int argc, char *argv[])
{
    if (argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *switch_ip = argv[1];

    /*
     * Parse TCP port.
     */
    errno = 0;

    char *end_pointer = NULL;

    long port_number = strtol(
        argv[2],
        &end_pointer,
        10
    );

    if (
        errno != 0 ||
        end_pointer == argv[2] ||
        *end_pointer != '\0' ||
        port_number < 1 ||
        port_number > 65535
    ) {
        fprintf(stderr, "Invalid TCP port.\n");
        return EXIT_FAILURE;
    }

    /*
     * Parse source MAC address.
     */
    uint8_t source_mac[6];

    if (parse_mac(argv[3], source_mac) != 0) {
        fprintf(stderr, "Invalid MAC address.\n");
        return EXIT_FAILURE;
    }

    /*
     * Parse VLAN.
     * Default VLAN: 1.
     */
    uint16_t vlan = 1;

    if (argc == 5) {
        errno = 0;
        end_pointer = NULL;

        long vlan_number = strtol(
            argv[4],
            &end_pointer,
            10
        );

        if (
            errno != 0 ||
            end_pointer == argv[4] ||
            *end_pointer != '\0' ||
            vlan_number < 1 ||
            vlan_number > 4094
        ) {
            fprintf(
                stderr,
                "Invalid VLAN ID. Use a value between 1 and 4094.\n"
            );

            return EXIT_FAILURE;
        }

        vlan = (uint16_t)vlan_number;
    }

    /*
     * Create TCP socket.
     */
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in switch_address;

    memset(&switch_address, 0, sizeof(switch_address));

    switch_address.sin_family = AF_INET;
    switch_address.sin_port = htons((uint16_t)port_number);

    if (
        inet_pton(
            AF_INET,
            switch_ip,
            &switch_address.sin_addr
        ) != 1
    ) {
        fprintf(stderr, "Invalid switch IP address.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    /*
     * Connect to switch.
     */
    printf("Connecting to switch...\n");

    if (
        connect(
            socket_fd,
            (struct sockaddr *)&switch_address,
            sizeof(switch_address)
        ) != 0
    ) {
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

    printf("Your VLAN: %u\n", vlan);

    /*
     * Initialize shared thread context.
     */
    host_context_t context;

    context.socket_fd = socket_fd;
    atomic_init(&context.running, 1);

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

    print_commands();

    /*
     * Interactive command loop.
     *
     * poll() uses a short timeout so the main thread can
     * notice when the receiver thread detects disconnection.
     */
    char input[2048];

    while (atomic_load(&context.running)) {
        struct pollfd input_poll;

        input_poll.fd = STDIN_FILENO;
        input_poll.events = POLLIN;
        input_poll.revents = 0;

        printf("> ");
        fflush(stdout);

        int poll_result = poll(&input_poll, 1, 250);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll");
            break;
        }

        if (poll_result == 0) {
            continue;
        }

        if (input_poll.revents & (POLLERR | POLLNVAL)) {
            fprintf(stderr, "Input error.\n");
            break;
        }

        if (input_poll.revents & (POLLIN | POLLHUP)) {
            if (fgets(input, sizeof(input), stdin) == NULL) {
                break;
            }

            input[strcspn(input, "\n")] = '\0';

            if (strcmp(input, "quit") == 0) {
                break;
            }

            if (strcmp(input, "help") == 0) {
                print_commands();
                continue;
            }

            if (strncmp(input, "send ", 5) == 0) {
                char *destination_text = input + 5;

                char *message = strchr(
                    destination_text,
                    ' '
                );

                if (message == NULL) {
                    printf(
                        "Usage: send <destination_mac> <message>\n"
                    );

                    continue;
                }

                *message = '\0';
                message++;

                uint8_t destination_mac[6];

                if (
                    parse_mac(
                        destination_text,
                        destination_mac
                    ) != 0
                ) {
                    printf("Invalid destination MAC address.\n");
                    continue;
                }

                if (*message == '\0') {
                    printf("Message cannot be empty.\n");
                    continue;
                }

                if (!atomic_load(&context.running)) {
                    break;
                }

                send_user_frame(
                    socket_fd,
                    source_mac,
                    destination_mac,
                    message,
                    vlan
                );

                continue;
            }

            if (input[0] != '\0') {
                printf(
                    "Unknown command. Type 'help' for commands.\n"
                );
            }
        }
    }

    /*
     * Clean shutdown.
     */
    atomic_store(&context.running, 0);

    shutdown(socket_fd, SHUT_RDWR);

    int join_result = pthread_join(receiver, NULL);

    if (join_result != 0) {
        fprintf(
            stderr,
            "pthread_join: %s\n",
            strerror(join_result)
        );
    }

    close(socket_fd);

    printf("Host disconnected.\n");

    return EXIT_SUCCESS;
}