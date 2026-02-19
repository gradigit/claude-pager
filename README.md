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
- A GUI editor for Claude Code's Ctrl-G feature (e.g. PromptPad)

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

### 1. Configure the editor shim

Set Claude Code's editor to the included shim script:

```json
{
  "editor": "/path/to/claude-pager/shim/claude-pager-shim.sh"
}
```

The shim launches your GUI editor immediately, then starts the pager in the background.

### 2. Set your GUI editor

The shim needs to know which GUI editor to launch. Set the `TRANSCRIPT_EDITOR` environment variable:

```sh
export TRANSCRIPT_EDITOR=promptpad-editor
```

If `TRANSCRIPT_EDITOR` is not set, the shim looks for `promptpad-editor` on your PATH.

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
| `TRANSCRIPT_EDITOR` | Path to your GUI editor |
| `CLAUDE_PAGER_LOG` | Write debug logs to this file path |

CLI options (when running standalone):

```
claude-pager [transcript.jsonl] [editor_pid] [--ctx-limit TOKENS] [--log-file PATH]
```

## How It Works

1. Claude Code opens the alt screen and spawns the editor shim
2. The shim launches your GUI editor (zero latency)
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
