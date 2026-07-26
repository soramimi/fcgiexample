#ifdef _WIN32
#define NOMINMAX
#endif

#include "socket.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#define O_BINARY 0
#define strnicmp(A, B, C) strncasecmp(A, B, C)
#endif

#include "debug.h"
#include "httpserver.h"
#include "joinpath.h"
#include "misc.h"
#include "strformat.h"
#include "thread.h"
#include <algorithm>
#include <limits>
#include <map>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
constexpr size_t MAX_CONTENT_LENGTH = 8 * 1024 * 1024; // 8MiB
constexpr int RECV_TIMEOUT_SEC = 30; // per-recv timeout (Slowloris mitigation)
constexpr int SEND_TIMEOUT_SEC = 30;

#ifndef _WIN32
volatile sig_atomic_t g_shutdown_requested = 0;
volatile sig_atomic_t g_listening_socket_for_signal = -1;

extern "C" void handle_termination_signal(int)
{
	g_shutdown_requested = 1;
	if (g_listening_socket_for_signal >= 0) {
		close(static_cast<int>(g_listening_socket_for_signal));
		g_listening_socket_for_signal = -1;
	}
}

void install_termination_handlers()
{
	static bool installed = false;
	if (installed) {
		return;
	}
	struct sigaction sa = { };
	sa.sa_handler = handle_termination_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	installed = true;
}
#endif

size_t count_headers(std::vector<std::string> const &header, std::string_view name)
{
	size_t count = 0;
	for (std::string const &line : header) {
		if (line.size() > name.size() && strnicmp(line.c_str(), std::string(name).c_str(), name.size()) == 0 && line[name.size()] == ':') {
			count++;
		}
	}
	return count;
}

bool is_sensitive_request_header(std::string_view name)
{
	return misc::iequals_ascii(name, "Authorization") || misc::iequals_ascii(name, "Proxy-Authorization") || misc::iequals_ascii(name, "Cookie") || misc::iequals_ascii(name, "X-Api-Key");
}

std::string mask_header_for_log(std::string const &line)
{
	size_t pos = line.find(':');
	if (pos == std::string::npos) {
		return line;
	}
	std::string_view name(line.data(), pos);
	if (!is_sensitive_request_header(misc::trim_ascii(name))) {
		return line;
	}
	std::string masked(line.data(), line.data() + pos + 1);
	masked += " [redacted]";
	return masked;
}

}

std::string SocketBuffer::readline()
{
	if (buffer.empty()) {
		buffer.resize(1024);
	}
	size_t line_start = offset;
	while (1) {
		if (offset < length) {
			unsigned char *left = &buffer[line_start];
			unsigned char *right = (unsigned char *)memchr(&buffer[offset], '\n', length - offset);
			if (right) {
				offset = right + 1 - &buffer[0];
				while (left < right && isspace(right[-1])) {
					right--;
				}
				return std::string(left, right);
			}
			if (static_cast<size_t>(length - line_start) >= max_line_length) {
				connected = false;
				return std::string();
			}
			int n = length - offset;
			memmove(&buffer[0], &buffer[offset], n);
			length = n;
			offset = 0;
			line_start = 0;
		} else {
			length = 0;
			offset = 0;
			line_start = 0;
		}
		if (static_cast<size_t>(length) >= buffer.size()) {
			size_t new_size = buffer.size() * 2;
			if (new_size > max_line_length) {
				new_size = max_line_length;
			}
			if (new_size <= buffer.size()) {
				connected = false;
				return std::string();
			}
			buffer.resize(new_size);
		}
		int l = ::recv(sock, (char *)&buffer[0] + length, buffer.size() - length, 0);
		if (l < 1) {
			connected = false;
			return std::string();
		}
		length += l;
	}
}

void SocketBuffer::read(std::vector<char> *out, size_t maxlen)
{
	out->clear();
	if (buffer.empty()) {
		buffer.resize(4096);
	}
	while (1) {
		if (offset < length) {
			bool end = false;
			size_t n = length - offset;
			if (maxlen > 0) {
				if (n >= maxlen) {
					n = maxlen;
					end = true;
				}
				maxlen -= n;
			} else {
				// maxlen == 0: nothing more to read
				break;
			}
			unsigned char *left = &buffer[offset];
			unsigned char *right = left + n;
			out->insert(out->end(), left, right);
			offset += n;
			n = length - offset;
			if (n > 0) memmove(&buffer[0], right, n);
			length = n;
			offset = 0;
			if (end) {
				break;
			}
		} else {
			length = 0;
			offset = 0;
		}
		int l = ::recv(sock, (char *)&buffer[0] + length, buffer.size() - length, 0);
		if (l < 1) {
			connected = false;
			return;
		}
		length += l;
	}
}

