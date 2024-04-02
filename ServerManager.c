#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_MESSAGE_LENGTH 1024
#define CURRENT_VERSION 1
#define PASSWORD_ACCEPTED "ACCEPTED"
#define SERVER_STARTED "STARTED"
#define SERVER_STOPPED "STOPPED"


typedef struct {
    char* ipAddress;
    int portNumber;
} ServerInfo;

typedef struct {
    uint8_t version;
    uint16_t contentLength;
    char *content;
} Packet;

struct SharedData {
    pthread_mutex_t mutex;
    pthread_cond_t condVar;
    int newDataFlag;
    char newData[MAX_MESSAGE_LENGTH]; // Adjust buffer size as needed
};

struct ThreadArgs {
    int sockfd;
    struct SharedData *sharedData;
};
bool checkIPAddress(char *ipAddress);
bool verifyMessageFormat(Packet packet);
void sendToServer(int sockfd, char *message);
int connectToServer(char *ipAddress, int portNumber);
bool startServer(int sockfd);
bool stopServer(int sockfd);
char* getInput();
void printMenu();
Packet receiveFromServer(int sockfd);
ServerInfo getSocketInformation();


void* listenToServer(void* arg) {
    /**
     * Listen to the server for diagnostic data
    */

    struct ThreadArgs *args = (struct ThreadArgs *)arg;
    int sockfd = args->sockfd;
    struct SharedData *sharedData = args->sharedData;
    char buffer[1024]; // Adjust buffer size as needed
    int bytesReceived;

    while (1) {
        Packet packet = receiveFromServer(sockfd);
        pthread_mutex_lock(&sharedData->mutex);
        strncpy(sharedData->newData, packet.content, sizeof(sharedData->newData));
        sharedData->newDataFlag = 1; // Set the flag to indicate new data
        pthread_cond_signal(&sharedData->condVar); // Signal the condition variable
        pthread_mutex_unlock(&sharedData->mutex);

        if (strcmp(packet.content, "STOPPED") == 0) {
            printw("beep boop Server stopped\n");
        } else if (strcmp(packet.content, "STARTED") == 0) {
            printw("beep boop Server started\n");
        } else if (strcmp(packet.content, "ACCEPTED") == 0) {
            printw("beep boop Password accepted\n");
        } else {
            printw("Received diagnostic data: %s\n", packet.content);
            sharedData->newDataFlag = 0;
            sharedData->newData[0] = '\0';
        }
    }
    pthread_exit(NULL);
}

bool verifyMessageFormat(Packet packet) {
    /**
     * Verify that the message is in the correct format
     * @param packet: The packet to verify
     * @return: True if the message is in the correct format, False otherwise
    */
    printw("Verifying message format...\n");
    printw("Version: %d\n", packet.version);
    printw("Content Length: %d\n", packet.contentLength);
    printw("Content: %s\n", packet.content);

    if ((packet.version == CURRENT_VERSION) &&
        (packet.contentLength <= MAX_MESSAGE_LENGTH) &&
        (packet.contentLength == strlen(packet.content)) &&
        (packet.content != NULL)) {
        return true;
    } else {
        return false;
    }
}

bool checkIPAddress(char *ipAddress) {
    /**
     * Check if the given string ipAddress is a valid IP address
     * @param ipAddress: The string to check
     * @return: True if the string is a valid IP address, False otherwise
    */

    struct sockaddr_in socketAddress;
    return inet_pton(AF_INET, ipAddress, &(socketAddress.sin_addr)) != 0;
}

void sendToServer(int sockfd, char *message) {
    /**
     * Send a message to the server
     * @param sockfd: The socket file descriptor
     * @param message: The message to send
    */

    int version = CURRENT_VERSION;
    int contentLength = strlen(message);
    char *packet = (char *)malloc(MAX_MESSAGE_LENGTH * sizeof(char));

    // Create the version packet
    uint8_t version_packet = CURRENT_VERSION;
    send(sockfd, &version_packet, sizeof(version_packet), 0);

    // Create the content length packet
    uint16_t content_length_packet = strlen(message);
    send(sockfd, &content_length_packet, sizeof(content_length_packet), 0);

    // Create the content packet
    send(sockfd, message, strlen(message), 0);
}

