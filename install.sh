#!/bin/sh
set -e

REPO="misut/intron"
INSTALL_DIR="$HOME/.intron/bin"

# Platform detection
OS=$(uname -s)
ARCH=$(uname -m)

case "$OS:$ARCH" in
    Darwin:arm64)   PLATFORM="aarch64-apple-darwin" ;;
    Darwin:x86_64)  PLATFORM="x86_64-apple-darwin" ;;
    Linux:aarch64)  PLATFORM="aarch64-linux-gnu" ;;
    Linux:x86_64)   PLATFORM="x86_64-linux-gnu" ;;
    *)
        echo "error: unsupported platform: $OS $ARCH"
        exit 1
        ;;
esac

# Get latest release tag
echo "fetching latest release..."
TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name"' | sed 's/.*"tag_name": "\(.*\)".*/\1/')

if [ -z "$TAG" ]; then
    echo "error: failed to fetch latest release"
    exit 1
fi

echo "installing intron $TAG for $PLATFORM..."

# Download
URL="https://github.com/$REPO/releases/download/$TAG/intron-$TAG-$PLATFORM.tar.gz"
TMPDIR=$(mktemp -d)
curl -fsSL "$URL" -o "$TMPDIR/intron.tar.gz"

# Extract and install
tar -xzf "$TMPDIR/intron.tar.gz" -C "$TMPDIR"
mkdir -p "$INSTALL_DIR"
mv "$TMPDIR/intron" "$INSTALL_DIR/intron"
chmod +x "$INSTALL_DIR/intron"
rm -rf "$TMPDIR"

echo "installed intron $TAG to $INSTALL_DIR/intron"
echo ""

# PATH check
case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *)
        echo "add this to your shell profile:"
        echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
        ;;
esac
