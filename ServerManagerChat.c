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
#define SERVER_MANAGER_PASSWORD "hellyabrother"

typedef struct {
    int socket;
    char username[MAX_USERNAME_LENGTH];
    bool is_active;
    bool is_server_manager;
} Client;

Client clients[MAX_CLIENTS]; // Global client list
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread-safe access to clients
int server_socket; // Make server_socket global
int mode = 0; // Global server mode
bool server_manager_authenticated = false;
bool server_operational = false;

void *handle_client(void *arg);
void *handle_server_manager(void *arg);
void start_server(const char *address, uint16_t port);
void broadcastMessage(const char* senderUsername, const char* message);
void listUsers(int sockfd);
void whisper(const char* senderUsername, const char* username, const char* message);
bool authenticate_server_manager(const char* password);
void initializeServerManagerMode(int client_socket);

// Helper functions to manage clients
void addClient(int socket);
void removeClient(int socket);
void trim_newline(char *str);
void send_message_protocol(int sockfd, const char *message);
Client* getClientByUsername(const char* username);
void setUsername(int sockfd, const char* username);
Client* getClientBySocket(int socket);
void processCommand(int sockfd, const char* command);
void sendHelp(int sockfd);
void sigintHandler(int sig_num);
int countActiveClients();
void notifyServerManagers(const char *message);