//

static char const *parse_header(char const *begin, char const *end, std::vector<std::string> *header)
{
	header->clear();
	char const *ptr = begin;
	while (ptr < end) {
		char const *left = ptr;
		while (ptr < end) {
			if (*ptr == '\r') {
				ptr++;
				if (ptr < end && *ptr == '\n') {
					ptr++;
				}
				break;
			}
			if (*ptr == '\n') {
				ptr++;
				break;
			}
			ptr++;
		}
		char const *right = ptr;
		while (left < right && (right[-1] == '\r' || right[-1] == '\n')) {
			right--;
		}
		if (left < right) {
			header->push_back(std::string(left, right));
		} else {
			break;
		}
	}
	return ptr;
}

// HTTP_Server

static bool send_all(socket_t sock, char const *ptr, int len)
{
	char const *end = ptr + len;
	while (ptr < end) {
		int n = end - ptr;
		n = std::min(n, 65536);
		n = ::send(sock, ptr, n, 0);
		if (n < 1) return false;
		ptr += n;
	}
	return ptr == end;
}

//

class RequestHandlerMap {
private:
	std::map<std::string, RequestHandler *> map;

public:
	void add(std::string const &suffix, RequestHandler *handler)
	{
		map[suffix] = handler;
	}
	RequestHandler *find(std::string const &suffix) const
	{
		std::map<std::string, RequestHandler *>::const_iterator it = map.find(suffix);
		if (it == map.end()) {
			return 0;
		}
		return it->second;
	}
};

namespace {

constexpr size_t MAX_WEBSOCKET_MESSAGE_SIZE = 1024 * 1024; // 1MiB

class WebSocket {
public:
	struct Message {
		int opcode;
		std::vector<char> payload;
	};

private:
	std::vector<unsigned char> buffer;
	std::vector<char> current_message;
	int message_opcode = 0;
	bool closing = false;

	bool unmask_and_append(unsigned char const *payload, uint64_t payload_length, unsigned char const *mask_key)
	{
		if (payload_length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
			return false;
		}
		size_t len = static_cast<size_t>(payload_length);
		if (current_message.size() + len > MAX_WEBSOCKET_MESSAGE_SIZE) {
			return false;
		}
		size_t base = current_message.size();
		current_message.resize(base + len);
		for (size_t j = 0; j < len; j++) {
			current_message[base + j] = payload[j] ^ mask_key[j & 3];
		}
		return true;
	}

public:
	bool feed(char const *data, int len, std::vector<Message> *out)
	{
		if (closing) return false;
		buffer.insert(buffer.end(), reinterpret_cast<unsigned char const *>(data), reinterpret_cast<unsigned char const *>(data) + len);
		while (true) {
			if (buffer.size() < 2) return true;
			unsigned char const *p = buffer.data();
			bool fin = (p[0] & 0x80) != 0;
			int rsv = (p[0] >> 4) & 0x07;
			int opcode = p[0] & 0x0f;
			bool masked = (p[1] & 0x80) != 0;
			uint64_t payload_length = p[1] & 0x7f;
			size_t i = 2;
			if (rsv != 0) return false;
			if (!masked) return false;
			if (payload_length == 126) {
				if (buffer.size() < i + 2) return true;
				payload_length = (static_cast<uint64_t>(p[i]) << 8) | p[i + 1];
				i += 2;
				if (payload_length > MAX_WEBSOCKET_MESSAGE_SIZE) return false;
			} else if (payload_length == 127) {
				if (buffer.size() < i + 8) return true;
				payload_length = 0;
				for (int j = 0; j < 8; j++) {
					payload_length = (payload_length << 8) | p[i + j];
				}
				i += 8;
				if (payload_length > MAX_WEBSOCKET_MESSAGE_SIZE) return false;
			}
			if (buffer.size() < i + 4) return true;
			unsigned char mask_key[4];
			memcpy(mask_key, p + i, 4);
			i += 4;
			if (buffer.size() < i + payload_length) return true;
			unsigned char const *payload = p + i;
			if (opcode == 0x8) { // close
				closing = true;
				return false;
			} else if (opcode == 0x9) { // ping
				out->push_back(Message());
				out->back().opcode = 0xa; // pong
				out->back().payload.assign(payload, payload + payload_length);
			} else if (opcode == 0xa) { // pong
				// ignore
			} else if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {
				if (opcode != 0x0) {
					if (!current_message.empty()) return false;
					message_opcode = opcode;
				}
				if (!unmask_and_append(payload, payload_length, mask_key)) {
					return false;
				}
				if (fin) {
					out->push_back(Message());
					out->back().opcode = message_opcode;
					out->back().payload.swap(current_message);
					message_opcode = 0;
				}
			} else {
				return false;
			}
			buffer.erase(buffer.begin(), buffer.begin() + i + static_cast<size_t>(payload_length));
		}
	}

