# claude-pager

A scrollable terminal pager for Claude Code session transcripts. Press **Ctrl-G** in Claude Code and your conversation history renders in the terminal while your GUI editor is open.

The pager is a single compiled C binary — no Python, no Node, no runtime dependencies. Full transcript render in ~5ms.

## Features

- Scrollable viewport with mouse wheel and keyboard navigation
- Markdown rendering: headings, bold, inline code, code blocks, lists
- Diff coloring (+green / -red / @@cyan)
- Context usage bar showing token consumption
- Live-follow mode: content updates as the transcript grows
- Terminal resize support (SIGWINCH)
- TurboDraft fast path: talks directly to TurboDraft's Unix socket, bypassing all shell overhead

## Requirements

- macOS (arm64 or x86_64)
- A GUI editor for Claude Code's Ctrl-G feature

## Install

### Option A: Use a pre-built binary (macOS arm64)

```sh
curl -L https://github.com/gradigit/claude-pager/releases/latest/download/claude-pager-open-arm64 \
  -o /usr/local/bin/claude-pager-open
chmod +x /usr/local/bin/claude-pager-open
```

### Option B: Build from source

```sh
git clone https://github.com/gradigit/claude-pager.git
cd claude-pager/bin
make
```

This produces `bin/claude-pager-open` (~70KB, zero dependencies).

## Claude Code Setup

### 1. Point your editor to the binary

Create or edit `~/.claude/editor-shim.sh`:

```sh
#!/usr/bin/env bash
exec /path/to/claude-pager-open "$@"
```

Then set it as your editor in your shell config:

```sh
# fish
set -gx VISUAL ~/.claude/editor-shim.sh
set -gx EDITOR ~/.claude/editor-shim.sh

# bash/zsh
export VISUAL=~/.claude/editor-shim.sh
export EDITOR=~/.claude/editor-shim.sh
```

### 2. Install the session hook

The included hook ensures the pager finds the correct transcript, even with multiple Claude sessions.

Add to `~/.claude/settings.json`:

```json
{
  "hooks": {
    "SessionStart": [
      {
        "type": "command",
        "command": "/path/to/claude-pager/shim/save-session-transcript.sh"
      }
    ]
  }
}
```

Without this hook, the pager falls back to finding the most recent transcript in your project directory.

### 3. TurboDraft users

No extra configuration needed. The binary detects TurboDraft's Unix socket at `~/Library/Application Support/TurboDraft/turbodraft.sock` and talks to it directly — no osascript, no `turbodraft-open`, no shell overhead.

If TurboDraft is not running, it falls back to the bash shim (`shim/claude-pager-shim.sh`) which works with any GUI editor.

### Other editors (VS Code, Sublime, etc.)

Set `CLAUDE_PAGER_EDITOR` in Claude Code's env settings:

```json
{
  "env": {
    "CLAUDE_PAGER_EDITOR": "code -w"
  }
}
```

The bash shim (`shim/claude-pager-shim.sh`) launches your editor and starts the Python pager in the background. To use the bash shim directly instead of the C binary, point your editor shim at it:

```sh
#!/usr/bin/env bash
exec /path/to/claude-pager/shim/claude-pager-shim.sh "$@"
```

The bash shim requires Python 3.9+ and `pip install claude-pager`.

## Key Bindings

| Key | Action |
|---|---|
| Scroll wheel | Scroll up/down |
| Arrow Up/Down | Scroll one line |
| Page Up/Down | Scroll one page |
| Home | Jump to top |
| End | Jump to bottom |
| q | Quit pager |

## How It Works

When you press Ctrl-G in Claude Code:

1. Claude Code opens an alt screen and spawns the editor shim
2. The C binary finds your session transcript via a tty-keyed temp file (~0.1ms)
3. It connects to TurboDraft's socket and sends `session.open` (~0.02ms)
4. It forks and renders the pager directly in C (~3ms for pre-render, ~5ms for full transcript)
5. TurboDraft opens the file (~120-180ms) — the pager is already visible
6. On close: the binary receives `session.wait` response, kills the pager, and exits

The pager uses alternate scroll mode (`\033[?1007h`) instead of mouse tracking, so OSC-8 hyperlinks remain Cmd+clickable.

## Architecture

```
claude-pager-open (C binary, ~70KB)
├── TurboDraft socket client (JSON-RPC 2.0 over Unix domain socket)
├── Transcript parser (minimal JSON scanner, single-pass JSONL)
├── Markdown renderer (ANSI escape codes)
├── Scrollable viewport (raw terminal mode, keyboard/mouse input)
└── Fallback: shim/claude-pager-shim.sh (bash + Python, for non-TurboDraft editors)
```

## Development

```sh
git clone https://github.com/gradigit/claude-pager.git
cd claude-pager/bin
make            # builds claude-pager-open
make clean      # removes build artifacts
```

The C source is in `bin/claude-pager-open.c` (socket + fork logic) and `bin/pager.c` (pager rendering).

The Python pager (`src/claude_pager/`) is used by the bash shim fallback path:

```sh
pip install -e '.[highlighting]'
```

## License

MIT
