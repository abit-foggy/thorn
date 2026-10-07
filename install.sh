#!/bin/sh
# install.sh - Installer for thorn
#
# Usage:
#   # Change TAG=stable to TAG=nightly for the nightly build
#   curl -fsSL https://raw.githubusercontent.com/abit-foggy/thorn/main/install.sh | TAG=stable bash
#
# Overrides:
#   TAG=nightly          Install nightly build instead of stable (default: stable)
#   PREFIX=/usr/local    Custom install prefix (binary installed to $PREFIX/bin/thorn)
#   REPO=owner/thorn     Custom GitHub repository (default: abit-foggy/thorn)
#
set -eu

REPO="${REPO:-abit-foggy/thorn}"
TAG="${TAG:-stable}"
PREFIX="${PREFIX:-}"

say() { printf 'install.sh: %s\n' "$*"; }
die() { printf 'install.sh: error: %s\n' "$*" >&2; exit 1; }

calc_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo ""
    fi
}

command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || \
    die "neither curl nor wget is installed"

fetch() {
    _token="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
    if command -v curl >/dev/null 2>&1; then
        if [ -n "$_token" ]; then
            curl -fsSL -H "Authorization: Bearer $_token" "$1" -o "$2"
        else
            curl -fsSL "$1" -o "$2"
        fi
    else
        wget -qO "$2" "$1"
    fi
}

# Determine destination directory
if [ -n "$PREFIX" ]; then
    TARGET_DIR="$PREFIX/bin"
elif [ -w "/usr/local/bin" ]; then
    TARGET_DIR="/usr/local/bin"
else
    TARGET_DIR="${HOME}/.local/bin"
fi

# Locate existing installed binary if any
INSTALLED_BIN=""
if [ -n "$PREFIX" ]; then
    if [ -f "$TARGET_DIR/thorn" ]; then
        INSTALLED_BIN="$TARGET_DIR/thorn"
    fi
elif command -v thorn >/dev/null 2>&1; then
    INSTALLED_BIN="$(command -v thorn)"
elif [ -f "$TARGET_DIR/thorn" ]; then
    INSTALLED_BIN="$TARGET_DIR/thorn"
fi

TMP_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t thorn-install.XXXXXX)"
TMP_BIN="$TMP_DIR/thorn"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

say "downloading thorn ($TAG) from https://github.com/$REPO"
BIN_URL="https://github.com/$REPO/releases/download/$TAG/thorn"
TAR_URL="https://github.com/$REPO/releases/download/$TAG/thorn-x86_64-linux.tar.gz"
TAR_NIGHTLY_URL="https://github.com/$REPO/releases/download/$TAG/thorn-x86_64-linux-nightly.tar.gz"

downloaded=0
if fetch "$BIN_URL" "$TMP_BIN" 2>/dev/null; then
    downloaded=1
elif fetch "$TAR_URL" "$TMP_DIR/thorn.tar.gz" 2>/dev/null; then
    tar -xzf "$TMP_DIR/thorn.tar.gz" -C "$TMP_DIR"
    [ -f "$TMP_BIN" ] && downloaded=1
elif fetch "$TAR_NIGHTLY_URL" "$TMP_DIR/thorn.tar.gz" 2>/dev/null; then
    tar -xzf "$TMP_DIR/thorn.tar.gz" -C "$TMP_DIR"
    [ -f "$TMP_BIN" ] && downloaded=1
elif command -v gh >/dev/null 2>&1; then
    if gh release download "$TAG" -R "$REPO" -p "thorn" -O "$TMP_BIN" 2>/dev/null; then
        downloaded=1
    elif gh release download "$TAG" -R "$REPO" -p "thorn-*.tar.gz" -O "$TMP_DIR/thorn.tar.gz" 2>/dev/null; then
        tar -xzf "$TMP_DIR/thorn.tar.gz" -C "$TMP_DIR"
        [ -f "$TMP_BIN" ] && downloaded=1
    fi
fi

[ "$downloaded" -eq 1 ] || die "failed to download thorn ($TAG) from https://github.com/$REPO"
chmod +x "$TMP_BIN"

NEW_SHA="$(calc_sha256 "$TMP_BIN")"

if [ -n "$INSTALLED_BIN" ] && [ -f "$INSTALLED_BIN" ]; then
    OLD_SHA="$(calc_sha256 "$INSTALLED_BIN")"
    if [ -n "$NEW_SHA" ] && [ "$NEW_SHA" = "$OLD_SHA" ]; then
        say "thorn is already up to date ($TAG sha256: $NEW_SHA)"
        say "removing downloaded binary"
        rm -f "$TMP_BIN"
        exit 0
    fi
    say "sha differs (installed: ${OLD_SHA:-unknown}, downloaded: $NEW_SHA) - updating"
    DEST="$INSTALLED_BIN"
else
    say "installing thorn ($TAG sha256: $NEW_SHA)"
    DEST="$TARGET_DIR/thorn"
fi

mkdir -p "$(dirname "$DEST")"
mv -f "$TMP_BIN" "$DEST"
chmod +x "$DEST"

case ":$PATH:" in
    *":$(dirname "$DEST"):*") ;;
    *)
        say "NOTE: $(dirname "$DEST") is not in your PATH. Consider adding it:"
        say "  export PATH=\"$(dirname "$DEST"):\$PATH\""
        ;;
esac

say "installed thorn to $DEST"
