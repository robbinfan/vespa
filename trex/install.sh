#!/bin/bash
#
# trex install script
#
# Usage:
#   curl -sL https://raw.githubusercontent.com/robbinfan/trex/main/install.sh | bash
#   # or
#   ./install.sh [--target REPO_DIR] [--platform claude|codex|cursor|all]
#
set -e

TARGET="${1:-.}"
PLATFORM="${2:-claude}"
TREX_DIR="$TARGET/.claude/tools/trex"

echo "Installing trex to $TREX_DIR ..."

# Create tool directory
mkdir -p "$TREX_DIR"

# Copy source
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cp "$SCRIPT_DIR/main.go" "$TREX_DIR/"
cp "$SCRIPT_DIR/go.mod" "$TREX_DIR/"

# Build
echo "Building trex..."
(cd "$TREX_DIR" && go build -o trex .)

# Build index
echo "Building trigram index..."
"$TREX_DIR/trex" build --dir "$TARGET"

# Install skill definition
case "$PLATFORM" in
  claude|all)
    echo "Installing Claude Code skill..."
    cp "$SCRIPT_DIR/skills/claude/skill.md" "$TREX_DIR/"
    ;;
esac

echo ""
echo "Done! trex installed at $TREX_DIR"
echo ""
echo "Usage:"
echo "  $TREX_DIR/trex search --pattern 'YourPattern' --root $TARGET --files-only"
echo ""
echo "To rebuild index after file changes:"
echo "  $TREX_DIR/trex update --dir $TARGET"
