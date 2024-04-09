#include <arpa/inet.h>
#include <fcntl.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

// gcc -o testman main2.c -lncurses

#define MAX_MESSAGE_LENGTH 1024    // Buffer length
#define CURRENT_VERSION 1          // Current version of the protocol
#define BUFFER_SIZE 100            // Buffer size
#define ASCII_BACKSPACE 127        // ASCII value for backspace
#define ASCII_DELETE 8             // ASCII value for delete
#define DECIMAL 10                 // Decimal base

// #define PASSWORD_ACCEPTED "ACCEPTED"    // Password accepted message
// #define SERVER_STARTED "STARTED"        // Server started message
// #define SERVER_STOPPED "STOPPED"        // Server stopped message

#ifndef SOCK_CLOEXEC
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-macros"
    #define SOCK_CLOEXEC 0
    #pragma GCC diagnostic pop
#endif

typedef struct
{
    char *ipAddress;
    int   portNumber;
} ServerInfo;

typedef struct
{
    uint8_t  version;
    uint16_t contentLength;
    char    *content;
} Packet;

struct SharedData
{
    pthread_mutex_t mutex;
    pthread_cond_t  condVar;
    pthread_cond_t  diagnosticDataCond;
    int             diagnosticDataFlag;
    int             newDataFlag;
    char            newData[MAX_MESSAGE_LENGTH];    // Adjust buffer size as needed
};

struct ThreadArgs
{
    int                sockfd;
    struct SharedData *sharedData;
};

bool       checkIPAddress(char *ipAddress);
bool       verifyMessageFormat(Packet packet);
bool       startServer(int sockfd);
bool       stopServer(int sockfd);
char      *getInput(void);
void       printMenu(void);
void       sendToServer(int sockfd, const char *message);
void      *listenToServer(void *arg);
void      *printToScreen(void *arg);
int        connectToServer(char *ipAddress, int portNumber);
Packet     receiveFromServer(int sockfd);
ServerInfo getSocketInformation(void);

void *listenToServer(void *arg)
{
    /**
     * Listen to the server for diagnostic data
     */

    struct ThreadArgs *args       = (struct ThreadArgs *)arg;
    int                sockfd     = args->sockfd;
    struct SharedData *sharedData = args->sharedData;

    while(1)
    {
        Packet packet = receiveFromServer(sockfd);
        pthread_mutex_lock(&sharedData->mutex);
        strncpy(sharedData->newData, packet.content, sizeof(sharedData->newData));

        if((strcmp(packet.content, "STOPPED") == 0 || strcmp(packet.content, "STOPPED\n") == 0))
        {
            printw("beep boop Server stopped\n");
            sharedData->newDataFlag = 1;                  // Set the flag to indicate new data
            pthread_cond_signal(&sharedData->condVar);    // Signal the condition variable
        }
        else if((strcmp(packet.content, "STARTED") == 0 || strcmp(packet.content, "STARTED\n") == 0))
        {
            printw("beep boop Server started\n");
            sharedData->newDataFlag = 1;                  // Set the flag to indicate new data
            pthread_cond_signal(&sharedData->condVar);    // Signal the condition variable
        }
        else if((strcmp(packet.content, "ACCEPTED") == 0) || (strcmp(packet.content, "ACCEPTED\n") == 0))
        {
            printw("beep boop Password accepted\n");
            sharedData->newDataFlag = 1;                  // Set the flag to indicate new data
            pthread_cond_signal(&sharedData->condVar);    // Signal the condition variable
        }
        else
        {
            printw("\nFrom Server: %s\n", packet.content);
            // sharedData->diagnosticDataFlag = 1;    // Set the flag to indicate new data
            // pthread_cond_signal(&sharedData->diagnosticDataCond);    // Signal the condition variable
        }
        pthread_mutex_unlock(&sharedData->mutex);
    }
}

