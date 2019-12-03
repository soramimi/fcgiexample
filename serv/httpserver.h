
#ifndef __SkyHTTPD_h
#define __SkyHTTPD_h

#include "httpstatus.h"
#include "socket.h"
#include <map>
#include <string.h>
#include <string>
#include <vector>

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

	SocketBuffer()
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
	void read(std::vector<char> *out, int maxlen);
};

enum class RequestMethod {
	INVALID,
	GET,
	POST,
};

struct http_request_t {
	std::vector<std::string> header;
	SocketBuffer *sockbuff;

	std::string header_value(std::string const &name) const;

	void read_content(std::vector<char> *out, int maxlen);

	std::string protocol;
	RequestMethod method;
	struct {
		int maj = 0;
		int min = 0;
	} protocol_version;
	std::string uri;
	std::string scheme;
	unsigned int content_length = 0;
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
	void write(char const *text, size_t len = -1)
	{
		write(text, text + (len == -1 ? strlen(text) : len));
	}
	void write(std::string const &str)
	{
		char const *begin = str.c_str();
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

	http_status_t const *http_process_request(HTTP_Thread *thread, http_request_t *request, http_response_t *response, HTTPIO *io);
	void http_send_response_header(socket_t sock, http_status_t const *status, std::vector<std::string> const &response);

	bool run();
};

#endif

