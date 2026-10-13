#!/bin/sh
# Copyright (c) 2026 Nurzaman
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr's checkpatch over every C file in the module, with Zephyr's own
# ignore list (.checkpatch.conf). Prints nothing for clean files and fails
# on any error or warning. Usage: checkpatch.sh <path-to-zephyr>

set -eu
zephyr=${1:?usage: checkpatch.sh <zephyr base>}
cd "$(dirname "$0")/../.."

# shellcheck disable=SC2046
"$zephyr/scripts/checkpatch.pl" --no-tree --quiet --terse -f $(git ls-files '*.c' '*.h')
echo "ok: checkpatch clean"
