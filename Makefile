CC      = cc
CFLAGS  = -std=c89 -pedantic -Wall -Ibuild
LDLIBS  = -lcurses -lm

OBJS = build/num.o build/cells.o build/parser.o build/eval.o build/screen.o \
      build/command.o build/help.o build/fileio.o build/print.o \
      build/main.o

SRCS = src/num.c src/cells.c src/parser.c src/eval.c src/screen.c \
      src/command.c src/help.c src/fileio.c src/print.c src/main.c

build/calcstar: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/%.o: src/%.c src/cs.h build/buildno.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# the build counter (CS_BUILDNO in build/buildno.h) increments on every
# SOURCE-CHANGING build: the generated header depends on the sources, so
# a no-op make does not touch it; build.count is created on first use
# and survives make clean.  (Keep this rule AFTER the default goal.)
build/buildno.h: $(SRCS) tools/buildno.sh
	@mkdir -p build
	@sh tools/buildno.sh build/buildno.h build.count

test: build/calcstar
	./build/calcstar --test

clean:
	rm -rf build

.PHONY: test clean
