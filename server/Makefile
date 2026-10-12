CC = $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror

all: aesdsocket

default: aesdsocket

aesdsocket: aesdsocket.o
	$(CC) $(CFLAGS) -o aesdsocket aesdsocket.o -pthread

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -pthread -c aesdsocket.c

clean:
	rm -f aesdsocket *.o

.PHONY: all clean default
