#!/usr/bin/env bash

set -euo pipefail

. "$(dirname "$0")/common.sh"

SERVER_LOG="$(make_temp_file)"
HELLO_HEADERS="$(make_temp_file)"
HELLO_BODY="$(make_temp_file)"
APP_GET_HEADERS="$(make_temp_file)"
APP_GET_BODY="$(make_temp_file)"
APP_POST_HEADERS="$(make_temp_file)"
APP_POST_BODY="$(make_temp_file)"

run_tests() {
	curl -fsS --max-time 5 -D "$HELLO_HEADERS" -o "$HELLO_BODY" http://127.0.0.1:5000/hello
	assert_contains "$HELLO_HEADERS" "HTTP/1.1 200 OK"
	assert_contains "$HELLO_HEADERS" "Content-Type: text/plain"
	assert_count "$HELLO_HEADERS" "Connection:" "1"
	assert_contains "$HELLO_BODY" "Hello, world"

	curl -fsS --max-time 5 -D "$APP_GET_HEADERS" -o "$APP_GET_BODY" "http://127.0.0.1:5000/app/foo?x=1"
	assert_contains "$APP_GET_HEADERS" "HTTP/1.1 200 OK"
	assert_contains "$APP_GET_BODY" "REQUEST_METHOD=GET"
	assert_contains "$APP_GET_BODY" "REQUEST_URI=/app/foo?x=1"
	assert_contains "$APP_GET_BODY" "QUERY_STRING=x=1"
	assert_contains "$APP_GET_BODY" "SCRIPT_NAME=/app"
	assert_contains "$APP_GET_BODY" "PATH_INFO=/foo"
	assert_contains "$APP_GET_BODY" "HTTP_HOST=127.0.0.1:5000"
	assert_contains "$APP_GET_BODY" "HTTP_USER_AGENT=curl/"

	curl -fsS --max-time 5 -D "$APP_POST_HEADERS" -o "$APP_POST_BODY" -X POST -H "Content-Type: text/plain" --data "abc123" http://127.0.0.1:5000/app/foo
	assert_contains "$APP_POST_HEADERS" "HTTP/1.1 200 OK"
	assert_contains "$APP_POST_BODY" "REQUEST_METHOD=POST"
	assert_contains "$APP_POST_BODY" "CONTENT_TYPE=text/plain"
	assert_contains "$APP_POST_BODY" "CONTENT_LENGTH=6"
	assert_contains "$APP_POST_BODY" "PATH_INFO=/foo"
	assert_not_contains "$APP_POST_HEADERS" "Connection: close"
}

trap cleanup_test EXIT

build_server
build_fcgiapp
start_server
run_tests

echo "PASS: basic integration checks"