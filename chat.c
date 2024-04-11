#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define BUFFER_SIZE 1024
#define MAX_USERNAME_LENGTH 256
#define SERVER_MANAGER_PASSWORD "hellyabrother"
#define TEN 10

typedef struct
{
    int  socket;
    char username[MAX_USERNAME_LENGTH];
    bool is_active;
    bool is_server_manager;
} Client;

static Client          clients[MAX_CLIENTS];                         // Global client list NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;    // Mutex for thread-safe access to clients NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static int             server_socket;                                // Make server_socket global NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static int             mode                         = 0;             // Global server mode NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static bool            server_manager_authenticated = false;         // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static bool            server_operational           = false;         // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void                           disconnectAllClients(void);
void                          *handle_client(void *arg);
void                          *handle_server_manager(void *arg);
__attribute__((noreturn)) void start_server(const char *address, uint16_t port);
void                           broadcastMessage(const char *senderUsername, const char *message);
void                           listUsers(int sockfd);
void                           whisper(const char *senderUsername, const char *username, const char *message);

bool authenticate_server_manager(const char *password);
// Helper functions to manage clients
void                           addClient(int socket);
void                           removeClient(int socket);
void                           trim_newline(char *str);
void                           send_message_protocol(int sockfd, const char *message);
Client                        *getClientByUsername(const char *username);
void                           setUsername(int sockfd, const char *username);
Client                        *getClientBySocket(int socket);
void                           processCommand(int sockfd, const char *command);
void                           sendHelp(int sockfd);
__attribute__((noreturn)) void sigintHandler(int sig_num);
int                            countActiveClients(void);
void                           notifyServerManagers(const char *message);

