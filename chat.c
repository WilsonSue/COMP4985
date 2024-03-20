#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>

#define MAX_CLIENTS 32
#define BUFFER_SIZE 1024
#define UINT16_MAX 65535

//test commit

struct ClientInfo {
    int client_socket;
    int client_index;
    int clients[MAX_CLIENTS];
    char username[256];
    bool is_username_set;
};

static const int value = 10;
static const int valueNew = 20;

void *handle_client(void *arg);
void start_server(const char *address, uint16_t port);
void processCommand(struct ClientInfo* client, const char* command);
void setUsername(struct ClientInfo* client, const char* username);
void listUsers(struct ClientInfo* client);
void whisper(struct ClientInfo* client, const char* username, const char* message);
void sendHelp(struct ClientInfo* client);
void send_message_protocol(int sockfd, const char *message);
void trim_newline(char *str);

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s [-a] <address> <port>\n", argv[0]);
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

        printf("Starting server on %s:%s\n", argv[2], argv[3]);
        start_server(argv[2], (uint16_t)port_long);
    } else {
        fprintf(stderr,
                "Invalid mode. Use -a for the server or -c for the client.\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}

void start_server(const char *address, uint16_t port) {
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
        perror("Server Socket creation failed");
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

    printf("Server setup complete. Waiting for connections...\n");
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
            printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

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
            printf("Started new thread for client %d\n", client_index);

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
                    send_message_protocol(clients[i], server_buffer);
                }
            }
        }
    }

    // Close the server socket when the loop exits
    close(server_socket);
}

void *handle_client(void *arg) {
    struct ClientInfo *client_info = (struct ClientInfo *)arg;
    uint8_t version;
    uint16_t content_size_net; // Content size in network byte order
    uint16_t content_size;     // Content size in host byte order
    char content[BUFFER_SIZE]; // Buffer for the message content

    while (1) {
        // Read the version
        ssize_t bytes_received = recv(client_info->client_socket, &version, sizeof(version), 0);
        if (bytes_received <= 0) {
            break; // Break if connection is closed or an error occurred
        }

        // Read the content size
        bytes_received = recv(client_info->client_socket, &content_size_net, sizeof(content_size_net), 0);
        if (bytes_received <= 0) {
            break; // Break if connection is closed or an error occurred
        }
        content_size = ntohs(content_size_net); // Convert from network byte order to host byte order

        // Validate content size to avoid buffer overflow
        if (content_size > sizeof(content) - 1) {
            printf("Content size too large\n");
            continue; // Skip this message
        }

        // Read the content
        bytes_received = recv(client_info->client_socket, content, content_size, 0);
        if (bytes_received <= 0) {
            break; // Break if connection is closed or an error occurred
        }
        content[bytes_received] = '\0'; // Null-terminate the received content

        // Now that we have the content, we can process the command
        processCommand(client_info, content);
    }

    // Close the client socket and free the client_info structure
    close(client_info->client_socket);
    free(client_info);
    return NULL;
}


void processCommand(struct ClientInfo* client, const char* command) {
    // Ensure command is null-terminated
    char command_copy[BUFFER_SIZE];
    strncpy(command_copy, command, BUFFER_SIZE);
    command_copy[BUFFER_SIZE - 1] = '\0';
    trim_newline(command_copy); // Remove newline characters

    // Parse and execute commands
    if (strncmp(command_copy, "/u ", 3) == 0) {
        // Set username
        setUsername(client, command_copy + 3); // Skip over "/u " to username
    } else if (strcmp(command_copy, "/ul") == 0) {
        // List users
        listUsers(client);
    } else if (strncmp(command_copy, "/w ", 3) == 0) {
        // Whisper
        char* username = strtok(command_copy + 3, " "); // Extract username
        char* message = strtok(NULL, ""); // Extract message
        if (username && message) {
            whisper(client, username, message);
        }
    } else if (strcmp(command_copy, "/h") == 0) {
        // Help
        sendHelp(client);
    } else {
        // Unknown command
        const char* response = "Unknown command\n";
        send_message_protocol(client->client_socket, response);
    }
}



void setUsername(struct ClientInfo* client, const char* username) {
    strncpy(client->username, username, sizeof(client->username) - 1);
    client->username[sizeof(client->username) - 1] = '\0'; // Ensure null-termination
    client->is_username_set = true;
    char msg[] = "Username set successfully!\n";
    send_message_protocol(client->client_socket, msg);
}


void listUsers(struct ClientInfo* client) {
    char userlist[BUFFER_SIZE] = "Connected users:\n";
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client->clients[i] != 0 && client[i].is_username_set) {
            strcat(userlist, client[i].username);
            strcat(userlist, "\n");
        }
    }
    // Using the send_message_protocol function
    send_message_protocol(client->client_socket, userlist);
}

void whisper(struct ClientInfo* client, const char* username, const char* message) {
    bool userFound = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client->clients[i] != 0 && strcmp(client[i].username, username) == 0) {
            // Here, assume you will wrap the actual whisper message in the protocol format
            send_message_protocol(client[i].client_socket, message);
            userFound = true;
            break;
        }
    }
    if (!userFound) {
        char msg[] = "User not found.\n";
        send_message_protocol(client->client_socket, msg);
    }
}


void sendHelp(struct ClientInfo* client) {
    char helpMessage[] =
            "Supported commands:\n"
            "/u <username> - Set your username.\n"
            "/ul - List all users connected to the server.\n"
            "/w <username> <message> - Whisper a private message to <username>.\n"
            "/h - Show this help message.\n";
    // Using the send_message_protocol function
    send_message_protocol(client->client_socket, helpMessage);
}

void send_message_protocol(int sockfd, const char *message) {
    uint8_t version = 1;
    uint16_t size = htons(strlen(message)); // Convert message length to network byte order

    // Allocate buffer for version, size, and message
    char buffer[BUFFER_SIZE];
    int offset = 0;

    // Assign version to buffer
    memcpy(buffer + offset, &version, sizeof(version));
    offset += sizeof(version);

    // Assign size to buffer
    memcpy(buffer + offset, &size, sizeof(size));
    offset += sizeof(size);

    // Copy message content to buffer
    memcpy(buffer + offset, message, ntohs(size));

    // Calculate total size to send
    ssize_t total_size = offset + ntohs(size);

    // Send buffer
    send(sockfd, buffer, total_size, 0);
}

void trim_newline(char *str) {
    char* pos;
    if ((pos = strchr(str, '\n')) != NULL) {
        *pos = '\0'; // Replace newline with null terminator
    }
}