CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
TARGET ?= aesdsocket
LDFLAGS ?= -pthread -lrt
USE_AESD_CHAR_DEVICE ?= 1
DEFINES = -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)

all: aesdsocket

default: aesdsocket

aesdsocket: aesdsocket.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o aesdsocket aesdsocket.o

aesdsocket.o: aesdsocket.c aesdsocket.h
	$(CC) $(CFLAGS) $(DEFINES) -c aesdsocket.c

clean:
	rm -f aesdsocket *.o

.PHONY: all clean default