void *printToScreen(void *arg)
{
    /**
     * Print the diagnostic data to the screen
     */

    printw("Print to screen thread is here!\n");
    refresh();
    struct ThreadArgs *args       = (struct ThreadArgs *)arg;
    struct SharedData *sharedData = args->sharedData;
    while(1)
    {
        pthread_mutex_lock(&sharedData->mutex);
        while(sharedData->diagnosticDataFlag == 0)
        {
            pthread_cond_wait(&sharedData->diagnosticDataCond, &sharedData->mutex);
        }
        printw("Received diagnostic data 0000: %s\n", sharedData->newData);
        refresh();
        sharedData->diagnosticDataFlag = 0;
        sharedData->newData[0]         = '\0';
        pthread_mutex_unlock(&sharedData->mutex);
    }
}

bool verifyMessageFormat(Packet packet)
{
    /**
     * Verify that the message is in the correct format
     * packet: The packet to verify
     * Return True if the message is in the correct format, False otherwise
     */
    printw("Verifying message format...\n");
    printw("Version: %d\n", packet.version);
    printw("Content Length: %d\n", packet.contentLength);
    printw("Content: %s\n", packet.content);

    return ((packet.version == CURRENT_VERSION) && (packet.contentLength <= MAX_MESSAGE_LENGTH) && (packet.contentLength == strlen(packet.content)) && (packet.content != NULL));
}

bool checkIPAddress(char *ipAddress)
{
    /**
     * Check if the given string ipAddress is a valid IP address
     * ipAddress: The string to check
     * return True if the string is a valid IP address, False otherwise
     */

    struct sockaddr_in socketAddress;
    return inet_pton(AF_INET, ipAddress, &(socketAddress.sin_addr)) != 0;
}

void sendToServer(int sockfd, const char *message)
{
    /**
     * Send a message to the server
     * sockfd: The socket file descriptor
     * message: The message to send
     */
    uint8_t  version_packet;
    uint16_t content_length_packet;

    printw("Debug:Sending message: %s\n", message);

    // Create the version packet
    version_packet = (uint8_t)CURRENT_VERSION;
    send(sockfd, &version_packet, sizeof(version_packet), 0);
    printw("Debug:Version packet: %d\n", version_packet);

    // Create the content length packet
    // printw("Debug:Content length packet: %d\n", strlen(message));

    content_length_packet = htons(strlen(message));
    send(sockfd, &content_length_packet, sizeof(content_length_packet), 0);
    printw("Debug:Content length packet: %d\n", content_length_packet);

    // Create the content packet
    send(sockfd, message, strlen(message), 0);
    printw("Debug:Content packet: %s\n", message);

}

