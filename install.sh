#!/usr/bin/env bash
# claude-pager installer — run with:
#   curl -sSL https://raw.githubusercontent.com/gradigit/claude-pager/main/install.sh | bash
set -euo pipefail

REPO="https://github.com/gradigit/claude-pager.git"
INSTALL_DIR="${HOME}/.claude-pager"
BINARY="${INSTALL_DIR}/bin/claude-pager-open"
SETTINGS="${HOME}/.claude/settings.json"
HOOK="${INSTALL_DIR}/shim/save-session-transcript.sh"

echo "Installing claude-pager..."

# ── Clone or update ──────────────────────────────────────────────────────────
if [[ -d "$INSTALL_DIR" ]]; then
    echo "Updating existing install..."
    git -C "$INSTALL_DIR" pull --ff-only
else
    git clone "$REPO" "$INSTALL_DIR"
fi

# ── Build ────────────────────────────────────────────────────────────────────
echo "Building..."
make -C "${INSTALL_DIR}/bin" clean
make -C "${INSTALL_DIR}/bin"

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: build failed — $BINARY not found" >&2
    exit 1
fi
echo "Built: $BINARY"

# ── Ensure jq is available ───────────────────────────────────────────────────
if ! command -v jq &>/dev/null; then
    echo "jq not found — installing via Homebrew..."
    if command -v brew &>/dev/null; then
        brew install jq
    else
        echo "ERROR: jq is required but not installed, and Homebrew is not available." >&2
        echo "  Install jq manually: https://jqlang.github.io/jq/download/" >&2
        exit 1
    fi
fi

# ── Configure settings.json ─────────────────────────────────────────────────
mkdir -p "$(dirname "$SETTINGS")"

if [[ ! -f "$SETTINGS" ]]; then
    echo "{}" > "$SETTINGS"
fi

# Read current editor value (if any)
OLD_EDITOR=$(jq -r '.editor // empty' "$SETTINGS")

# Save old editor as env.CLAUDE_PAGER_EDITOR (if it's not already our binary)
if [[ -n "$OLD_EDITOR" && "$OLD_EDITOR" != "$BINARY" && "$OLD_EDITOR" != *"claude-pager"* ]]; then
    echo "Preserving previous editor: $OLD_EDITOR"
    SETTINGS_TMP=$(mktemp)
    jq --arg ed "$OLD_EDITOR" '.env.CLAUDE_PAGER_EDITOR = $ed' "$SETTINGS" > "$SETTINGS_TMP"
    mv "$SETTINGS_TMP" "$SETTINGS"
else
    # Check if CLAUDE_PAGER_EDITOR is already set
    EXISTING_CPE=$(jq -r '.env.CLAUDE_PAGER_EDITOR // empty' "$SETTINGS")
    if [[ -z "$EXISTING_CPE" ]]; then
        # No old editor and no CLAUDE_PAGER_EDITOR — try to detect an IDE
        DETECTED=""
        for candidate in cursor code zed subl; do
            if command -v "$candidate" &>/dev/null; then
                case "$candidate" in
                    cursor) DETECTED="cursor --wait" ;;
                    code)   DETECTED="code --wait" ;;
                    zed)    DETECTED="zed --wait" ;;
                    subl)   DETECTED="subl --wait" ;;
                esac
                break
            fi
        done

        if [[ -n "$DETECTED" ]]; then
            echo "Detected editor: $DETECTED"
            SETTINGS_TMP=$(mktemp)
            jq --arg ed "$DETECTED" '.env.CLAUDE_PAGER_EDITOR = $ed' "$SETTINGS" > "$SETTINGS_TMP"
            mv "$SETTINGS_TMP" "$SETTINGS"
        else
            # Nothing detected — prompt the user
            echo ""
            echo "No GUI editor detected. Which editor should claude-pager open files in?"
            echo ""
            echo "  1) code --wait      (VS Code)"
            echo "  2) cursor --wait    (Cursor)"
            echo "  3) zed --wait       (Zed)"
            echo "  4) subl --wait      (Sublime Text)"
            echo "  5) vim              (terminal)"
            echo "  6) nvim             (terminal)"
            echo "  7) other"
            echo ""
            read -rp "Choice [1-7]: " choice
            case "$choice" in
                1) DETECTED="code --wait" ;;
                2) DETECTED="cursor --wait" ;;
                3) DETECTED="zed --wait" ;;
                4) DETECTED="subl --wait" ;;
                5) DETECTED="vim" ;;
                6) DETECTED="nvim" ;;
                7)
                    read -rp "Enter editor command: " DETECTED
                    ;;
                *)
                    echo "No editor selected — you can set it later in ~/.claude/settings.json"
                    DETECTED=""
                    ;;
            esac

            if [[ -n "$DETECTED" ]]; then
                SETTINGS_TMP=$(mktemp)
                jq --arg ed "$DETECTED" '.env.CLAUDE_PAGER_EDITOR = $ed' "$SETTINGS" > "$SETTINGS_TMP"
                mv "$SETTINGS_TMP" "$SETTINGS"
                echo "Set editor: $DETECTED"
            fi
        fi
    else
        echo "Editor already configured: $EXISTING_CPE"
    fi
fi

# Set editor to claude-pager-open binary
SETTINGS_TMP=$(mktemp)
jq --arg bin "$BINARY" '.editor = $bin' "$SETTINGS" > "$SETTINGS_TMP"
mv "$SETTINGS_TMP" "$SETTINGS"
echo "Set editor in settings.json: $BINARY"

# ── Session hook ─────────────────────────────────────────────────────────────
if jq -e '.hooks.SessionStart' "$SETTINGS" &>/dev/null; then
    if jq -e '.hooks.SessionStart[] | select(.command | contains("save-session-transcript"))' "$SETTINGS" &>/dev/null; then
        echo "Session hook already configured"
    else
        SETTINGS_TMP=$(mktemp)
        jq --arg cmd "$HOOK" '.hooks.SessionStart += [{"type": "command", "command": $cmd}]' "$SETTINGS" > "$SETTINGS_TMP"
        mv "$SETTINGS_TMP" "$SETTINGS"
        echo "Added session hook"
    fi
else
    SETTINGS_TMP=$(mktemp)
    jq --arg cmd "$HOOK" '.hooks.SessionStart = [{"type": "command", "command": $cmd}]' "$SETTINGS" > "$SETTINGS_TMP"
    mv "$SETTINGS_TMP" "$SETTINGS"
    echo "Added session hook"
fi

echo ""
echo "Done! Restart Claude Code and press Ctrl-G to use the pager."
echo ""
echo "Your editor is configured in ~/.claude/settings.json:"
echo "  editor: $BINARY"
echo "  env.CLAUDE_PAGER_EDITOR: $(jq -r '.env.CLAUDE_PAGER_EDITOR // "not set"' "$SETTINGS")"
