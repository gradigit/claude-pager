CC      = clang
CFLAGS  = -O2 -Wall -Wextra
BINARY  = claude-pager-open
SRCS    = claude-pager-open.c pager.c
OBJS    = claude-pager-open.o pager.o

$(BINARY): $(OBJS)
	$(CC) $(CFLAGS) -o $(BINARY) $(OBJS)

%.o: %.c pager.h
	$(CC) $(CFLAGS) -c $<

install: $(BINARY)
	@echo "Built: $$(pwd)/$(BINARY)"

clean:
	rm -f $(BINARY) $(OBJS)

.PHONY: install clean
