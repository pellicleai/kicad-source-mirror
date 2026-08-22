#!/bin/bash
set -e

# Links KiCad resources (symbols, footprints, icons, schemas) into the
# built app bundle so the app works correctly after building.
#
# Run this after `ninja` completes:
#   cd kicad-source/build && ninja
#   cd ../../scripts && ./link_resources.sh

echo "Linking resources into KiCad app bundle..."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(dirname "$SCRIPT_DIR")"

# Detect OS
OS="unknown"
case "$(uname -s)" in
    Darwin*) OS="macos" ;;
    Linux*)  OS="linux" ;;
esac

# Find the sibling repos (symbols, footprints)
SYMS_DIR="$SRC_DIR/../kicad-symbols"
FOOT_DIR="$SRC_DIR/../kicad-footprints"

if [ ! -d "$SYMS_DIR" ]; then
    echo "ERROR: kicad-symbols not found at $SYMS_DIR"
    echo "Clone it: git clone https://github.com/pellicleai/kicad-symbols.git"
    exit 1
fi

if [ ! -d "$FOOT_DIR" ]; then
    echo "ERROR: kicad-footprints not found at $FOOT_DIR"
    echo "Clone it: git clone https://github.com/pellicleai/kicad-footprints.git"
    exit 1
fi

if [ "$OS" = "macos" ]; then
    APP_BUNDLE="$SRC_DIR/build/kicad/KiCad.app"
    SS="$APP_BUNDLE/Contents/SharedSupport"
else
    # Linux: resources go next to the binary
    APP_BUNDLE="$SRC_DIR/build/kicad"
    SS="$APP_BUNDLE/share/kicad"
fi

mkdir -p "$SS"

# Link symbols directory
if [ ! -e "$SS/symbols" ]; then
    ln -sf "$SYMS_DIR" "$SS/symbols"
    echo "  Linked symbols"
else
    echo "  symbols already linked"
fi

# Link footprints directory
if [ ! -e "$SS/footprints" ]; then
    ln -sf "$FOOT_DIR" "$SS/footprints"
    echo "  Linked footprints"
else
    echo "  footprints already linked"
fi

# Copy resources (icons, images)
mkdir -p "$SS/resources"
if [ -f "$SRC_DIR/build/resources/images.tar.gz" ] && [ ! -f "$SS/resources/images.tar.gz" ]; then
    cp "$SRC_DIR/build/resources/images.tar.gz" "$SS/resources/"
    echo "  Copied images.tar.gz"
fi

# Copy schemas
if [ ! -d "$SS/schemas" ]; then
    cp -r "$SRC_DIR/resources/schemas" "$SS/schemas" 2>/dev/null || true
    cp "$SRC_DIR/build/schemas/pcm.v1.schema.json" "$SS/schemas/" 2>/dev/null || true
    cp "$SRC_DIR/build/schemas/pcm.v2.schema.json" "$SS/schemas/" 2>/dev/null || true
    echo "  Copied schemas"
fi

# Copy templates
if [ ! -d "$SS/template" ]; then
    cp -r "$SRC_DIR/resources/project_template" "$SS/template" 2>/dev/null || true
    echo "  Copied templates"
fi

# Copy library tables to KiCad config
if [ "$OS" = "macos" ]; then
    KICAD_CONFIG="$HOME/Library/Preferences/kicad/10.99"
else
    KICAD_CONFIG="$HOME/.config/kicad/10.99"
fi

mkdir -p "$KICAD_CONFIG"
cp "$SYMS_DIR/sym-lib-table" "$KICAD_CONFIG/sym-lib-table" 2>/dev/null || true
cp "$FOOT_DIR/fp-lib-table" "$KICAD_CONFIG/fp-lib-table" 2>/dev/null || true
echo "  Copied library tables to $KICAD_CONFIG"

echo ""
echo "Done! Resources linked."
echo ""
echo "To run KiCad:"
if [ "$OS" = "macos" ]; then
    echo "  open $APP_BUNDLE"
else
    echo "  $APP_BUNDLE/kicad"
fi