int main(int argc, char *argv[])
{
    // Parse command-line options
    int      opt;
    char    *address;
    char    *port_str;
    long int port_long;
    while((opt = getopt(argc, argv, "im")) != -1)
    {
        switch(opt)
        {
            case 'i':
                mode = 1;    // Independent server mode
                break;
            case 'm':
                mode = 2;    // Server manager mode
                break;
            default:
                fprintf(stderr, "Usage: %s [-i | -m] <address> <port>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Ensure address and port are provided
    if(mode == 0 || optind + 2 > argc)
    {
        fprintf(stderr, "Usage: %s [-i | -m] <address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    address   = argv[optind];
    port_str  = argv[optind + 1];
    port_long = strtol(port_str, NULL, TEN);
    if(port_long < 1 || port_long > UINT16_MAX)
    {
        fprintf(stderr, "Invalid port number: %s\n", port_str);
        exit(EXIT_FAILURE);
    }

    printf("Mode: %d\n", mode);
    start_server(address, (uint16_t)port_long);

    // return 0; <-- NEED TO CHANGE THIS
}

void start_server(const char *address, uint16_t port)
{
    int                client_socket;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t          client_len;

    // Register signal handler for SIGINT
    signal(SIGINT, sigintHandler);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(server_socket == -1)
    {
        perror("Server socket creation failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(address);
    server_addr.sin_port        = htons(port);

    if(bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if(listen(server_socket, MAX_CLIENTS) == -1)
    {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s:%d\n", address, port);

    if(mode == 1)
    {                                 // Independent server mode
        server_operational = true;    // Server is operational immediately in independent mode
    }

    while(1)
    {
        client_len    = sizeof(client_addr);
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if(client_socket < 0)
        {
            perror("Accept failed");
            continue;
        }

        if(mode == 2 && !server_manager_authenticated)
        {
            pthread_t server_manager_thread;
            if(pthread_create(&server_manager_thread, NULL, handle_server_manager, (void *)&client_socket) != 0)
            {
                perror("Server Manager Thread creation failed");
                close(client_socket);
            }
        }
        else
        {
            pthread_t tid;
            if(pthread_create(&tid, NULL, handle_client, (void *)&client_socket) != 0)
            {
                perror("Thread creation failed");
                close(client_socket);
            }
            else
            {
                pthread_detach(tid);
            }
        }
    }
    // close(server_socket); <--NEED TO CHANGE THIS
}

void *handle_client(void *arg)
{
    int      client_socket = (int)(intptr_t)arg;
    uint8_t  version;
    uint16_t content_size_net;
    uint16_t content_size;
    char     content[BUFFER_SIZE];
    ssize_t  bytes_received;

    addClient(client_socket);    // Add client with a temporary empty username

    while(1)
    {
        if(recv(client_socket, &version, sizeof(version), 0) <= 0 || recv(client_socket, &content_size_net, sizeof(content_size_net), 0) <= 0)
        {
            break;    // Client disconnected
        }

        content_size = ntohs(content_size_net);
        if(content_size > BUFFER_SIZE - 1)
        {
            printf("Message too large.\n");
            continue;
        }

        bytes_received = recv(client_socket, content, content_size, 0);
        if(bytes_received <= 0)
        {
            break;    // Client disconnected
        }

        content[bytes_received] = '\0';    // Ensure message is null-terminated
        printf("Received message: %s\n", content);

        printf("Debug: Message before processing command: %s\n", content);
        // Example processing command, this needs to be expanded according to your application's protocol
        processCommand(client_socket, content);
    }

    removeClient(client_socket);    // Remove client from global list
    close(client_socket);
    return NULL;
}

// commands need a space to work ex. "/h ", "/ul ", "/e "
void processCommand(int sockfd, const char *command)
{
    char        commandCopy[BUFFER_SIZE];
    char       *token;
    char       *rest    = commandCopy;    // Directly use trimmed command copy
    char       *saveptr = NULL;
    const char *msg;
    Client     *client;
    token = strtok_r(rest, " ", &saveptr);

    client = getClientBySocket(sockfd);
    if(!client)
    {
        printf("Client not found for socket: %d\n", sockfd);
        return;
    }

    // Copy and trim the command to ensure we work with a clean version
    strncpy(commandCopy, command, BUFFER_SIZE - 1);    // Ensure space for null terminator
    commandCopy[BUFFER_SIZE - 1] = '\0';               // Manually null-terminate
    trim_newline(commandCopy);                         // Trim newline from the command copy

    // Use commandCopy instead of duplicating original command
    if(token == NULL)
    {
        return;    // Early exit if the token is null
    }

    if(strcmp(token, "/s") == 0 && client->is_server_manager)
    {
        server_operational = true;
        msg                = "Server is now operational. Accepting client connections...\n";
        send_message_protocol(sockfd, msg);
        return;    // Early return to prevent further command processing
    }
    if(strcmp(token, "/u") == 0)
    {
        token = strtok_r(NULL, " ", &saveptr);
        if(token)
        {
            setUsername(sockfd, token);
        }
        else
        {
            const char *errorMessage = "Error: No username provided. Usage: /u <username>\n";
            send_message_protocol(sockfd, errorMessage);
        }
    }
    else if(strcmp(token, "/ul") == 0)
    {
        listUsers(sockfd);
    }
    else if(strcmp(token, "/w") == 0)
    {
        char *username = strtok_r(NULL, " ", &saveptr);
        char *message  = strtok_r(NULL, "", &saveptr);
        if(username && message)
        {
            whisper(client->username, username, message);
        }
        else
        {
            const char *errorMessage = "Error: Incorrect whisper command usage. Usage: /w <username> <message>\n";
            send_message_protocol(sockfd, errorMessage);
        }
    }
    else if(strcmp(token, "/h") == 0)
    {
        sendHelp(sockfd);
    }
    else if(strcmp(token, "/e") == 0)
    {
        printf("Client requested to close the connection.\n");
        removeClient(sockfd);
        close(sockfd);
    }
    else
    {
        broadcastMessage(client->username, command);    // Use trimmed command for broadcasting
    }
}

void setUsername(int sockfd, const char *username)
{
    char msg[] = "Username set successfully.\n";
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].socket == sockfd)
        {
            char trimmedUsername[MAX_USERNAME_LENGTH];
            strncpy(trimmedUsername, username, MAX_USERNAME_LENGTH);
            trim_newline(trimmedUsername);    // Trim newline character
            strncpy(clients[i].username, trimmedUsername, MAX_USERNAME_LENGTH - 1);
            clients[i].username[MAX_USERNAME_LENGTH - 1] = '\0';    // Ensure null-termination

            send_message_protocol(sockfd, msg);
            printf("Username changed to: %s\n", clients[i].username);    // Debugging line to confirm the change
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void listUsers(int sockfd)
{
    char userlist[BUFFER_SIZE] = "Connected users:\n";
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].is_active)
        {
            strcat(userlist, clients[i].username);
            strcat(userlist, "\n");
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    send_message_protocol(sockfd, userlist);
}

void whisper(const char *senderUsername, const char *username, const char *message)
{
    Client *sender;
    Client *recipient = getClientByUsername(username);
    if(recipient)
    {
        char whisperMsg[BUFFER_SIZE];
        snprintf(whisperMsg, sizeof(whisperMsg), "%s whispers: %s\n", senderUsername, message);
        send_message_protocol(recipient->socket, whisperMsg);
    }
    else
    {
        // Handle user not found
        char errorMsg[BUFFER_SIZE];
        snprintf(errorMsg, sizeof(errorMsg), "Error: User '%s' not found.\n", username);
        // Now find the sender's socket to send back the error message
        sender = getClientByUsername(senderUsername);
        if(sender)
        {
            send_message_protocol(sender->socket, errorMsg);
        }
        else
        {
            printf("Error: Sender '%s' not found.\n", senderUsername);
        }
    }
}

void sendHelp(int sockfd)
{
    char helpMessage[] = "Supported commands:\n"
                         "/u <username> - Set your username.\n"
                         "/ul - List all users connected to the server.\n"
                         "/w <username> <message> - Whisper a private message to <username>.\n"
                         "/h - Show this help message.\n"
                         "/e - Exit the connection.\n";
    send_message_protocol(sockfd, helpMessage);
}

void send_message_protocol(int sockfd, const char *message)
{
    ssize_t  total_size;
    char     buffer[BUFFER_SIZE];
    int      offset  = 0;
    uint8_t  version = 1;
    uint16_t size    = (uint16_t)(strlen(message));    // Convert message length to network byte order

    printf("Debug: Sending version: %u, message size: %u, message: %s\n", version, ntohs(size), message);

    memcpy(buffer + offset, &version, sizeof(version));
    offset += sizeof(version);

    memcpy(buffer + offset, &size, sizeof(size));
    offset += sizeof(size);

    memcpy(buffer + offset, message, ntohs(size));

    total_size = offset + ntohs(size);

    send(sockfd, buffer, (size_t)total_size, 0);
}

void trim_newline(char *str)
{
    char *pos;
    pos = strchr(str, '\n');
    if(pos != NULL)
    {
        *pos = '\0';    // Replace newline with null terminator
    }
}

void broadcastMessage(const char *senderUsername, const char *message)
{
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].is_active && strcmp(clients[i].username, senderUsername) != 0)
        {    // Don't send the message back to the sender
            char formattedMessage[BUFFER_SIZE];
            snprintf(formattedMessage, BUFFER_SIZE, "%s has sent: %s\n", senderUsername, message);

            // Debug log to verify formatted message
            printf("Broadcasting message: %s\n", formattedMessage);

            send_message_protocol(clients[i].socket, formattedMessage);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void addClient(int socket)
{
    bool found = false;
    pthread_mutex_lock(&clients_mutex);

    for(int i = 0; i < MAX_CLIENTS && !found; i++)
    {
        if(!clients[i].is_active)
        {
            clients[i].socket = socket;
            snprintf(clients[i].username, MAX_USERNAME_LENGTH, "client_%d", i);
            clients[i].is_active = true;
            printf("Client %d connected with username: %s\n", socket, clients[i].username);
            found = true;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    if(found)
    {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Connected clients: %d", countActiveClients());
        printf("Debug: %s\n", msg);    // Debug print for the server console
        notifyServerManagers(msg);
    }
}

void removeClient(int socket)
{
    bool found = false;
    pthread_mutex_lock(&clients_mutex);

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].socket == socket)
        {
            clients[i].is_active = false;
            found                = true;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    if(found)
    {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "Connected clients: %d", countActiveClients());
        printf("Debug: %s\n", msg);    // Debug print for the server console
        notifyServerManagers(msg);
    }
}

Client *getClientBySocket(int socket)
{
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].socket == socket && clients[i].is_active)
        {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void sigintHandler(int sig_num)
{
    // Signal handler for SIGINT
    (void)sig_num;

    printf("\nTerminating server...\n");
    if(server_socket >= 0)
    {
        close(server_socket);
        printf("Server socket closed.\n");
    }
    exit(EXIT_SUCCESS);
}

Client *getClientByUsername(const char *username)
{
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].is_active && strcmp(clients[i].username, username) == 0)
        {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

bool authenticate_server_manager(const char *password)
{
    return strcmp(password, SERVER_MANAGER_PASSWORD) == 0;
}

void disconnectAllClients(void)
{
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].is_active)
        {
            const char *msg = "Server is shutting down. Disconnecting...\n";
            send_message_protocol(clients[i].socket, msg);
            close(clients[i].socket);        // Close the client socket
            clients[i].is_active = false;    // Mark the client as inactive
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    printf("All clients have been disconnected.\n");
}

void *handle_server_manager(void *arg)
{
    int         client_socket = (int)(intptr_t)arg;
    char        content[BUFFER_SIZE];
    uint8_t     version;
    uint16_t    content_size_net;
    uint16_t    content_size;
    const char *accept_msg;
    ssize_t     bytes_received;
    const char *start_msg;
    const char *fail_msg;
    const char *stop_msg;

    while(!server_manager_authenticated)
    {
        // Read version
        if(recv(client_socket, &version, sizeof(version), 0) <= 0)
        {
            printf("Failed to read version\n");
            break;
        }

        // Read content size
        if(recv(client_socket, &content_size_net, sizeof(content_size_net), 0) <= 0)
        {
            printf("Failed to read content size\n");
            break;
        }
        content_size = ntohs(content_size_net);

        // Ensure that the content size is within buffer limits
        if(content_size > BUFFER_SIZE - 1)
        {
            printf("Message too large.\n");
            continue;
        }

        // Read content based on the received size
        bytes_received = recv(client_socket, content, content_size, 0);
        if(bytes_received <= 0)
        {
            printf("Failed to read content or server manager disconnected\n");
            break;
        }
        content[bytes_received] = '\0';    // Null-terminate the received content

        printf("Debug: Received password attempt: %s\n", content);
        if(!server_manager_authenticated && authenticate_server_manager(content))
        {
            server_manager_authenticated = true;
            accept_msg                   = "ACCEPTED";
            send_message_protocol(client_socket, accept_msg);
            // Mark this client as the server manager.
            pthread_mutex_lock(&clients_mutex);
            for(int i = 0; i < MAX_CLIENTS; i++)
            {
                if(clients[i].socket == client_socket)
                {
                    clients[i].is_server_manager = true;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }
        else
        {
            fail_msg = "Authentication failed. Try again.\n";
            send_message_protocol(client_socket, fail_msg);
        }
    }

    // Handle server manager commands.
    while(server_manager_authenticated)
    {
        printf("Listening for commands...\n");
        // Read version
        if(recv(client_socket, &version, sizeof(version), 0) <= 0)
        {
            printf("Failed to read version\n");
            break;
        }

        // Read content size
        if(recv(client_socket, &content_size_net, sizeof(content_size_net), 0) <= 0)
        {
            printf("Failed to read content size\n");
            break;
        }
        content_size = ntohs(content_size_net);

        // Ensure that the content size is within buffer limits
        if(content_size > BUFFER_SIZE - 1)
        {
            printf("Message too large.\n");
            continue;
        }

        // Read content based on the received size
        bytes_received = recv(client_socket, content, content_size, 0);
        if(bytes_received <= 0)
        {
            printf("Failed to read content or server manager disconnected\n");
            break;    // Break out of the loop on failure to read content
        }
        content[bytes_received] = '\0';    // Null-terminate the received content

        // Process the command
        if(strcmp(content, "/s") == 0)
        {
            server_operational = true;
            start_msg          = "STARTED";
            send_message_protocol(client_socket, start_msg);
            broadcastMessage("Server", "Server is now operational. Accepting client connections...");
            printf("Server is now operational. Accepting client connections...\n");
        }
        else if(strcmp(content, "/q") == 0)
        {
            server_operational = false;
            stop_msg           = "STOPPED";
            send_message_protocol(client_socket, stop_msg);
            printf("Server is shutting down by server manager command.\n");
            disconnectAllClients();
        }
    }
    return NULL;
}

int countActiveClients(void)
{
    int count = 0;
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].is_active)
        {
            count++;
        }
    }
    return count;
}

void notifyServerManagers(const char *message)
{
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].is_active && clients[i].is_server_manager)
        {
            printf("Debug: Notifying server manager (socket %d) - %s\n", clients[i].socket, message);
            send_message_protocol(clients[i].socket, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
