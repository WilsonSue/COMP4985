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
void broadcastMessage(const char* message);
void listUsers(int sockfd);
void whisper(const char* senderUsername, const char* username, const char* message);
void trim_newline(char *str);

// Helper functions to manage clients
void addClient(int socket, const char* username);
void removeClient(int socket);
Client* getClientByUsername(const char* username);
void setUsername(int sockfd, const char* username);
Client* getClientBySocket(int socket);
void processCommand(int sockfd, const char* command);
void sendHelp(int sockfd);
void sigintHandler(int sig_num);

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *endptr;
    long int port_long = strtol(argv[2], &endptr, 10);

    // Check for conversion errors
    if (*endptr != '\0' || port_long < 0 || port_long > UINT16_MAX) {
        fprintf(stderr, "Invalid port number: %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    printf("Starting server on %s:%ld\n", argv[1], port_long);
    start_server(argv[1], (uint16_t)port_long);

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

    addClient(client_socket, ""); // Add client with a temporary empty username

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
        token = strtok_r(NULL, " ", &saveptr); // Continue using saveptr
        if (token) {
            setUsername(sockfd, token);
        }
    } else if (strcmp(token, "/ul") == 0) {
        listUsers(sockfd);
    } else if (strcmp(token, "/w") == 0) {
        char* username = strtok_r(NULL, " ", &saveptr); // Continue using saveptr
        char* message = strtok_r(NULL, "", &saveptr); // Continue using saveptr
        if (username && message) {
            whisper(client->username, username, message);
        }
    } else if (strcmp(token, "/h") == 0) {
        sendHelp(sockfd);
    } else {
        // This is not a recognized command, treat it as a broadcast message
        broadcastMessage(command);
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

void broadcastMessage(const char* message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active) {
            send_message_protocol(clients[i].socket, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void addClient(int socket, const char* username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].is_active) {
            clients[i].socket = socket;
            strncpy(clients[i].username, username, MAX_USERNAME_LENGTH - 1);
            clients[i].is_active = true;
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
    printf("\nTerminating server...\n");
    if (server_socket >= 0) {
        close(server_socket);
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
