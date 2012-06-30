CC=gcc
CFLAGS=-Wall -O2 -ansi -c

all: odd

odd: odd.o
	$(CC) odd.o -o odd

odd.o: odd.c
	$(CC) $(CFLAGS) -c odd.c

clean:
	rm -rf odd.o