	static std::vector<unsigned char> make_frame(int opcode, char const *data, size_t len)
	{
		std::vector<unsigned char> frame;
		frame.push_back(static_cast<unsigned char>(0x80 | (opcode & 0x0f)));
		if (len < 126) {
			frame.push_back(static_cast<unsigned char>(len));
		} else if (len <= 0xffff) {
			frame.push_back(126);
			frame.push_back(static_cast<unsigned char>(len >> 8));
			frame.push_back(static_cast<unsigned char>(len));
		} else {
			frame.push_back(127);
			for (int i = 7; i >= 0; i--) {
				frame.push_back(static_cast<unsigned char>((len >> (i * 8)) & 0xff));
			}
		}
		frame.insert(frame.end(), data, data + len);
		return frame;
	}
};

} // namespace

struct HTTP_Server::Private {
	int tcp_port;
	std::string bind_addr;
	socket_t listening_socket;

	HTTP_Handler *http_handler;

	RequestHandlerMap request_handler_map;
};

class WebSocketThread : public Thread {
public:
	int sock = -1;
	virtual void run()
	{
		while (true) {
			char buf[65536];
			int n = read(sock, buf, sizeof(buf));
			if (n < 1) break;
			printlog(strformat("ws thread read %u bytes").u(n).str());
		}
	}
};

