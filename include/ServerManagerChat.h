#ifndef COMP4985A1_SERVERMANAGERCHAT_H
#define COMP4985A1_SERVERMANAGERCHAT_H

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

#endif    // COMP4985A1_SERVERMANAGERCHAT_H
