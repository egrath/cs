CC      = cc
CFLAGS  = -std=c89 -pedantic -Wall
LDLIBS  = -lcurses -lm

SRC = src/cs.h src/cells.c src/parser.c src/eval.c src/screen.c \
      src/command.c src/help.c src/fileio.c src/print.c src/main.c

calcstar: $(SRC)
	$(CC) $(CFLAGS) src/*.c -o calcstar $(LDLIBS)

test: calcstar
	./calcstar --test

clean:
	rm -f calcstar *.o src/*.o

.PHONY: test clean
