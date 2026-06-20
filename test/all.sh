#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="/home/soramimi/develop/fcgiexample"

if [[ "${DEBUG:-0}" == "1" ]]; then
	set -x
fi

run_test() {
	local script="$1"
	echo "==> $(basename "$script")"
	"$script"
}

cd "$ROOT_DIR"

run_test "$ROOT_DIR/test/basic.sh"
run_test "$ROOT_DIR/test/error_cases.sh"
run_test "$ROOT_DIR/test/websocket.sh"

echo "PASS: all test scripts"