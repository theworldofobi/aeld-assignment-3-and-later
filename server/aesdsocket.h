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
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <sys/queue.h>

#define PORT      9000
#define BACKLOG   10
#define BUF_SIZE  1024

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATA_FILE       "/dev/aesdchar"
#define DATA_OPEN_FLAGS (O_WRONLY)
#else
#define DATA_FILE       "/var/tmp/aesdsocketdata"
#define DATA_OPEN_FLAGS (O_WRONLY | O_CREAT | O_APPEND)
#endif
#define PID_FILE  "/var/run/aesdsocket.pid"

struct thread_node {
  pthread_t thread;
  int client_fd;
  char ip[INET_ADDRSTRLEN];
  atomic_bool done;
  SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list, thread_node);

#endif // AESDSOCKET_H
