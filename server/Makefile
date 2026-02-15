# Author: Atharv More
#aesdsocket makefile

CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g -O0

all: aesdsocket

default: aesdsocket

aesdsocket:
	$(CC) $(CFLAGS) aesdsocket.c -o aesdsocket

clean:
	rm -f aesdsocket *.o