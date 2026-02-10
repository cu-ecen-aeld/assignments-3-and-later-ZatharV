# Author: Atharv More

CROSS_COMPILE ?=

all: writer

default: writer

writer:
	$(CROSS_COMPILE)gcc -Wall -Werror writer.c -o writer

clean:
	rm -f writer *.o
