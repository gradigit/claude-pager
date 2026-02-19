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
- Works with any GUI editor (TurboDraft, VS Code, Sublime, etc.)
- TurboDraft fast path: talks directly to TurboDraft's Unix socket, bypassing all shell overhead

## Requirements

- macOS (arm64 or x86_64)
- A C compiler (Xcode Command Line Tools: `xcode-select --install`)

## Install

### One-liner

```sh
curl -sSL https://raw.githubusercontent.com/gradigit/claude-pager/main/install.sh | bash
```

This clones the repo to `~/.claude-pager`, builds the binary, and creates the editor shim. Follow the printed instructions to set your `VISUAL`/`EDITOR` env vars.

### AI agent install

Paste `https://github.com/gradigit/claude-pager` into Claude Code or any AI coding agent. The [agent instructions](#agent-instructions) below have everything it needs to install and configure claude-pager automatically.

### Manual install

```sh
git clone https://github.com/gradigit/claude-pager.git
cd claude-pager/bin
make
```

This produces `bin/claude-pager-open` (~70KB, zero dependencies).

## Setup

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

### 3. Editor configuration

The binary works with any GUI editor out of the box:

- **TurboDraft**: No extra config needed. The binary detects TurboDraft's Unix socket and talks to it directly — zero shell overhead.
- **Other editors** (VS Code, Sublime, etc.): Set `CLAUDE_PAGER_EDITOR` in your environment or in Claude Code's env settings:

```json
{
  "env": {
    "CLAUDE_PAGER_EDITOR": "code -w"
  }
}
```

If no editor is configured, it falls back to `VISUAL`, then `EDITOR`, then macOS `open -W -t`.

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
3. If TurboDraft is available: connects to its socket and sends `session.open` (~0.02ms)
4. It forks and renders the pager directly in C (~3ms for pre-render, ~5ms for full transcript)
5. Your editor opens the file — the pager is already visible
6. On close: the binary detects the editor exited, kills the pager, and returns control to Claude Code

The pager uses alternate scroll mode (`\033[?1007h`) instead of mouse tracking, so OSC-8 hyperlinks remain Cmd+clickable.

## Architecture

```
claude-pager-open (C binary, ~70KB)
├── Editor detection (CLAUDE_PAGER_EDITOR / VISUAL / EDITOR)
├── TurboDraft socket client (JSON-RPC 2.0 over Unix domain socket)
├── Generic editor path (fork editor + fork pager + waitpid)
├── Transcript parser (minimal JSON scanner, single-pass JSONL)
├── Markdown renderer (ANSI escape codes)
├── Scrollable viewport (raw terminal mode, keyboard/mouse input)
└── Recursion guard (_CLAUDE_PAGER_ACTIVE env var)
```

## Agent Instructions

This section is for AI coding agents (Claude Code, Cursor, Copilot, etc.). Everything here is deterministic — no user input is needed. Detect the user's shell from `$SHELL` and proceed.

### 1. Check prerequisites

```sh
# macOS required; ensure a C compiler is available
xcode-select -p >/dev/null 2>&1 || xcode-select --install
```

### 2. Clone and build

```sh
git clone https://github.com/gradigit/claude-pager.git ~/.claude-pager
make -C ~/.claude-pager/bin
# verify binary exists
test -x ~/.claude-pager/bin/claude-pager-open
```

### 3. Create editor shim

Write `~/.claude/editor-shim.sh`:

```sh
#!/usr/bin/env bash
exec ~/.claude-pager/bin/claude-pager-open "$@"
```

Then `chmod +x ~/.claude/editor-shim.sh`.

### 4. Set VISUAL and EDITOR

Detect the user's shell from `$SHELL` and append to the appropriate config file:

| Shell | Config file | Lines to add |
|---|---|---|
| fish | `~/.config/fish/config.fish` | `set -gx VISUAL ~/.claude/editor-shim.sh` and `set -gx EDITOR ~/.claude/editor-shim.sh` |
| zsh | `~/.zshrc` | `export VISUAL=~/.claude/editor-shim.sh` and `export EDITOR=~/.claude/editor-shim.sh` |
| bash | `~/.bashrc` | `export VISUAL=~/.claude/editor-shim.sh` and `export EDITOR=~/.claude/editor-shim.sh` |

Check if these lines already exist before adding them. If `VISUAL`/`EDITOR` are already set to something else, replace them.

### 5. Add the session hook

Read `~/.claude/settings.json` (create it if missing). Merge a `SessionStart` hook into the `hooks` object — do not overwrite existing hooks or other settings:

```json
{
  "hooks": {
    "SessionStart": [
      {
        "type": "command",
        "command": "~/.claude-pager/shim/save-session-transcript.sh"
      }
    ]
  }
}
```

If a `SessionStart` array already exists, append to it. If the hook command is already present, skip.

### 6. Done

Tell the user to restart their shell (or `source` their config) and press **Ctrl-G** in Claude Code. The pager will render their conversation transcript while the editor is open.

### Agent notes

- The binary auto-detects TurboDraft's Unix socket — no special config needed for TurboDraft users
- If no `CLAUDE_PAGER_EDITOR` is set, the binary uses `VISUAL` → `EDITOR` → macOS `open -W -t`
- The session hook enables multi-session support; without it the pager falls back to the most recent transcript in the project directory
- `_CLAUDE_PAGER_ACTIVE` env var is set internally to prevent recursion when `VISUAL`/`EDITOR` points to the shim — agents do not need to set this

## Development

```sh
git clone https://github.com/gradigit/claude-pager.git
cd claude-pager/bin
make            # builds claude-pager-open
make clean      # removes build artifacts
```

The C source is in `bin/claude-pager-open.c` (editor detection + socket + fork logic) and `bin/pager.c` (pager rendering).

The Python pager (`src/claude_pager/`) and bash shim (`shim/claude-pager-shim.sh`) are legacy fallbacks — the C binary handles everything.

## License

MIT