class HTTP_Thread : public Thread {
public:
	HTTP_Server *server;
	SocketBuffer sockbuff;
	void setup(HTTP_Server *s)
	{
		server = s;
	}
	virtual void run()
	{
		try {
			while (1) {
				struct sockaddr_in peer_sin;

				socklen_t len = sizeof(peer_sin);

				socket_t connected_socket = -1;
				// Retry accept() on EINTR (signal interruption)
				while (true) {
					connected_socket = accept(server->m->listening_socket, (struct sockaddr *)&peer_sin, &len);
					if (connected_socket != -1) break;
					if (errno == EINTR) continue;
					if (
#ifndef _WIN32
						g_shutdown_requested ||
#endif
						server->m->listening_socket == -1) {
						return;
					}
					throw "accept failed";
				}

				// Apply send/recv timeouts to mitigate Slowloris and stuck clients
				set_socket_timeout(connected_socket, RECV_TIMEOUT_SEC, SEND_TIMEOUT_SEC);
				// Disable Nagle to ensure small interactive frames (101 headers, WS control frames) are sent immediately.
				set_tcp_nodelay(connected_socket);

				sockbuff.clear();
				sockbuff.sock = connected_socket;
				sockbuff.connected = true;

				bool web_socket_upgraded = false;
				while (sockbuff.connected) {
					http_request_t request;
					request.sockbuff = &sockbuff;
					http_status_t const *status = nullptr;
					size_t total_header_size = 0;

					while (sockbuff.connected) {
						std::string line = sockbuff.readline();
						if (line.empty()) {
							if (!sockbuff.connected) {
								status = http400_bad_request;
							}
							break;
						}
						total_header_size += line.size() + 2; // include \r\n
						if (total_header_size > sockbuff.max_header_total_length) {
							status = http400_bad_request;
							sockbuff.connected = false;
							break;
						}
						request.header.push_back(line);
					}
					if ((request.header.size() > 0 || status) && sockbuff.connected) {
						http_response_t response;
						if (!request.header.empty()) {
							std::vector<std::string> first;
							misc::split_words_by_space(request.header.front(), &first);
							if (first.size() < 3) {
								status = http400_bad_request;
							} else {
								if (first[0] == "GET") {
									request.method = RequestMethod::GET;
								} else if (first[0] == "POST") {
									request.method = RequestMethod::POST;
								}
								request.uri = first[1];
								std::vector<std::string> prot;
								misc::split_words(first[2], '/', &prot);
								if (prot.size() == 2) {
									request.protocol = prot[0];
									if (request.protocol == "HTTP") {
										unsigned int maj, min;
										if (sscanf(prot[1].c_str(), "%u.%u", &maj, &min) == 2) {
											request.protocol_version.maj = maj;
											request.protocol_version.min = min;
											if (maj == 1 && min >= 1) {
												response.keepalive = ConnectionType::KeepAlive;
											}
										}
										request.scheme = "http";
									}
								}
								request.header.erase(request.header.begin());
								{
									std::string s = request.header_value("Connection");
									if (misc::header_has_token(s, "close")) {
										response.keepalive = ConnectionType::Close;
									} else if (misc::header_has_token(s, "keep-alive")) {
										response.keepalive = ConnectionType::KeepAlive;
									}
								}
								{
									size_t host_count = count_headers(request.header, "Host");
									if (host_count > 1) {
										status = http400_bad_request;
									} else if (request.protocol == "HTTP" && request.protocol_version.maj == 1 && request.protocol_version.min >= 1 && host_count != 1) {
										status = http400_bad_request;
									}
								}
								if (!status) {
									std::string transfer_encoding = request.header_value("Transfer-Encoding");
									if (!transfer_encoding.empty()) {
										status = http501_not_implemented;
									}
								}
								if (!status) {
									std::string expect = request.header_value("Expect");
									if (!expect.empty()) {
										status = http417_expectation_failed;
									}
								}
								if (!status) {
									// Content-Length: reject duplicates (HTTP smuggling mitigation)
									std::string s = request.header_value("Content-Length");
									if (!s.empty()) {
										size_t cl_count = count_headers(request.header, "Content-Length");
										char *endptr = nullptr;
										long cl = strtol(s.c_str(), &endptr, 10);
										if (cl_count > 1 || endptr == s.c_str() || *endptr != '\0' || cl < 0 || static_cast<unsigned long>(cl) > MAX_CONTENT_LENGTH) {
											status = (cl_count > 1) ? http400_bad_request : http413_request_entity_too_large;
										} else {
											request.content_length = static_cast<size_t>(cl);
										}
									}
								}
							}
						}
						if (!status) {
							HTTPIO *io = &response;
							status = server->http_process_request(this, &request, &response, io);
							if (!status) {
								status = http500_internal_server_error;
							}
						}
						if (status->code / 100 >= 4) {
							if (response.content.empty()) {
								response.write("Content-Type: text/plain\r\n\r\n");
								char tmp[10];
								sprintf(tmp, "%u ", status->code);
								response.write(tmp);
								response.write(status->text);
							}
						}
						if (response.content.empty()) {
							if (status->code / 100 < 4) {
								status = http204_no_content;
							}
						} else {
							std::vector<std::string> header;
							char const *begin = &response.content[0];
							char const *end = begin + response.content.size();
							char const *ptr = parse_header(begin, end, &header);
							int len = end - ptr;
							if (status != http101_switching_protocols) {
								char tmp[100];
								sprintf(tmp, "Content-Length: %u", len);
								header.push_back(tmp);
							}
							if (response.keepalive == ConnectionType::KeepAlive) {
								header.push_back("Connection: keep-alive");
							}
							if (!server->http_send_response_header(sockbuff.sock, status, header)) {
								sockbuff.connected = false;
							} else if (status != http101_switching_protocols && !send_all(sockbuff.sock, ptr, len)) {
								sockbuff.connected = false;
							}
						}

						// websocket
						if (status == http101_switching_protocols && response.keepalive == ConnectionType::UpgradeWebSocket) {
							web_socket_upgraded = true;
							WebSocket ws;
							bool ws_ok = true;
							while (sockbuff.connected && ws_ok) {
								char buf[65536];
								int n = read(connected_socket, buf, sizeof(buf));
								if (n < 1) break;
								std::vector<WebSocket::Message> messages;
								if (!ws.feed(buf, n, &messages)) {
									ws_ok = false;
									break;
								}
								for (WebSocket::Message const &msg : messages) {
									if (msg.opcode == 0x9) { // ping
										auto frame = WebSocket::make_frame(0xa, msg.payload.data(), msg.payload.size());
										if (!send_all(connected_socket, reinterpret_cast<char const *>(frame.data()), frame.size())) {
											ws_ok = false;
											break;
										}
									} else if (msg.opcode == 0xa) { // pong
										// ignore
									} else if (msg.opcode == 0x1 || msg.opcode == 0x2) {
										// Sanitize payload for logging: replace control chars
										std::string m(msg.payload.data(), msg.payload.size());
										std::string safe;
										safe.reserve(m.size());
										for (size_t k = 0; k < m.size(); k++) {
											unsigned char ch = static_cast<unsigned char>(m[k]);
											if (ch < 0x20 || ch == 0x7f) {
												safe.push_back('?');
											} else {
												safe.push_back(static_cast<char>(ch));
											}
										}
										printlog(strformat("ws recv %u bytes: %s").u(msg.payload.size()).s(safe).str());
										// Echo back the received message
										printlog(strformat("ws sending echo (%u bytes)").u(msg.payload.size()).str());
										auto frame = WebSocket::make_frame(msg.opcode, msg.payload.data(), msg.payload.size());
										if (!send_all(connected_socket, reinterpret_cast<char const *>(frame.data()), frame.size())) {
											ws_ok = false;
											break;
										}
									}
								}
							}
							if (!ws_ok) {
								// Send Close frame (status 1000) on protocol/error per RFC 6455 7.4
								unsigned char close_payload[2] = { 0x03, 0xe8 }; // 1000
								auto frame = WebSocket::make_frame(0x8, reinterpret_cast<char const *>(close_payload), 2);
								send_all(connected_socket, reinterpret_cast<char const *>(frame.data()), frame.size());
							}
							break;
						}
						if (request.content_length > 0 && !request.content_consumed && sockbuff.connected) {
							std::vector<char> discard;
							size_t remaining = request.content_length;
							while (remaining > 0 && sockbuff.connected) {
								size_t to_read = std::min<size_t>(remaining, 65536);
								discard.clear();
								sockbuff.read(&discard, to_read);
								if (discard.empty()) {
									sockbuff.connected = false;
									break;
								}
								remaining -= discard.size();
							}
						}
						if (response.keepalive != ConnectionType::KeepAlive) {
							break;
						}
					}
				}
				(void)web_socket_upgraded;
				shutdown(connected_socket, SHUT_RDWR);
				closesocket(connected_socket);
			}
		} catch (char const *e) {
			printlog(std::string("http thread error: ") + e);
		} catch (std::string const &e) {
			printlog(std::string("http thread error: ") + e);
		} catch (std::exception const &e) {
			printlog(std::string("http thread error: ") + e.what());
		} catch (...) {
			printlog("http thread: unknown exception");
		}
	}
};

