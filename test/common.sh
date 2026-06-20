#!/usr/bin/env bash

ROOT_DIR="/home/soramimi/develop/fcgiexample"
SERVER_BIN="$ROOT_DIR/_bin/fcgiserv"
APP_BIN="$ROOT_DIR/_bin/fcgiapp"
TMP_APP_MAKEFILE="/tmp/fcgiapp.mk"
TEST_TMP_FILES=()
SERVER_PID="${SERVER_PID:-}"
SERVER_LOG="${SERVER_LOG:-}"

if [[ "${DEBUG:-0}" == "1" ]]; then
	set -x
fi

register_tmp() {
	TEST_TMP_FILES+=("$1")
}

make_temp_file() {
	local tmp
	tmp="$(mktemp)"
	register_tmp "$tmp"
	printf '%s\n' "$tmp"
}

cleanup_server() {
	if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
		kill "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
	SERVER_PID=""
}

cleanup_tmp_files() {
	if [[ ${#TEST_TMP_FILES[@]} -gt 0 ]]; then
		rm -f "${TEST_TMP_FILES[@]}"
	fi
	TEST_TMP_FILES=()
}

cleanup_test() {
	cleanup_server
	cleanup_tmp_files
}

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

assert_contains() {
	local file="$1"
	local pattern="$2"
	if ! grep -Fq "$pattern" "$file"; then
		echo "Expected pattern not found: $pattern" >&2
		echo "---- $file ----" >&2
		cat "$file" >&2
		fail "assert_contains"
	fi
}

assert_not_contains() {
	local file="$1"
	local pattern="$2"
	if grep -Fq "$pattern" "$file"; then
		echo "Unexpected pattern found: $pattern" >&2
		echo "---- $file ----" >&2
		cat "$file" >&2
		fail "assert_not_contains"
	fi
}

assert_count() {
	local file="$1"
	local pattern="$2"
	local expected="$3"
	local count
	count="$(grep -Fc "$pattern" "$file" || true)"
	if [[ "$count" != "$expected" ]]; then
		echo "Unexpected count for pattern '$pattern': got $count expected $expected" >&2
		echo "---- $file ----" >&2
		cat "$file" >&2
		fail "assert_count"
	fi
}

build_server() {
	cd "$ROOT_DIR"
	make -j4 _bin/fcgiserv
	[[ -x "$SERVER_BIN" ]] || fail "missing server binary: $SERVER_BIN"
}

build_fcgiapp() {
	cd "$ROOT_DIR"
	if [[ ! -f "$TMP_APP_MAKEFILE" || fcgiapp.pro -nt "$TMP_APP_MAKEFILE" ]]; then
		qmake -o "$TMP_APP_MAKEFILE" fcgiapp.pro
	fi
	make -f "$TMP_APP_MAKEFILE" -j4
	[[ -x "$APP_BIN" ]] || fail "missing app binary: $APP_BIN"
}

start_server() {
	SERVER_LOG="${SERVER_LOG:-$(make_temp_file)}"
	cd "$ROOT_DIR"
	"$SERVER_BIN" >"$SERVER_LOG" 2>&1 &
	SERVER_PID="$!"
	for _ in $(seq 1 50); do
		if curl -fsS --max-time 1 http://127.0.0.1:5000/hello >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.1
	done
	echo "---- server log ----" >&2
	cat "$SERVER_LOG" >&2 || true
	fail "server did not start"
}

raw_request() {
	local payload="$1"
	local response_file="$2"
	: >"$response_file"
	exec 3<>/dev/tcp/127.0.0.1/5000
	printf "%s" "$payload" >&3
	cat <&3 >"$response_file" || true
	exec 3<&-
	exec 3>&-
}