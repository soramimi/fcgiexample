
#ifndef __SkyHTTPD_h
#define __SkyHTTPD_h

#include "httpstatus.h"
#include "socket.h"
#include <map>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#ifndef _WIN32
#define stricmp(A, B) strcasecmp(A, B)
#endif

struct HTTP_Thread;
struct http_request_t;
class http_response_t;

struct SocketBuffer {
	socket_t sock;
	std::vector<unsigned char> buffer;
	int length;
	int offset;
	bool connected;
	size_t max_line_length;
	size_t max_header_total_length;

	SocketBuffer()
		: max_line_length(8192)
		, max_header_total_length(65536)
	{
		clear();
	}
	void clear()
	{
		buffer.clear();
		length = 0;
		offset = 0;
		connected = false;
	}
	std::string readline();
	void read(std::vector<char> *out, size_t maxlen);
};

enum class RequestMethod {
	INVALID,
	GET,
	POST,
};

struct http_request_t {
	std::vector<std::string> header;
	SocketBuffer *sockbuff = nullptr;

	std::string header_value(std::string const &name) const;

	void read_content(std::vector<char> *out, size_t maxlen);

	std::string protocol;
	RequestMethod method = RequestMethod::INVALID;
	struct {
		int maj = 0;
		int min = 0;
	} protocol_version;
	std::string uri;
	std::string scheme;
	size_t content_length = 0;
	bool content_consumed = false;
};

struct RequestHandler {
	void write(http_response_t *response, void const *ptr, int len);
	virtual http_status_t const *process(std::string const &url, std::string const &suffix, SocketBuffer *sockbuff, http_request_t *request, http_response_t *response) = 0;
};

class HTTP_Server;

enum class ConnectionType {
	Close,
	KeepAlive,
	UpgradeWebSocket,
};

class HTTPIO {
public:
	virtual void write_(char const *begin, char const *end) = 0;
	void write(char const *begin, char const *end)
	{
		write_(begin, end);
	}
	void write(char const *text)
	{
		write(text, strlen(text));
	}
	void write(char const *text, size_t len)
	{
		write(text, text + len);
	}
	void write(std::string_view const &str)
	{
		char const *begin = str.data();
		char const *end = begin + str.size();
		write(begin, end);
	}
	void write(std::vector<char> const *vec)
	{
		if (vec && !vec->empty()) {
			char const *begin = &vec->at(0);
			char const *end = begin + vec->size();
			write(begin, end);
		}
	}
};

class http_response_t : public HTTPIO {
public:
	std::vector<char> content;

	ConnectionType keepalive = ConnectionType::Close;

	void write_(char const *begin, char const *end) override
	{
		content.insert(content.end(), begin, end);
	}
};

class HTTP_Handler {
public:
	virtual http_status_t const *do_get(HTTP_Server *server, std::string const &url, http_request_t *request, http_response_t *response, HTTPIO *io) = 0;
	virtual http_status_t const *do_post(HTTP_Server *server, std::string const &url, http_request_t *request, http_response_t *response, HTTPIO *io) = 0;
};

class HTTP_Server {
	friend class HTTP_Thread;

private:
	struct Private;
	Private *m;
public:
	HTTP_Server(HTTP_Handler *handler);
	~HTTP_Server();

	void setPort(int port);
	void setBindAddress(std::string const &addr);

	http_status_t const *http_process_request(HTTP_Thread *thread, http_request_t *request, http_response_t *response, HTTPIO *io);
	bool http_send_response_header(socket_t sock, http_status_t const *status, std::vector<std::string> const &response);

	void start();
	void join();
	void stop();
	
	bool run();
	
};

#endif
