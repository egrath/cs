CC      = cc
CFLAGS  = -std=c89 -pedantic -Wall -I$(OUTDIR) -Wextra -pedantic-errors

# ULTRIX: the ancient cc stays unused - gcc understands our C89 flags,
# and -DULTRIX switches the curses inclusion to cursesX.h with the
# missing declarations (see cs.h).
UNAME   = $(shell uname -s)
ifeq ($(UNAME), ULTRIX)
	CC = gcc
	CFLAGS += -DULTRIX
endif
LDLIBS  = -lcurses
OUTDIR  = build

OBJS = $(OUTDIR)/num.o $(OUTDIR)/cells.o $(OUTDIR)/parser.o $(OUTDIR)/eval.o \
      $(OUTDIR)/screen.o $(OUTDIR)/command.o $(OUTDIR)/help.o \
      $(OUTDIR)/fileio.o $(OUTDIR)/print.o $(OUTDIR)/cs.o

SRCS = src/num.c src/cells.c src/parser.c src/eval.c src/screen.c \
      src/command.c src/help.c src/fileio.c src/print.c src/cs.c

$(OUTDIR)/calcstar: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OUTDIR)/%.o: src/%.c src/cs.h $(OUTDIR)/buildno.h
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# the build counter (CS_BUILDNO in $(OUTDIR)/buildno.h) increments on
# every SOURCE-CHANGING build: the generated header depends on the
# sources, so a no-op make does not touch it; build.count is created on
# first use and survives make clean.  (Keep this rule AFTER the default
# goal.)
$(OUTDIR)/buildno.h: $(SRCS) tools/buildno.sh
	@mkdir -p $(OUTDIR)
	@sh tools/buildno.sh $(OUTDIR)/buildno.h build.count

test: $(OUTDIR)/calcstar
	./$(OUTDIR)/calcstar --test

clean:
	rm -rf build build.gcc

.PHONY: test clean

# the gcc-14 second build (2026-11-22): the SAME sources and flags
# against gcc-14, objects in build.gcc/ - catches every warning the
# default cc (clang) build may accept.  The build counter is COPIED
# from the default build's header (no bump: one source change bumps
# build.count once, via the cc build).
gcc:
	@test -f build/buildno.h || $(MAKE) build/buildno.h
	@mkdir -p build.gcc
	@cp build/buildno.h build.gcc/buildno.h
	@touch build.gcc/buildno.h
	@$(MAKE) OUTDIR=build.gcc CC=gcc-14 build.gcc/calcstar
	./build.gcc/calcstar --test

.PHONY: gcc
