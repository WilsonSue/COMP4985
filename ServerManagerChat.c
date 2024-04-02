#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <getopt.h>

#define MAX_CLIENTS 32
#define BUFFER_SIZE 1024
#define MAX_USERNAME_LENGTH 256

typedef struct {
    int socket;
    char username[MAX_USERNAME_LENGTH];
    bool is_active;
} Client;

Client clients[MAX_CLIENTS]; // Global client list
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread-safe access to clients
int server_socket; // Make server_socket global

void *handle_client(void *arg);
void start_server(const char *address, uint16_t port);
void send_message_protocol(int sockfd, const char *message);
void broadcastMessage(const char* senderUsername, const char* message);
void listUsers(int sockfd);
void whisper(const char* senderUsername, const char* username, const char* message);
void trim_newline(char *str);

// Helper functions to manage clients
void addClient(int socket);
void removeClient(int socket);
Client* getClientByUsername(const char* username);
void setUsername(int sockfd, const char* username);
Client* getClientBySocket(int socket);
void processCommand(int sockfd, const char* command);
void sendHelp(int sockfd);
void sigintHandler(int sig_num);

int main(int argc, char *argv[]) {
    int mode = 0; // 0: undefined, 1: independent server, 2: server manager

    int opt;
    while ((opt = getopt(argc, argv, "im")) != -1) {
        switch (opt) {
            case 'i':
                mode = 1;
                break;
            case 'm':
                mode = 2;
                break;
            default: /* '?' */
                fprintf(stderr, "Usage: %s [-i | -m] <address> <port>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (mode == 0 || optind + 2 > argc) {
        fprintf(stderr, "Usage: %s [-i | -m] <address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *address = argv[optind];
    char *port_str = argv[optind + 1];
    long int port_long = strtol(port_str, NULL, 10);
    if (port_long < 1 || port_long > UINT16_MAX) {
        fprintf(stderr, "Invalid port number: %s\n", port_str);
        exit(EXIT_FAILURE);
    }

    switch (mode) {
        case 1: // Independent server mode
            printf("Starting independent server on %s:%ld\n", address, port_long);
            start_server(address, (uint16_t)port_long);
            break;
        case 2: // Server manager mode
            printf("Starting server manager on %s:%ld\n", address, port_long);
            // Add specific logic for server manager mode here
            break;
        default:
            fprintf(stderr, "Unknown mode. Please use -i for independent server mode or -m for server manager mode.\n");
            exit(EXIT_FAILURE);
    }


    return 0;
}

void start_server(const char *address, uint16_t port) {
    int client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;

    // Register signal handler for SIGINT
    signal(SIGINT, sigintHandler);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Server socket creation failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(address);
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, MAX_CLIENTS) == -1) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s:%d\n", address, port);

    while (1) {
        client_len = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void *)(intptr_t)client_socket) != 0) {
            perror("Thread creation failed");
            close(client_socket);
        } else {
            pthread_detach(tid);
        }
    }
    close(server_socket);
}


void *handle_client(void *arg) {
    int client_socket = (int)(intptr_t)arg;
    uint8_t version;
    uint16_t content_size_net, content_size;
    char content[BUFFER_SIZE];

    addClient(client_socket); // Add client with a temporary empty username

    while (1) {
        if (recv(client_socket, &version, sizeof(version), 0) <= 0 ||
            recv(client_socket, &content_size_net, sizeof(content_size_net), 0) <= 0) {
            break; // Client disconnected
        }

        content_size = ntohs(content_size_net);
        if (content_size >= BUFFER_SIZE) {
            printf("Message too large.\n");
            continue;
        }

        ssize_t bytes_received = recv(client_socket, content, content_size, 0);
        if (bytes_received <= 0) {
            break; // Client disconnected
        }

        content[bytes_received] = '\0'; // Ensure message is null-terminated
        printf("Received message: %s\n", content);

        printf("Debug: Message before processing command: %s\n", content);
        // Example processing command, this needs to be expanded according to your application's protocol
        processCommand(client_socket, content);
    }

    removeClient(client_socket); // Remove client from global list
    close(client_socket);
    return NULL;
}

