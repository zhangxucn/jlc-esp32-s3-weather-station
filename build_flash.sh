#!/bin/zsh
cd "${0:A:h}" || exit 1
source ./export_env.sh || exit 1
set -o pipefail
echo "=== build ==="
idf.py build || { echo "BUILD FAILED - abort"; exit 1; }
echo "=== bin ==="
ls -la build/esp32-weather.bin
echo "=== flash (esptool direct 0x10000) ==="
python -m esptool --chip esp32s3 -p "${ESPPORT:-/dev/ttyUSB0}" -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 8MB --flash-freq 80m 0x10000 build/esp32-weather.bin
echo "=== done ==="
