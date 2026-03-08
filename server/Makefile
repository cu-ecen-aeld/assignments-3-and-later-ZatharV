# Author: Atharv More
# aesdsocket makefile

CC = $(CROSS_COMPILE)gcc
# FIX: Explicitly define USE_AESD_CHAR_DEVICE=1 for Assignment 8
CFLAGS = -Wall -Werror -g -O0 -DUSE_AESD_CHAR_DEVICE=1
LDFLAGS ?= -pthread

all: aesdsocket

default: aesdsocket

aesdsocket:
	$(CC) $(CFLAGS) aesdsocket.c -o aesdsocket $(LDFLAGS)

clean:
	rm -f aesdsocket *.o
