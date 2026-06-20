#!/usr/bin/env bash

set -euo pipefail

. "$(dirname "$0")/common.sh"

SERVER_LOG="$(make_temp_file)"
TMP_RESPONSE="$(make_temp_file)"

run_tests() {
	local headers body

	headers="$(mktemp)"
	body="$(mktemp)"
	curl -sS --max-time 5 -D "$headers" -o "$body" -X POST http://127.0.0.1:5000/index.html || true
	assert_contains "$headers" "HTTP/1.1 405 Method Not Allowed"
	rm -f "$headers" "$body"

	headers="$(mktemp)"
	body="$(mktemp)"
	curl -sS --path-as-is --max-time 5 -D "$headers" -o "$body" http://127.0.0.1:5000/../etc/passwd || true
	assert_contains "$headers" "HTTP/1.1 400 Bad Request"
	rm -f "$headers" "$body"

	raw_request $'POST /app HTTP/1.1\r\nHost: 127.0.0.1:5000\r\nContent-Length: 3\r\nContent-Length: 4\r\nConnection: close\r\n\r\n' "$TMP_RESPONSE"
	assert_contains "$TMP_RESPONSE" "HTTP/1.1 400 Bad Request"

	raw_request $'POST /app HTTP/1.1\r\nHost: 127.0.0.1:5000\r\nContent-Length: 9000000\r\nConnection: close\r\n\r\n' "$TMP_RESPONSE"
	assert_contains "$TMP_RESPONSE" "HTTP/1.1 413 Request Entity Too Large"
}

trap cleanup_test EXIT

build_server
start_server
run_tests

echo "PASS: error case checks"