void RequestHandler::write(http_response_t *response, void const *ptr, int len)
{
	unsigned char const *p = (unsigned char const *)ptr;
	response->content.insert(response->content.end(), p, p + len);
}

HTTP_Server::HTTP_Server(HTTP_Handler *handler)
{
	m = new Private();
	m->http_handler = handler;
	m->tcp_port = 80;
	m->bind_addr = "127.0.0.1"; // default: loopback only (do not expose to network by default)
}

HTTP_Server::~HTTP_Server()
{
	delete m;
}

void HTTP_Server::setPort(int port)
{
	m->tcp_port = port;
}

void HTTP_Server::setBindAddress(std::string const &addr)
{
	m->bind_addr = addr;
}

static bool validate_url(std::string const &url)
{
	if (url.empty() || url[0] != '/') {
		return false;
	}
	if (url.find('\\') != std::string::npos) {
		return false;
	}
	if (url.find("..") != std::string::npos) {
		return false;
	}
	return (bool)misc::normalize_path(url);
}

http_status_t const *HTTP_Server::http_process_request(HTTP_Thread *thread, http_request_t *request, http_response_t *response, HTTPIO *io)
{
	(void)thread;
	for (std::string const &line : request->header) {
		printlog(mask_header_for_log(line));
	}
	if (!m->http_handler) {
		return http503_service_unavailable;
	}
	if (request->method == RequestMethod::GET || request->method == RequestMethod::POST) {
		if (!validate_url(request->uri)) {
			return http400_bad_request;
		}
		response->content.clear();
		response->content.reserve(65536);
		return m->http_handler->do_get(this, request->uri, request, response, io);
	}
	return http405_method_not_allowed;
}

