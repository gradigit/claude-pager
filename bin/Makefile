CC      = clang
CFLAGS  = -O2 -Wall -Wextra

BINARIES = claude-pager-open claude-pager-c
COMMON   = pager.o

claude-pager-open: claude-pager-open.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ claude-pager-open.o $(COMMON)

claude-pager-c: pager_cli.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ pager_cli.o $(COMMON)

%.o: %.c pager.h
	$(CC) $(CFLAGS) -c $<

all: $(BINARIES)

install: all
	@echo "Built: $$(pwd)/claude-pager-open"
	@echo "Built: $$(pwd)/claude-pager-c"

clean:
	rm -f $(BINARIES) *.o

.PHONY: all install clean
.DEFAULT_GOAL := all
