#!/bin/bash

#
# A script to build client and update for development use
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CLIENT_DIR="$ROOT_DIR/../neutralino.js"
CLIENT_DIST="$CLIENT_DIR/dist"
CLIENT_DEST="$ROOT_DIR/bin/resources/js"

if [ ! -d "$CLIENT_DIR" ]; then
    echo "ERR: You need to download the client repo next to this runtime repo."
    exit 1
fi

echo "Building client..."
cd "$CLIENT_DIR" || exit 1
npm run build -- --dev || exit 1

echo "Updating client..."
mkdir -p "$CLIENT_DEST"
cp "$CLIENT_DIST/neutralino.js" "$CLIENT_DEST/neutralino.js"
cp "$CLIENT_DIST/neutralino.mjs" "$CLIENT_DEST/neutralino.mjs"
cp "$CLIENT_DIST/neutralino.js.map" "$CLIENT_DEST/neutralino.js.map"

echo "OK: Updated."

