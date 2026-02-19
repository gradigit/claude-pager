#!/usr/bin/env bash
# claude-pager installer — run with:
#   curl -sSL https://raw.githubusercontent.com/gradigit/claude-pager/main/install.sh | bash
set -euo pipefail

REPO="https://github.com/gradigit/claude-pager.git"
INSTALL_DIR="${HOME}/.claude-pager"
BINARY="${INSTALL_DIR}/bin/claude-pager-open"
SHIM="${HOME}/.claude/editor-shim.sh"
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

# ── Editor shim ──────────────────────────────────────────────────────────────
mkdir -p "$(dirname "$SHIM")"
cat > "$SHIM" <<EOF
#!/usr/bin/env bash
exec "$BINARY" "\$@"
EOF
chmod +x "$SHIM"
echo "Editor shim: $SHIM"

# ── Session hook ─────────────────────────────────────────────────────────────
if [[ -f "$SETTINGS" ]]; then
    # Check if hook already exists
    if grep -q "save-session-transcript" "$SETTINGS" 2>/dev/null; then
        echo "Session hook already configured in $SETTINGS"
    else
        echo ""
        echo "Add this hook to $SETTINGS under \"hooks\":"
        echo ""
        echo '  "SessionStart": [{'
        echo '    "type": "command",'
        echo "    \"command\": \"$HOOK\""
        echo '  }]'
        echo ""
    fi
else
    mkdir -p "$(dirname "$SETTINGS")"
    cat > "$SETTINGS" <<EOF
{
  "hooks": {
    "SessionStart": [
      {
        "type": "command",
        "command": "$HOOK"
      }
    ]
  }
}
EOF
    echo "Created $SETTINGS with session hook"
fi

# ── Shell config ─────────────────────────────────────────────────────────────
echo ""
echo "Done! Add to your shell config:"
echo ""

# Detect shell
case "${SHELL:-}" in
    */fish)
        echo "  # fish — add to ~/.config/fish/config.fish"
        echo "  set -gx VISUAL $SHIM"
        echo "  set -gx EDITOR $SHIM"
        ;;
    */zsh)
        echo "  # zsh — add to ~/.zshrc"
        echo "  export VISUAL=\"$SHIM\""
        echo "  export EDITOR=\"$SHIM\""
        ;;
    *)
        echo "  # bash — add to ~/.bashrc"
        echo "  export VISUAL=\"$SHIM\""
        echo "  export EDITOR=\"$SHIM\""
        ;;
esac

echo ""
echo "Then restart your shell and press Ctrl-G in Claude Code."
