#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define UINT16_MAX 65535

struct ClientInfo {
    int client_socket;
    int client_index;
    int clients[MAX_CLIENTS];
};

static const int value = 10;
static const int valueNew = 20;

static void *handle_client(void *arg);
static void start_server(const char *address, uint16_t port);
static void start_client(const char *address, uint16_t port);

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s [-a/-c] <address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strcmp(argv[1], "-a") == 0) {
        char *endptr;
        long int port_long = strtol(argv[3], &endptr, value);

        // Check for conversion errors
        if (*endptr != '\0' || port_long < 0 || port_long > UINT16_MAX) {
            fprintf(stderr, "Invalid port number: %s\n", argv[3]);
            exit(EXIT_FAILURE);
        }

        start_server(argv[2], (uint16_t)port_long);
    } else if (strcmp(argv[1], "-c") == 0) {
        char *endptr;
        long int port_long = strtol(argv[3], &endptr, value);

        // Check for conversion errors
        if (*endptr != '\0' || port_long < 0 || port_long > UINT16_MAX) {
            fprintf(stderr, "Invalid port number: %s\n", argv[3]);
            exit(EXIT_FAILURE);
        }

        start_client(argv[2], (uint16_t)port_long);
    } else {
        fprintf(stderr,
                "Invalid mode. Use -a for the server or -c for the client.\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}

void *handle_client(void *arg) {
    char buffer[BUFFER_SIZE];
    struct ClientInfo *client_info = (struct ClientInfo *)arg;
    int client_socket = client_info->client_socket;
    int client_index = client_info->client_index;
    int *clients = client_info->clients;

    while (1) {
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            printf("Server closed the connection.\n");
            close(client_socket);
            clients[client_index] = 0;
            free(client_info);
            pthread_exit(NULL);
        }

        buffer[bytes_received] = '\0';
        printf("Received from Client %d: %s", client_index, buffer);

        // Broadcast the message to all other connected clients
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] != 0 && i != client_index) {
                send(clients[i], buffer, strlen(buffer), 0);
            }
        }
    }
}

static void start_server(const char *address, uint16_t port) {
    int server_socket;
    int client_socket;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    int clients[MAX_CLIENTS] = {0};
    int optval = 1;

#ifdef SOCK_CLOEXEC
    server_socket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    server_socket =
      socket(AF_INET, SOCK_STREAM, 0); // NOLINT(android-cloexec-socket)
#endif

    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(address);
    server_addr.sin_port = htons(port);

    // Bind
    if (bind(server_socket, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval,
                   sizeof(optval)) == -1) {
        perror("Setsockopt failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(server_socket, valueNew) == -1) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s:%d\n", address, port);

    while (1) {
        int activity;
        int client_len = sizeof(client_addr);
        pthread_t tid;
        int max_sd = server_socket;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET((long unsigned int)server_socket, &readfds);
        FD_SET((long unsigned int)STDIN_FILENO, &readfds);

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] > 0) {
                FD_SET((long unsigned int)clients[i], &readfds);
                if (clients[i] > max_sd) {
                    max_sd = clients[i];
                }
            }
        }

        // Wait for activity on one of the sockets
        activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);

        if (activity < 0) {
            perror("Select error");
            close(server_socket);
            exit(EXIT_FAILURE);
        }

        // New connection
        if (FD_ISSET((long unsigned int)server_socket, &readfds)) {
            struct ClientInfo *client_info;
            int client_index = -1;
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i] == 0) {
                    client_index = i;
                    break;
                }
            }

            if (client_index == -1) {
                fprintf(stderr, "Too many clients. Connection rejected.\n");
                close(server_socket); // Move close inside the loop
                break;
            }

            client_socket = accept(server_socket, (struct sockaddr *)&client_addr,
                                   (socklen_t *)&client_len);
            if (client_socket == -1) {
                perror("Accept failed");
                close(server_socket);
                exit(EXIT_FAILURE);
            }

            printf("New connection from %s:%d, assigned to Client %d\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                   client_index);

            clients[client_index] = client_socket;

            // Create a structure to hold client information
            client_info = (struct ClientInfo *)malloc(sizeof(struct ClientInfo));
            if (client_info == NULL) {
                perror("Memory allocation failed");
                close(server_socket);
                exit(EXIT_FAILURE);
            }

            client_info->client_socket = client_socket;
            client_info->client_index = client_index;
            memcpy(client_info->clients, clients, sizeof(clients));

            // Create a new thread to handle the client
            if (pthread_create(&tid, NULL, handle_client, (void *)client_info) != 0) {
                perror("Thread creation failed");
                close(server_socket);
                free(client_info);
                exit(EXIT_FAILURE);
            }

            // Detach the thread (we won't join it, allowing it to clean up resources
            // on its own)
            pthread_detach(tid);
        }

        // Check if there is input from the server's console
        if (FD_ISSET((long unsigned int)STDIN_FILENO, &readfds)) {
            char server_buffer[BUFFER_SIZE];
            fgets(server_buffer, sizeof(server_buffer), stdin);

            // Broadcast the server's message to all connected clients
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i] != 0) {
                    send(clients[i], server_buffer, strlen(server_buffer), 0);
                }
            }
        }
    }

    // Close the server socket when the loop exits
    close(server_socket);
}

