# Author: Atharv More

CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror

all: writer

default: writer

writer:
	$(CC) $(CFLAGS) writer.c -o writer

clean:
	rm -f writer *.o
