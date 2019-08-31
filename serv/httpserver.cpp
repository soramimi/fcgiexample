#ifdef _WIN32
#define NOMINMAX
#endif

#include "socket.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#define O_BINARY 0
#define strnicmp(A, B, C) strncasecmp(A, B, C)
#endif

#include "httpserver.h"
#include "joinpath.h"
#include "misc.h"
#include "thread.h"
#include <algorithm>
#include <map>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <vector>

std::string SocketBuffer::readline()
{
	if (buffer.empty()) {
		buffer.resize(1024);
	}
	while (1) {
		if (offset < length) {
			unsigned char *left = &buffer[offset];
			unsigned char *right = (unsigned char *)memchr(left, '\n', length - offset);
			if (right) {
				offset = right + 1 - &buffer[0];
				while (left < right && isspace(right[-1])) {
					right--;
				}
				return std::string(left, right);
			}
			int n = length - offset;
			memmove(&buffer[0], left, n);
			length = n;
			offset = 0;
		} else {
			length = 0;
			offset = 0;
		}
		int l = ::recv(sock, (char *)&buffer[0] + length, buffer.size() - length, 0);
		if (l < 1) {
			connected = false;
			return std::string();
		}
		length += l;
	}
}

void SocketBuffer::read(std::vector<char> *out, int maxlen)
{
	out->clear();
	if (buffer.empty()) {
		buffer.resize(4096);
	}
	while (1) {
		if (offset < length) {
			bool end = false;
			int n = length - offset;
			if (maxlen >= 0) {
				if (n >= maxlen) {
					n = maxlen;
					end = true;
				}
				maxlen -= n;
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

int send(socket_t sock, char const *ptr, int len)
{
	char const *begin = ptr;
	char const *end = ptr + len;
	while (ptr < end) {
		int n = end - ptr;
		n = std::min(n, 65536);
		n = ::send(sock, ptr, n, 0);
		if (n < 1) break;
		if (ptr + n > end) break;
		ptr += n;
	}
	return ptr - begin;
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

struct HTTP_Server::Private {
	int tcp_port;
	socket_t listening_socket;

	HTTP_Handler *http_handler;

	RequestHandlerMap request_handler_map;
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
			while (true) {
				struct sockaddr_in peer_sin;

				socklen_t len = sizeof(peer_sin);

				socket_t connected_socket = accept(server->m->listening_socket, (struct sockaddr *)&peer_sin, &len);
				if (connected_socket == -1) {
					throw "accept failed";
				}

				sockbuff.clear();
				sockbuff.sock = connected_socket;
				sockbuff.connected = true;

				while (sockbuff.connected) {
					http_request_t request;
					request.sockbuff = &sockbuff;

					while (1) {
						std::string line = sockbuff.readline();
						if (line.empty()) {
							break;
						}
						request.header.push_back(line);
					}
					if (request.header.size() > 0) {
						http_response_t response;
						http_status_t const *status = server->http_process_request(this, &request, &response);
						if (status->code / 100 >= 4) {
							if (response.content.empty()) {
								response.print("Content-Type: text/plain\r\n\r\n");
								char tmp[10];
								sprintf(tmp, "%u ", status->code);
								response.print(tmp);
								response.print(status->text);
							}
						}
						if (response.content.empty()) {
							status = http_204_no_content;
						} else {
							std::vector<std::string> header;
							char const *begin = &response.content[0];
							char const *end = begin + response.content.size();
							char const *ptr = parse_header(begin, end, &header);
							int len = end - ptr;
							{
								char tmp[100];
								sprintf(tmp, "Content-Length: %u", len);
								header.push_back(tmp);
							}
							if (response.keepalive) {
								header.push_back("Connection: keep-alive");
							}
							server->http_send_response_header(sockbuff.sock, status, header);
							send(sockbuff.sock, ptr, len);
						}
						if (!response.keepalive) {
							break;
						}
					}
				}

				closesocket(connected_socket);
			}
		} catch (char const *) {
		} catch (std::string const &) {
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
}

HTTP_Server::~HTTP_Server()
{
	delete m;
}

void HTTP_Server::setPort(int port)
{
	m->tcp_port = port;
}

static bool validate_url(std::string const &url)
{
	char const *p = url.c_str();
	if (strchr(p, '\\')) {
		return false;
	}
	if (strstr(p, "..")) {
		return false;
	}
	if (strstr(p, "//")) {
		return false;
	}
	if (strstr(p, "/.")) {
		return false;
	}
	return true;
}

static std::string make_location(std::string const &url)
{
	return "Location: " + url;
}

http_status_t const *HTTP_Server::http_process_request(HTTP_Thread *thread, http_request_t *request, http_response_t *response)
{
	std::vector<std::string> first;
	misc::split_words_by_space(request->header[0], &first);
	if (first.size() < 3) {
		return http_400_bad_request;
	}
	response->keepalive = false;
	request->protocol_version.major = 1;
	request->protocol_version.minor = 0;
	{
		std::vector<std::string> prot;
		misc::split_words(request->header[0], '/', &prot);
		if (prot.size() == 2) {
			if (prot[0] == "HTTP") {
				int major, minor;
				if (sscanf(prot[0].c_str(), "%u.%u", &major, &minor) == 2) {
					request->protocol_version.major = major;
					request->protocol_version.minor = minor;
					if (major >= 1 && minor >= 1) {
						response->keepalive = true;
					}
				}
			}
		}
	}
	{
		std::string s = request->get("Connection");
		if (stricmp(s.c_str(), "close") == 0) {
			response->keepalive = false;
		} else if (stricmp(s.c_str(), "keep-alive") == 0) {
			response->keepalive = true;
		}
	}
	if (!m->http_handler) {
		return http_503_service_unavailable;
	}
	request->method = (Method)-1;
	if (first[0] == "GET") {
		request->method = HTTP_GET;
	} else if (first[0] == "POST") {
		request->method = HTTP_POST;
	}
	if (request->method == HTTP_GET || request->method == HTTP_POST) {
		std::string url = first[1];
		if (!validate_url(url)) {
			return http_400_bad_request;
		}
		response->content.clear();
		response->content.reserve(65536);
		return m->http_handler->do_get(this, url, request, response);
	}
	return http_405_method_not_allowed;
}

void HTTP_Server::http_send_response_header(socket_t sock, http_status_t const *status, std::vector<std::string> const &header)
{
	std::vector<unsigned char> out;
	out.reserve(1024);
	{
		char tmp[100];
		sprintf(tmp, "HTTP/1.1 %03u %s\r\n", status->code, status->text);
		out.insert(out.end(), tmp, tmp + strlen(tmp));
	}
	if (!header.empty()) {
		for (std::vector<std::string>::const_iterator it = header.begin(); it != header.end(); it++) {
			unsigned char const *p = (unsigned char const *)it->c_str();
			out.insert(out.end(), p, p + it->size());
			out.push_back('\r');
			out.push_back('\n');
		}
		out.push_back('\r');
		out.push_back('\n');
	}
	send(sock, (char const *)&out[0], out.size());
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
		sin.sin_addr.s_addr = htonl(INADDR_ANY);

		if (bind(m->listening_socket, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			throw std::string("bind");
		}

		ret = listen(m->listening_socket, SOMAXCONN);
		if (ret == -1) {
			throw std::string("listen");
		}

		int threadcount = 1;
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
			usleep(100000);
#endif
		}

		ret = closesocket(m->listening_socket);
		if (ret == -1) {
			throw std::string("close");
		}

		for (int i = 0; i < threadcount; i++) {
			threads[i].join();
		}
	} catch (std::string const &) {
		return false;
	}

	return true;
}

//

std::string http_request_t::get(const std::string &name) const
{
	for (std::vector<std::string>::const_iterator it = header.begin(); it != header.end(); it++) {
		if (strnicmp(it->c_str(), name.c_str(), name.size()) == 0 && it->c_str()[name.size()] == ':') {
			char const *left = it->c_str();
			char const *right = left + it->size();
			left += name.size() + 1;
			while (left < right && isspace(left[0] & 0xff)) left++;
			while (left < right && isspace(right[-1] & 0xff)) right--;
			return std::string(left, right);
		}
	}
	return std::string();
}

void http_request_t::read_content(std::vector<char> *out, int maxlen)
{
	sockbuff->read(out, maxlen);
}