void processCommand(int sockfd, const char* command) {
    Client* client = getClientBySocket(sockfd);
    if (!client) {
        printf("Client not found for socket: %d\n", sockfd);
        return;
    }

    char* token;
    char* rest = strdup(command); // Create a mutable copy of command for strtok_r
    char* saveptr = NULL; // Use a separate pointer for strtok_r
    token = strtok_r(rest, " ", &saveptr); // Pass the address of saveptr

    if (token == NULL) {
        free(rest);
        return;
    }

    if (strcmp(token, "/u") == 0) {
        token = strtok_r(NULL, " ", &saveptr); // Attempt to retrieve the next part of the command (username)
        if (token) {
            setUsername(sockfd, token);
        } else {
            // If token is NULL, it means no username was provided after /u
            const char* errorMessage = "Error: No username provided. Usage: /u <username>\n";
            send_message_protocol(sockfd, errorMessage);
        }
    } else if (strcmp(token, "/ul") == 0) {
        listUsers(sockfd);
    } else if (strcmp(token, "/w") == 0) {
        char* username = strtok_r(NULL, " ", &saveptr); // Continue using saveptr
        char* message = strtok_r(NULL, "", &saveptr); // Continue using saveptr
        if (username && message) {
            whisper(client->username, username, message);
        }  else {
            const char* errorMessage = "Error: Incorrect whisper command usage. Usage: /w <username> <message>\n";
            send_message_protocol(sockfd, errorMessage);
        }
    } else if (strcmp(token, "/h") == 0) {
        printf("Help command received.\n");
        sendHelp(sockfd);
    } else {
        // This is not a recognized command, treat it as a broadcast message
        broadcastMessage(client->username, command);
    }
    free(rest); // Now it's safe to free the strdup'ed command copy
}

void setUsername(int sockfd, const char* username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == sockfd) {
            strncpy(clients[i].username, username, MAX_USERNAME_LENGTH - 1);
            clients[i].username[MAX_USERNAME_LENGTH - 1] = '\0'; // Ensure null-termination
            char msg[] = "Username set successfully.\n";
            send_message_protocol(sockfd, msg);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void listUsers(int sockfd) {
    char userlist[BUFFER_SIZE] = "Connected users:\n";
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active) {
            strcat(userlist, clients[i].username);
            strcat(userlist, "\n");
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    send_message_protocol(sockfd, userlist);
}


void whisper(const char* senderUsername, const char* username, const char* message) {
    Client* recipient = getClientByUsername(username);
    if (recipient) {
        char whisperMsg[BUFFER_SIZE];
        snprintf(whisperMsg, sizeof(whisperMsg), "%s whispers: %s", senderUsername, message);
        send_message_protocol(recipient->socket, whisperMsg);
    } else {
        // Handle user not found
    }
}


void sendHelp(int sockfd) {
    char helpMessage[] =
            "Supported commands:\n"
            "/u <username> - Set your username.\n"
            "/ul - List all users connected to the server.\n"
            "/w <username> <message> - Whisper a private message to <username>.\n"
            "/h - Show this help message.\n";
    send_message_protocol(sockfd, helpMessage);
}

void send_message_protocol(int sockfd, const char *message) {
    uint8_t version = 1;
    uint16_t size = htons(strlen(message)); // Convert message length to network byte order

    printf("Debug: Sending version: %u, message size: %u, message: %s\n", version, ntohs(size), message);

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

// Modify the function to accept the sender's username
void broadcastMessage(const char* senderUsername, const char* message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active && strcmp(clients[i].username, senderUsername) != 0) { // Don't send the message back to the sender
            char formattedMessage[BUFFER_SIZE];
            snprintf(formattedMessage, BUFFER_SIZE, "%s has sent: %s", senderUsername, message);

            // Debug log to verify formatted message
            printf("Broadcasting message: %s\n", formattedMessage);

            send_message_protocol(clients[i].socket, formattedMessage);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}


void addClient(int socket) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].is_active) {
            clients[i].socket = socket;
            snprintf(clients[i].username, MAX_USERNAME_LENGTH, "client_%d", i); // Auto assign username
            clients[i].is_active = true;
            printf("Client %d connected with username: %s\n", socket, clients[i].username); // Debug log
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void removeClient(int socket) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == socket) {
            clients[i].is_active = false;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

Client* getClientBySocket(int socket) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == socket && clients[i].is_active) {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void sigintHandler(int sig_num) {
    // Signal handler for SIGINT
    (void)sig_num;

    printf("\nTerminating server...\n");
    if (server_socket >= 0) {
        close(server_socket);
        printf("Server socket closed.\n");
    }
    exit(EXIT_SUCCESS);
}

Client* getClientByUsername(const char* username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active && strcmp(clients[i].username, username) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}