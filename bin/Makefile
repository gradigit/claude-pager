CC      = clang
CFLAGS  = -O2 -Wall -Wextra
BINARY  = claude-pager-open

$(BINARY): claude-pager-open.c
	$(CC) $(CFLAGS) -o $(BINARY) claude-pager-open.c

install: $(BINARY)
	@echo "Built: $$(pwd)/$(BINARY)"
	@echo "Set your Claude Code editor in settings.json:"
	@echo '  { "editor": "$$(pwd)/$(BINARY)" }'

clean:
	rm -f $(BINARY)

.PHONY: install clean
