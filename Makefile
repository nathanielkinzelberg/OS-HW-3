CC      = gcc
CFLAGS  = -std=gnu17 -Wall -Wextra -g -I.
SRCS    = fs.c

all: test

test: test.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

run: test
	./test

clean:
	rm -f test *.o disk
	rm -rf *.dSYM

.PHONY: all run clean