Packet receiveFromServer(int sockfd) {
    /**
     * Receive the password acknowledgement from the server
     * @param sockfd: The socket file descriptor
    */
    Packet packet;
    uint8_t version;
    uint16_t contentLength;

    // Receive the version packet
    if (recv(sockfd, &version, sizeof(version), 0) <= 0) {
        printw("Error receiving version packet\n");
        perror("recv11");
        close(sockfd);
        exit(EXIT_FAILURE);
    } else {
        packet.version = version;
        printw("Version: %d\n", packet.version);
    }

    // Receive the content length packet
    if (recv(sockfd, &contentLength, sizeof(contentLength), 0) <= 0) {
        perror("recv22");
        close(sockfd);
        exit(EXIT_FAILURE);
    } else {
        packet.contentLength = contentLength;
        printw("Content Length: %d\n", packet.contentLength);
    }

    // Allocate memory for the content
    packet.content = (char *)malloc(packet.contentLength + 1);
    if (packet.content == NULL) {
        perror("malloc");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    printw("Content: %s\n", packet.content);

    // Receive the content packet
    if (recv(sockfd, packet.content, packet.contentLength, 0) <= 0) {
        perror("recv33");
        close(sockfd);
        free(packet.content);
        exit(EXIT_FAILURE);
    }
    packet.content[packet.contentLength] = '\0';

    return packet;
}

int connectToServer(char *ipAddress, int portNumber) {
    /**
     * Connect to the server at the given IP address
     * @param ipAddress: The IP address of the server
     * @return: sockfd if successful, 0 otherwise
    */

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return 0;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portNumber);
    if (inet_pton(AF_INET, ipAddress, &(server_addr.sin_addr)) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return 0;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        close(sockfd);
        return 0;
    }

    printw("Connection successful\n");
    return sockfd;
}



