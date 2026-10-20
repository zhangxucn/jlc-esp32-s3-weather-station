#!/bin/zsh
cd "${0:A:h}" || exit 1
source ./export_env.sh || exit 1
set -o pipefail
idf.py build
