# claude-pager

A scrollable terminal pager for Claude Code session transcripts. When you press **Ctrl-G** in Claude Code, this tool renders your conversation history in the terminal while your GUI editor is open.

## Features

- Scrollable viewport with mouse wheel and keyboard navigation
- Markdown rendering with headings, lists, code blocks, bold/italic
- OSC-8 hyperlinks (Cmd+clickable file paths and URLs in supported terminals)
- Diff coloring (+green / -red / @@cyan)
- Syntax highlighting via Pygments (optional)
- Context usage bar showing token consumption
- Live-follow mode: content updates as the transcript grows
- Terminal resize support (SIGWINCH)

## Requirements

- Python 3.9+
- macOS or Linux
- A GUI editor for Claude Code's Ctrl-G feature (e.g. TurboDraft)

## Install

```sh
pip install claude-pager
```

For syntax highlighting in code blocks:

```sh
pip install 'claude-pager[highlighting]'
```

Or install from source:

```sh
git clone https://github.com/gradigit/claude-pager.git
cd claude-pager
pip install -e '.[highlighting]'
```

## Claude Code Setup

### 1. Configure the editor

**TurboDraft users (recommended):** use the compiled C binary for zero-overhead editor launch:

```sh
cd /path/to/claude-pager/bin && make
```

Then set Claude Code's editor to the binary:

```json
{
  "editor": "/path/to/claude-pager/bin/claude-pager-open"
}
```

`claude-pager-open` talks directly to TurboDraft's Unix socket, bypassing the bash shim (~25 ms), `turbodraft-editor`'s osascript (~234 ms), and `turbodraft-open` startup (~5 ms). Total savings: ~264 ms.

**Other editors:** use the bash shim:

```json
{
  "editor": "/path/to/claude-pager/shim/claude-pager-shim.sh"
}
```

Both approaches launch the editor immediately and start the pager in the background.

### 2. Set your GUI editor

Set `CLAUDE_PAGER_EDITOR` in Claude Code's `env` settings:

```json
{
  "env": {
    "CLAUDE_PAGER_EDITOR": "turbodraft-editor"
  }
}
```

The bash shim also falls back to `$VISUAL` / `$EDITOR` if `CLAUDE_PAGER_EDITOR` is unset. `claude-pager-open` uses TurboDraft's socket directly and does not need this variable.

### 3. Install the session hook (recommended)

The included `save-session-transcript.sh` hook ensures the pager always finds the correct transcript, even with multiple Claude sessions:

Add to your Claude Code settings under `hooks.SessionStart`:

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

## Key Bindings

| Key | Action |
|---|---|
| Scroll wheel | Scroll up/down |
| Arrow Up/Down | Scroll one line |
| Page Up/Down | Scroll one page |
| Home | Jump to top |
| End | Jump to bottom |

## Configuration

| Environment Variable | Description |
|---|---|
| `CLAUDE_PAGER_EDITOR` | Path to your GUI editor (used by the bash shim) |
| `CLAUDE_PAGER_LOG` | Write debug logs to this file path |

CLI options (when running standalone):

```
claude-pager [transcript.jsonl] [editor_pid] [--ctx-limit TOKENS] [--log-file PATH]
```

## How It Works

**With `claude-pager-open` (TurboDraft):**

1. Claude Code spawns `claude-pager-open`
2. The binary connects to TurboDraft's Unix socket (~0 ms) and sends a `session.open` request
3. TurboDraft opens the file immediately — no bash, no osascript, no extra process spawning
4. The binary forks `pager-setup.sh` in the background to find the transcript and start the pager
5. The binary blocks on `session.wait` until TurboDraft closes the session
6. On close: the pager is terminated and the binary exits

**With `claude-pager-shim.sh` (other editors):**

1. Claude Code opens the alt screen and spawns the bash shim
2. The shim launches your GUI editor immediately, then starts the pager in the background
3. The shim finds the current session's transcript via:
   - TTY-keyed file written by the SessionStart hook
   - PWD-derived project directory lookup
   - Globally most recent transcript (last resort)
4. The pager renders the transcript in the terminal with scrolling support
5. When you close the editor, the pager exits and Claude Code restores the main screen

The pager uses alternate scroll mode (`\033[?1007h`) instead of mouse tracking, so OSC-8 hyperlinks remain Cmd+clickable.

## Development

```sh
git clone https://github.com/gradigit/claude-pager.git
cd claude-pager
pip install -e '.[highlighting]'
python -m pytest
```

## License

MIT