char* getInput() {
    /**
     * Get input from the user
     * @return: The input string
    */
    char *input = (char *)malloc(100 * sizeof(char));
    if (input == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    int index = 0;
    int ch;

    while ((ch = getch()) != '\n') {
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { // Backspace or Delete
            if (index > 0) {
                index--;
                printw("\b \b"); // Move cursor back, print space, move back again (backspace effect)
                refresh();
            }
        } else if (index < 99) { // Limit input to avoid buffer overflow
            input[index++] = ch;
            printw("%c", ch); // Echo the character back to the screen
            refresh();
        }
    }
    input[index] = '\0'; // Null-terminate the string
    return input;
}

void printMenu() {
    /**
     * Print the menu options
    */
    printw("\n1. Connect to server\n");
    printw("2. Start the server\n");
    printw("3. Stop the server\n");
    printw("4. Exit the program\n");
    printw("Enter your choice: ");
}

ServerInfo getSocketInformation() {
    /**
     * Get the IP address and port number of the server
     * @return: The server information
    */
    char *ipAddress;
    char *portNumberStr;
    int portNumber;
    ServerInfo serverInfo;

    // Print a message and wait for user input
    printw("Enter the server IP address: ");
    ipAddress = getInput();
    printw("\nYou entered: %s\n", ipAddress);
    printw("\nThe IP address %s is a %s!\n", ipAddress, checkIPAddress(ipAddress) ? "Valid IP Address" : "Invalid IP Address");
    serverInfo.ipAddress = ipAddress;

    // Get the port number
    printw("Enter the port number: ");
    portNumberStr = getInput();

    // Convert the port number to an integer
    portNumber = atoi(portNumberStr);
    printw("\nYou entered: %d\n", portNumber);
    serverInfo.portNumber = portNumber;

    return serverInfo;
}


int main() {
    initscr();  // Initialize the screen
    cbreak();   // Line buffering disabled, Pass on every character
    noecho();   // Don't echo while we do getch
    keypad(stdscr, TRUE); // Enable keypad input
    int sockfd = 0;
    char *ipAddress;
    int portNumber;
    ServerInfo serverInfo;
    bool running = true;
    bool serverRunning = false;
    pthread_t listenThread;

    struct SharedData sharedData;
    pthread_mutex_init(&sharedData.mutex, NULL);
    pthread_cond_init(&sharedData.condVar, NULL);
    sharedData.newDataFlag = 0;


    printw("COMP 4985 Project: Server Manager Program\n");

    serverInfo = getSocketInformation();
    ipAddress = serverInfo.ipAddress;
    portNumber = serverInfo.portNumber;
    sockfd = connectToServer(ipAddress, portNumber);

    if (sockfd != 0) {
        printw("\nTalking with server.\n");
        struct ThreadArgs args;
        args.sockfd = sockfd;
        args.sharedData = &sharedData; // Pass a pointer to sharedData to the listening thread
        if (pthread_create(&listenThread, NULL, (void* (*)(void*))listenToServer, (void *)&args) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        } else {
            printw("Listening thread created\n");
        }
    }

    while (running) {
        printMenu();
        int choice = getInput()[0] - '0';

        switch(choice) {
            case 1:
                printw("Connecting to server...\n");
                printw("Enter the password for the server: ");
                char *password = getInput();
                sendToServer(sockfd, password);
                pthread_mutex_lock(&sharedData.mutex);
                printw("Flag value: %d\n", sharedData.newDataFlag);
                while (sharedData.newDataFlag == 0) {
                    pthread_cond_wait(&sharedData.condVar, &sharedData.mutex);
                }
                if (sharedData.newDataFlag) {
                    if (strcmp(sharedData.newData, "ACCEPTED") == 0) {
                        printw("Password accepted\n");
                    } else {
                        printw("Password rejected\n");
                    }
                    sharedData.newDataFlag = 0;
                }
                pthread_mutex_unlock(&sharedData.mutex);
                break;
            case 2:
                printw("Starting the server...\n");
                if (sockfd == 0) {
                    printw("Please connect to the server first\n");
                } else if (serverRunning) {
                    printw("The server is already running\n");
                } else {
                    sendToServer(sockfd, "/s");
                    pthread_mutex_lock(&sharedData.mutex);
                    while (sharedData.newDataFlag == 0) {
                        pthread_cond_wait(&sharedData.condVar, &sharedData.mutex);
                    }
                    if (sharedData.newDataFlag) {
                        if (strcmp(sharedData.newData, "STARTED") == 0) {
                            printw("Server started\n");
                            serverRunning = true;
                        } else {
                            printw("Server failed to start\n");
                        }
                        sharedData.newDataFlag = 0;
                    }
                    pthread_mutex_unlock(&sharedData.mutex);
                }
                break;
            case 3:
                printw("Stopping the server...\n");
                if (sockfd == 0) {
                    printw("Please connect to the server first\n");
                } else if (!serverRunning) {
                    printw("The server is already stopped\n");
                }
                else {
                    sendToServer(sockfd, "/q");
                    pthread_mutex_lock(&sharedData.mutex);
                    while (sharedData.newDataFlag == 0) {
                        pthread_cond_wait(&sharedData.condVar, &sharedData.mutex);
                    }
                    if (sharedData.newDataFlag) {
                        if (strcmp(sharedData.newData, "STOPPED") == 0) {
                            printw("Server stopped\n");
                            serverRunning = false;
                        } else {
                            printw("Server failed to stop\n");
                        }
                        sharedData.newDataFlag = 0;
                    }
                    pthread_mutex_unlock(&sharedData.mutex);
                }
                break;
            case 4:
                printw("Exiting the program...\n");
                close(sockfd);
                sockfd = 0;
                free(ipAddress);
                pthread_join(listenThread, NULL);
                pthread_mutex_destroy(&sharedData.mutex);
                pthread_cond_destroy(&sharedData.condVar);
                running = false;
                break;
            default:
                printw("Invalid choice\n");
        }
    }

    printw("Press any key to exit...\n");
    getch();
    endwin();
    return 0;
}