bool HTTP_Server::http_send_response_header(socket_t sock, http_status_t const *status, std::vector<std::string> const &header)
{
	std::vector<unsigned char> out;
	std::vector<std::string> deduped_header;
	out.reserve(1024);
	deduped_header.reserve(header.size());
	{
		char tmp[100];
		sprintf(tmp, "HTTP/1.1 %03u %s\r\n", status->code, status->text);
		out.insert(out.end(), tmp, tmp + strlen(tmp));
	}
	if (!header.empty()) {
		for (std::vector<std::string>::const_iterator it = header.begin(); it != header.end(); it++) {
			if (it->find(':') == std::string::npos) {
				continue;
			}
			bool replaced = false;
			for (std::string &existing : deduped_header) {
				if (misc::same_header_name(existing, *it)) {
					existing = *it;
					replaced = true;
					break;
				}
			}
			if (!replaced) {
				deduped_header.push_back(*it);
			}
		}
		for (std::vector<std::string>::const_iterator it = deduped_header.begin(); it != deduped_header.end(); it++) {
			unsigned char const *p = (unsigned char const *)it->c_str();
			out.insert(out.end(), p, p + it->size());
			out.push_back('\r');
			out.push_back('\n');
		}
		out.push_back('\r');
		out.push_back('\n');
	}
	return send_all(sock, (char const *)&out[0], out.size());
}

bool HTTP_Server::run()
{
	try {
		std::vector<HTTP_Thread> threads;

		int ret;

		struct sockaddr_in sin;

		m->listening_socket = socket(AF_INET, SOCK_STREAM, 0);
		if (m->listening_socket == -1) {
			throw std::string("socket");
		}

		{
			int val = 1;
			if (setsockopt(m->listening_socket, SOL_SOCKET, SO_REUSEADDR, (char const *)&val, sizeof(val)) == -1) {
				throw std::string("setsockopt");
			}
		}

		sin.sin_family = AF_INET;
		sin.sin_port = htons(m->tcp_port);
		// Bind to configured address (default 127.0.0.1 to avoid unintended exposure).
		// Empty/invalid address falls back to INADDR_ANY only when explicitly requested via "0.0.0.0".
		std::string bind_addr = m->bind_addr;
		if (bind_addr.empty()) {
			bind_addr = "127.0.0.1";
		}
		sin.sin_addr.s_addr = inet_addr(bind_addr.c_str());
		if (sin.sin_addr.s_addr == INADDR_NONE) {
			throw std::string("invalid bind address");
		}

		if (bind(m->listening_socket, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			throw std::string("bind");
		}

		ret = listen(m->listening_socket, SOMAXCONN);
		if (ret == -1) {
			throw std::string("listen");
		}

#ifndef _WIN32
		install_termination_handlers();
		g_shutdown_requested = 0;
		g_listening_socket_for_signal = m->listening_socket;
#endif

		int threadcount = 8;
		threads.resize(threadcount);
		for (int i = 0; i < threadcount; i++) {
			threads[i].setup(this);
		}
		for (int i = 0; i < threadcount; i++) {
			threads[i].start();
		}

		while (true) {
#ifdef _WIN32
			Sleep(100);
#else
			if (g_shutdown_requested) {
				break;
			}
			usleep(100000);
#endif
		}

		if (m->listening_socket != -1) {
			shutdown(m->listening_socket, SHUT_RDWR);
			ret = closesocket(m->listening_socket);
			m->listening_socket = -1;
			if (ret == -1) {
				throw std::string("close");
			}
		}

		for (int i = 0; i < threadcount; i++) {
			threads[i].stop();
			threads[i].join();
		}
	} catch (std::string const &e) {
		printlog(std::string("http server error: ") + e);
		return false;
	}

	return true;
}

//

std::string http_request_t::header_value(std::string const &name) const
{
	for (std::vector<std::string>::const_iterator it = header.begin(); it != header.end(); it++) {
		if (strnicmp(it->c_str(), name.c_str(), name.size()) == 0 && it->c_str()[name.size()] == ':') {
			char const *left = it->c_str();
			char const *right = left + it->size();
			left += name.size() + 1;
			while (left < right && isspace(left[0] & 0xff))
				left++;
			while (left < right && isspace(right[-1] & 0xff))
				right--;
			return std::string(left, right);
		}
	}
	return std::string();
}

void http_request_t::read_content(std::vector<char> *out, size_t maxlen)
{
	if (maxlen == 0) {
		maxlen = content_length;
	}
	sockbuff->read(out, maxlen);
	content_consumed = true;
}