void start_client(const char *address, uint16_t port) {
    int client_socket;
    struct sockaddr_in server_addr;
    int flags;

#ifdef SOCK_CLOEXEC
    client_socket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    client_socket =
      socket(AF_INET, SOCK_STREAM, 0); // NOLINT(android-cloexec-socket)
#endif

    if (client_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    flags = fcntl(client_socket, F_GETFD);
    if (flags == -1) {
        close(client_socket);
        perror("Error getting flags on socket");
        exit(EXIT_FAILURE);
    }

    flags |= FD_CLOEXEC;
    if (fcntl(client_socket, F_SETFD, flags) == -1) {
        close(client_socket);
        perror("Error setting FD_CLOEXEC on socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(address);
    server_addr.sin_port = htons(port);

    // Connect to the server
    if (connect(client_socket, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {
        perror("\nConnection failed");
        exit(EXIT_FAILURE);
    }

    printf("Connected to the server. Type your messages and press Enter to send. "
           "Press Ctrl-Z to exit or Ctrl-D to close the Server Connection.\n");

    // Start a simple chat loop
    while (1) {
        int activity;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET((long unsigned int)client_socket, &readfds);
        FD_SET((long unsigned int)STDIN_FILENO, &readfds);

        // Wait for activity on the socket or user input
        activity = select(client_socket + 1, &readfds, NULL, NULL, NULL);

        if (activity < 0) {
            perror("Select error");
            break;
        }

        // Check if there is a message from the server or other clients
        if (FD_ISSET((long unsigned int)client_socket, &readfds)) {
            char server_buffer[BUFFER_SIZE];
            ssize_t bytes_received =
                    recv(client_socket, server_buffer, sizeof(server_buffer) - 1, 0);

            if (bytes_received <= 0) {
                printf("\nServer closed the connection.\n");
                break;
            }

            server_buffer[bytes_received] = '\0';
            printf("Received: %s", server_buffer);
        }

        // Check if there is user input
        if (FD_ISSET((long unsigned int)STDIN_FILENO, &readfds)) {
            char client_buffer[BUFFER_SIZE];
            if (fgets(client_buffer, sizeof(client_buffer), stdin) == NULL) {
                // Ctrl-D was pressed, causing EOF
                printf("EOF detected. Closing connection.\n");
                break;
            }

            if (send(client_socket, client_buffer, strlen(client_buffer), 0) == -1) {
                perror("Error sending message");
                break;
            }
        }
    }

    // Close the client socket when the loop exits
    close(client_socket);
}
