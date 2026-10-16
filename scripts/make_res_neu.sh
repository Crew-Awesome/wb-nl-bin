#!/bin/bash

#
# A script to build res.neu for the test app
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Updating client..."
"$SCRIPT_DIR/update_client.sh"

echo "Packing resources to resources.neu..."
cd "$ROOT_DIR/bin" || exit 1
mkdir -p ./.res
cp -r ./resources ./.res/
cp ./neutralino.config.json ./.res/neutralino.config.json
asar pack ./.res resources.neu
rm -r ./.res

echo "Check resources.neu structure"
asar list resources.neu

cp resources.neu ./resources/updater_test/update_res.neu

echo "OK: Packed."