Packet receiveFromServer(int sockfd)
{
    /**
     * Receive the password acknowledgement from the server
     * sockfd: The socket file descriptor
     */
    Packet   packet;
    uint8_t  version;
    uint16_t contentLength;

    // Receive the version packet
    printw("Receiving packet... HAAHAHA\n");
    if(recv(sockfd, &version, sizeof(version), 0) <= 0)
    {
        printw("Error receiving version packet\n");
        perror("recv11");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    else
    {
        packet.version = version;
        printw("Debug: Received version: %d\n", packet.version);
    }

    // Receive the content length packet
    if(recv(sockfd, &contentLength, sizeof(contentLength), 0) <= 0)
    {
        perror("recv22");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    else
    {
        packet.contentLength = ntohs(contentLength);
        printw("Debug: Received Content Length: %d\n", packet.contentLength);
    }

    // Allocate memory for the content
    packet.content = (char *)malloc(packet.contentLength + 1);
    if(packet.content == NULL)
    {
        perror("malloc");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    // Receive the content packet
    if(recv(sockfd, packet.content, packet.contentLength, 0) <= 0)
    {
        perror("recv33");
        close(sockfd);
        free(packet.content);
        exit(EXIT_FAILURE);
    }
    packet.content[packet.contentLength] = '\0';
    printw("Debug: Received Content: %s\n", packet.content);

    return packet;
}

int connectToServer(char *ipAddress, int portNumber)
{
    /**
     * Connect to the server at the given IP address
     * ipAddress: The IP address of the server
     * Return sockfd if successful, 0 otherwise
     */

    struct sockaddr_in server_addr;

    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(sockfd == -1)
    {
        perror("socket");
        return 0;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(portNumber);
    if(inet_pton(AF_INET, ipAddress, &(server_addr.sin_addr)) <= 0)
    {
        printw("inet_pton Error.\n");
        close(sockfd);
        return 0;
    }

    if(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        printw("Connection failed\n");
        close(sockfd);
        return 0;
    }

    printw("Connection successful\n");
    return sockfd;
}

char *getInput(void)
{
    /**
     * Get input from the user
     * Return The input string
     */
    nodelay(stdscr, TRUE);

    char *input;
    int   index = 0;
    int   ch;

    input = (char *)malloc(BUFFER_SIZE * sizeof(char));

    if(input == NULL)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    while(1)
    {
        ch = getch();
        if(ch != ERR && ch != '\n')    // Check if a character was read and it's not newline
        {
            if(ch == KEY_BACKSPACE || ch == ASCII_BACKSPACE || ch == ASCII_DELETE)
            {    // Backspace or Delete
                if(index > 0)
                {
                    index--;
                    printw("\b \b");    // Move cursor back, print space, move back again (backspace effect)
                    refresh();
                }
            }
            else if(index < BUFFER_SIZE - 1)
            {    // Limit input to avoid buffer overflow
                input[index++] = (char)ch;
                printw("%c", ch);    // Echo the character back to the screen
                refresh();
            }
        }
        else if(ch == '\n')
        {
            break;    // Exit the loop when Enter key is pressed
        }
    }
    input[index] = '\0';    // Null-terminate the string
    return input;
}

void printMenu(void)
{
    /**
     * Print the menu options
     */
    printw("\n1. Connect to server\n");
    printw("2. Start the server\n");
    printw("3. Stop the server\n");
    printw("4. Exit the program\n");
    printw("Enter your choice: ");
    refresh();
}

ServerInfo getSocketInformation(void)
{
    /**
     * Get the IP address and port number of the server
     * Return The server information
     */
    char      *ipAddress;
    bool       isValidAddress;
    char      *portNumberStr;
    int        portNumber = 0;
    char      *endPtr;
    long int   portValue;
    ServerInfo serverInfo;

    // Print a message and wait for user input
    while(1)
    {
        printw("Enter the server IP address: ");
        ipAddress      = getInput();
        isValidAddress = checkIPAddress(ipAddress);
        if(isValidAddress)
        {
            printw("\nThe IP address %s is a valid IP address.\n", ipAddress);
            serverInfo.ipAddress = ipAddress;
            break;
        }
        else
        {
            printw("\nThe IP address %s is not a valid IP address.\n", ipAddress);
        }
    }

    // Get the port number
    printw("Enter the port number: ");
    portNumberStr = getInput();
    // Convert the port number to an integer
    portValue = strtol(portNumberStr, &endPtr, DECIMAL);

    // Check for conversion errors
    if(endPtr == portNumberStr || *endPtr != '\0')
    {
        // Handle conversion error
        printw("Error: Invalid port number\n");
    }
    else
    {
        portNumber = (int)portValue;
    }
    printw("\nYou entered: %d\n", portNumber);
    serverInfo.portNumber = portNumber;
    free(portNumberStr);
    return serverInfo;
}

int main(void)
{
    int               sockfd;
    int               portNumber;
    char             *ipAddress;
    ServerInfo        serverInfo;
    bool              running       = true;
    bool              serverRunning = false;
    pthread_t         listenThread;
    pthread_t         printThread;
    struct SharedData sharedData;
    struct ThreadArgs args;

    // pthread_t  printThread;
    // nodelay(stdscr, TRUE);

    initscr();               // Initialize the screen
    cbreak();                // Line buffering disabled, Pass on every character
    noecho();                // Don't echo while we do getch
    keypad(stdscr, TRUE);    // Enable keypad input
    // WINDOW *myWin = newwin(10, 40, 5, 5); // Create a window
    // scrollok(myWin, TRUE);                // Enable scrolling for the window

    pthread_mutex_init(&sharedData.mutex, NULL);
    pthread_cond_init(&sharedData.condVar, NULL);
    sharedData.newDataFlag = 0;

    printw("COMP 4985 Project: Server Manager Program\n");

    sockfd = 0;

    while(sockfd == 0)
    {
        serverInfo = getSocketInformation();
        ipAddress  = serverInfo.ipAddress;
        portNumber = serverInfo.portNumber;
        sockfd     = connectToServer(ipAddress, portNumber);
    }

    printw("\nTalking with server.\n");

    args.sockfd     = sockfd;
    args.sharedData = &sharedData;    // Pass a pointer to sharedData to the listening thread

    if(pthread_create(&listenThread, NULL, (void *(*)(void *))listenToServer, (void *)&args) != 0)
    {
        perror("pthread_create");
        exit(EXIT_FAILURE);
    }
    else
    {
        printw("Listening thread created\n");
    }

    // if(pthread_create(&printThread, NULL, (void* (*)(void*))printToScreen, (void *)&args) != 0)
    // {
    //     perror("pthread_create");
    //     exit(EXIT_FAILURE);
    // }
    // else
    // {
    //     printw("Print thread created\n");
    // }

    while(running)
    {
        int   choice;
        char *input;
        printMenu();
        input  = getInput();
        choice = input[0] - '0';

        switch(choice)
        {
            case 1:
            {
                char *password;    // Declare password variable

                printw("Connecting to server...\n");
                printw("Enter the password for the server: ");
                password = getInput();
                sendToServer(sockfd, password);
                pthread_mutex_lock(&sharedData.mutex);
                printw("Flag value: %d\n", sharedData.newDataFlag);
                while(sharedData.newDataFlag == 0)
                {
                    pthread_cond_wait(&sharedData.condVar, &sharedData.mutex);
                }
                if((strcmp(sharedData.newData, "ACCEPTED") == 0) || (strcmp(sharedData.newData, "ACCEPTED\n") == 0))
                {
                    printw("Password accepted\n");
                }
                else
                {
                    printw("Password rejected\n");
                }
                sharedData.newDataFlag = 0;
                pthread_mutex_unlock(&sharedData.mutex);
                free(password);
                break;
            }
            case 2:
            {
                printw("Starting the server...\n");
                if(sockfd == 0)
                {
                    printw("Please connect to the server first\n");
                }
                else if(serverRunning)
                {
                    printw("The server is already running\n");
                }
                else
                {
                    sendToServer(sockfd, "/s");
                    pthread_mutex_lock(&sharedData.mutex);
                    while(sharedData.newDataFlag == 0)
                    {
                        pthread_cond_wait(&sharedData.condVar, &sharedData.mutex);
                    }
                    // if(sharedData.newDataFlag)
                    // {
                    if((strcmp(sharedData.newData, "STARTED") == 0) || (strcmp(sharedData.newData, "STARTED\n") == 0))
                    {
                        printw("Server started\n");
                        serverRunning = true;
                    }
                    else
                    {
                        printw("Server failed to start\n");
                    }
                    sharedData.newDataFlag = 0;
                    // }
                    pthread_mutex_unlock(&sharedData.mutex);
                }
                break;
            }
            case 3:
            {
                printw("Stopping the server...\n");
                if(sockfd == 0)
                {
                    printw("Please connect to the server first\n");
                }
                else if(!serverRunning)
                {
                    printw("The server is already stopped\n");
                }
                else
                {
                    sendToServer(sockfd, "/q");
                    pthread_mutex_lock(&sharedData.mutex);
                    while(sharedData.newDataFlag == 0)
                    {
                        pthread_cond_wait(&sharedData.condVar, &sharedData.mutex);
                    }
                    // if(sharedData.newDataFlag)
                    // {
                    if((strcmp(sharedData.newData, "STOPPED") == 0) || (strcmp(sharedData.newData, "STOPPED\n") == 0))
                    {
                        printw("Server stopped\n");
                        serverRunning = false;
                    }
                    else
                    {
                        printw("Server failed to stop\n");
                    }
                    sharedData.newDataFlag = 0;
                    // }
                    pthread_mutex_unlock(&sharedData.mutex);
                }
                break;
            }
            case 4:
            {
                printw("Exiting the program...\n");
                close(sockfd);
                sockfd = 0;
                free(ipAddress);
                pthread_kill(listenThread, 0);
                // pthread_kill(printThread, 0);
                pthread_mutex_destroy(&sharedData.mutex);
                pthread_cond_destroy(&sharedData.condVar);
                running = false;
                break;
            }
            default:
            {
                printw("Invalid choice\n");
            }
        }
        free(input);
    }

    printw("Press any key to exit...\n");
    getch();
    endwin();
    return 0;
}