int main(int argc, char *argv[]) {
    // Parse command-line options
    int opt;
    while ((opt = getopt(argc, argv, "im")) != -1) {
        switch (opt) {
            case 'i':
                mode = 1; // Independent server mode
                break;
            case 'm':
                mode = 2; // Server manager mode
                break;
            default:
                fprintf(stderr, "Usage: %s [-i | -m] <address> <port>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Ensure address and port are provided
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

    printf("Mode: %d\n", mode);
    start_server(address, (uint16_t)port_long);

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

    if (mode == 1) { // Independent server mode
        server_operational = true; // Server is operational immediately in independent mode
    }

    while (1) {
        client_len = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        if (mode == 2 && !server_manager_authenticated) {
            // Handle server manager authentication separately.
            pthread_t server_manager_thread;
            if (pthread_create(&server_manager_thread, NULL, handle_server_manager, (void *)(intptr_t)client_socket) != 0) {
                perror("Server Manager Thread creation failed");
                close(client_socket);
            }
            continue; // Go back to the start of the loop.
        } else if (mode == 1 || (mode == 2 && server_operational)) { // Adjusted line
            pthread_t tid;
            if (pthread_create(&tid, NULL, handle_client, (void *)(intptr_t)client_socket) != 0) {
                perror("Thread creation failed");
                close(client_socket);
            } else {
                pthread_detach(tid);
            }
        } else {
            // This else block is for handling non-operational state in server manager mode
            close(client_socket); // Close client connection if not operational yet in server manager mode
        }
    }
    close(server_socket);
}


void *handle_client(void *arg) {
    int client_socket = (int)(intptr_t)arg;
    uint8_t version;
    uint16_t content_size_net, content_size;
    char content[BUFFER_SIZE];

    if (mode == 2) { // Server manager mode
        // Initially, only authenticate the server manager
        initializeServerManagerMode(client_socket);
    }

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

// commands need a space to work ex. "/h ", "/ul ", "/e "
void processCommand(int sockfd, const char* command) {
    Client* client = getClientBySocket(sockfd);
    if (!client) {
        printf("Client not found for socket: %d\n", sockfd);
        return;
    }

    char* token;
    char* rest = strdup(command);
    char* saveptr = NULL;
    token = strtok_r(rest, " ", &saveptr);

    if (token == NULL) {
        free(rest);
        return;
    }

    // Handling server start command by the server manager.
    if (strcmp(token, "/s") == 0 && client->is_server_manager) {
        server_operational = true;
        const char* msg = "Server is now operational. Accepting client connections...\n";
        send_message_protocol(sockfd, msg);
        free(rest);
        return; // Early return to prevent further command processing.
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
    } else if (strcmp(token, "/e") == 0) {
        printf("Client requested to close the connection.\n");
        removeClient(sockfd);
        close(sockfd);
        return; // Exit the function to prevent further processing
    } else {
        // Else it is not a recognized command, treat it as a broadcast message
        broadcastMessage(client->username, command);
    }
    free(rest);
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
        snprintf(whisperMsg, sizeof(whisperMsg), "%s whispers: %s\n", senderUsername, message);
        send_message_protocol(recipient->socket, whisperMsg);
    } else {
        // Handle user not found
        char errorMsg[BUFFER_SIZE];
        snprintf(errorMsg, sizeof(errorMsg), "Error: User '%s' not found.\n", username);
        // Now find the sender's socket to send back the error message
        Client* sender = getClientByUsername(senderUsername);
        if (sender) {
            send_message_protocol(sender->socket, errorMsg);
        } else {
            printf("Error: Sender '%s' not found.\n", senderUsername);
        }
    }
}

void sendHelp(int sockfd) {
    char helpMessage[] =
            "Supported commands:\n"
            "/u <username> - Set your username.\n"
            "/ul - List all users connected to the server.\n"
            "/w <username> <message> - Whisper a private message to <username>.\n"
            "/h - Show this help message.\n"
            "/e - Exit the connection.\n";
    send_message_protocol(sockfd, helpMessage);
}

void send_message_protocol(int sockfd, const char *message) {
    uint8_t version = 1;
    uint16_t size = htons(strlen(message)); // Convert message length to network byte order

    printf("Debug: Sending version: %u, message size: %u, message: %s\n", version, ntohs(size), message);

    char buffer[BUFFER_SIZE];
    int offset = 0;

    memcpy(buffer + offset, &version, sizeof(version));
    offset += sizeof(version);

    memcpy(buffer + offset, &size, sizeof(size));
    offset += sizeof(size);

    memcpy(buffer + offset, message, ntohs(size));

    ssize_t total_size = offset + ntohs(size);

    send(sockfd, buffer, total_size, 0);
}

void trim_newline(char *str) {
    char* pos;
    if ((pos = strchr(str, '\n')) != NULL) {
        *pos = '\0'; // Replace newline with null terminator
    }
}

void broadcastMessage(const char* senderUsername, const char* message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active && strcmp(clients[i].username, senderUsername) != 0) { // Don't send the message back to the sender
            char formattedMessage[BUFFER_SIZE];
            snprintf(formattedMessage, BUFFER_SIZE, "%s has sent: %s\n", senderUsername, message);

            // Debug log to verify formatted message
            printf("Broadcasting message: %s\n", formattedMessage);

            send_message_protocol(clients[i].socket, formattedMessage);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}


void addClient(int socket) {
    pthread_mutex_lock(&clients_mutex);
    bool found = false;
    for (int i = 0; i < MAX_CLIENTS && !found; i++) {
        if (!clients[i].is_active) {
            clients[i].socket = socket;
            snprintf(clients[i].username, MAX_USERNAME_LENGTH, "client_%d", i);
            clients[i].is_active = true;
            printf("Client %d connected with username: %s\n", socket, clients[i].username);
            found = true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    if (found) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Connected clients: %d", countActiveClients());
        notifyServerManagers(msg);
    }
}

void removeClient(int socket) {
    pthread_mutex_lock(&clients_mutex);
    bool found = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket == socket) {
            clients[i].is_active = false;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    if (found) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Connected clients: %d", countActiveClients());
        notifyServerManagers(msg);
    }
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

void initializeServerManagerMode(int client_socket) {
    char buffer[BUFFER_SIZE];
    bool authenticated = false;

    // Initially, authenticate only the server manager.
    while (!authenticated) {
        ssize_t bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            printf("Client disconnected or error occurred.\n");
            close(client_socket);
            return;
        }

        buffer[bytes_received] = '\0'; // Null-terminate the received data.
        if (authenticate_server_manager(buffer)) {
            authenticated = true;
            server_manager_authenticated = true;
            const char* msg = "Authentication successful. Use /s to start server.\n";
            send_message_protocol(client_socket, msg);

            // Mark this client as the server manager.
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].socket == client_socket) {
                    clients[i].is_server_manager = true;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        } else {
            const char* msg = "Authentication failed. Try again.\n";
            send_message_protocol(client_socket, msg);
        }
    }
}


bool authenticate_server_manager(const char* password) {
    return strcmp(password, SERVER_MANAGER_PASSWORD) == 0;
}

void *handle_server_manager(void *arg) {
    int client_socket = (int)(intptr_t)arg;
    char content[BUFFER_SIZE];
    uint8_t version;
    uint16_t content_size_net;
    uint16_t content_size;

    while (!server_manager_authenticated) {
        // Read version
        if (recv(client_socket, &version, sizeof(version), 0) <= 0) {
            printf("Failed to read version\n");
            break;
        }

        // Read content size
        if (recv(client_socket, &content_size_net, sizeof(content_size_net), 0) <= 0) {
            printf("Failed to read content size\n");
            break;
        }
        content_size = ntohs(content_size_net);

        // Ensure that the content size is within buffer limits
        if (content_size >= BUFFER_SIZE) {
            printf("Content size too large\n");
            break;
        }

        // Read content based on the received size
        ssize_t bytes_received = recv(client_socket, content, content_size, 0);
        if (bytes_received <= 0) {
            printf("Failed to read content or server manager disconnected\n");
            break;
        }
        content[bytes_received] = '\0'; // Null-terminate the received content

        printf("Debug: Received password attempt: %s\n", content);
        if (!server_manager_authenticated && authenticate_server_manager(content)) {
            server_manager_authenticated = true;
            const char* msg = "Authentication successful. Use /s to start the server or /q to quit.\n";
            send_message_protocol(client_socket, msg);
            // Mark this client as the server manager.
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].socket == client_socket) {
                    clients[i].is_server_manager = true;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        } else {
            const char* msg = "Authentication failed. Try again.\n";
            send_message_protocol(client_socket, msg);
        }
    }

    // Handle server manager commands.
    while (server_manager_authenticated) {
        // Read version
        if (recv(client_socket, &version, sizeof(version), 0) <= 0) {
            printf("Failed to read version\n");
            break;
        }

        // Read content size
        if (recv(client_socket, &content_size_net, sizeof(content_size_net), 0) <= 0) {
            printf("Failed to read content size\n");
            break;
        }
        content_size = ntohs(content_size_net);

        // Ensure that the content size is within buffer limits
        if (content_size >= BUFFER_SIZE) {
            printf("Content size too large\n");
            continue; // Skip this message but don't disconnect
        }

        // Read content based on the received size
        ssize_t bytes_received = recv(client_socket, content, content_size, 0);
        if (bytes_received <= 0) {
            printf("Failed to read content or server manager disconnected\n");
            break; // Break out of the loop on failure to read content
        }
        content[bytes_received] = '\0'; // Null-terminate the received content

        // Process the command
        if (strcmp(content, "/s") == 0) {
            server_operational = true;
            broadcastMessage("Server", "Server is now operational. Accepting client connections...");
            printf("Server is now operational. Accepting client connections...\n");
        } else if (strcmp(content, "/q") == 0) {
            printf("Server is shutting down by server manager command.\n");
            exit(EXIT_SUCCESS); // Or handle server shutdown more gracefully.
        }
    }
    return NULL;
}

int countActiveClients() {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active) count++;
    }
    return count;
}

void notifyServerManagers(const char *message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].is_active && clients[i].is_server_manager) {
            send_message_protocol(clients[i].socket, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

