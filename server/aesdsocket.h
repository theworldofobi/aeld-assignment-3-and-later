#ifndef AESDSOCKET_H
#define AESDSOCKET_H

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdbool.h>

#define PORT      9000
#define BACKLOG   10
#define BUF_SIZE  1024
#define DATA_FILE "/var/tmp/aesdsocketdata"

#endif // AESDSOCKET_H
