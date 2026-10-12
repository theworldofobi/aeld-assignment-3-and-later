CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
TARGET ?= aesdsocket
LDFLAGS ?= -pthread -lrt

all: aesdsocket

default: aesdsocket

aesdsocket: aesdsocket.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o aesdsocket aesdsocket.o

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c aesdsocket.c

clean:
	rm -f aesdsocket *.o

.PHONY: all clean default
