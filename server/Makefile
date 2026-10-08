CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror

default: all

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CFLAGS) -o aesdsocket aesdsocket.c $(LDFLAGS)

clean:
	rm -f aesdsocket *.o
