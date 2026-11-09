CC      = cc
CFLAGS  = -std=c89 -pedantic -Wall
LDLIBS  = -lcurses -lm

OBJS = build/num.o build/cells.o build/parser.o build/eval.o build/screen.o \
      build/command.o build/help.o build/fileio.o build/print.o \
      build/main.o

build/calcstar: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/%.o: src/%.c src/cs.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

test: build/calcstar
	./build/calcstar --test

clean:
	rm -rf build

.PHONY: test clean
