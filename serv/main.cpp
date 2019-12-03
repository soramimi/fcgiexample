
#include "FcgiProcess.h"
#include "debug.h"
#include "httpserver.h"
#include "misc.h"
#include "socket.h"
#include <list>
#include <memory>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include "base64.h"
#include "sha1.h"
#include "strformat.h"

#ifdef _WIN32
#include <mbctype.h>
#include <shlobj.h>
#include <direct.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#define strnicmp(A, B, C) strncasecmp(A, B, C)
#define MAX_PATH PATH_MAX
#endif

namespace http {
struct SocketBuffer;
}

#ifdef _WIN32
std::string get_current_dir()
{
	char tmp[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, tmp);
	return tmp;
}
#else
std::string get_current_dir()
{
	char *p = getcwd(0, 0);
	std::string s = p;
	free(p);
	return s;
}

#endif

class MyHandler : public HTTP_Handler {
public:
	struct FormPart {
		std::vector<std::string> header;
		std::vector<char> data;
	};
private:

//	Connection pipe;
	std::string pipepath;
	std::shared_ptr<AbstractFcgi> proc;

	struct ContentType {
		std::string mime;
		std::string boundary;
		unsigned int length;
		ContentType()
			: length(0)
		{
		}
	};

	int get_content_length(http_request_t const *request) const
	{
		return strtol(request->header_value("Content-Length").c_str(), 0, 10);
	}

	void parse_content_type(http_request_t const *request, ContentType *out) const
	{
		*out = ContentType();
		std::string contype = request->header_value("Content-Type");
		char const *begin = contype.c_str();
		char const *end = begin + contype.size();
		std::vector<std::string> vec;
		misc::split_words(begin, end, ';', &vec);
		if (!vec.empty()) {
			out->mime = vec[0];
			for (std::vector<std::string>::const_iterator it = vec.begin(); it != vec.end(); it++) {
				if (strnicmp(it->c_str(), "boundary=", 9) == 0) {
					out->boundary = it->c_str() + 9;
				}
			}
		}
		out->length = get_content_length(request);

	}

	void parse_form_part(char const *begin, char const *end, FormPart *out) const
	{
		std::vector<std::string> header;
		char const *ptr = begin;
		char const *left = ptr;
		char const *right = ptr;
		while (1) {
			int c = 0;
			if (ptr < end) {
				c = *ptr & 0xff;
			}
			if (c == 0 || c == '\n' || c == '\r') {
				right = ptr;
				if (c == '\r') {
					ptr++;
					if (ptr < end && *ptr == '\n') {
						ptr++;
					}
				} else if (c == '\n') {
					ptr++;
				}
				if (left == right) {
					break;
				}
				std::string line(left, right);
				header.push_back(line);
				left = ptr;
			} else {
				ptr++;
			}
		}
		*out = FormPart();
		out->header = header;
		if (ptr < end && end[-1] == '\n') {
			end--;
			if (ptr < end && end[-1] == '\r') {
				end--;
			}
		} else if (ptr < end && end[-1] == '\r') {
			end--;
		}
		out->data.insert(out->data.end(), ptr, end);
	}

	void parse_multipart(char const *begin, char const *end, std::string const &boundary, std::list<FormPart> *parts) const
	{
		char const *ptr = begin;
		char const *part_begin = 0;
		char const *part_end = 0;
		while (ptr + boundary.size() + 1 < end) {
			if (ptr[0] == '-' && ptr[1] == '-') {
				if (memcmp(ptr + 2, boundary.c_str(), boundary.size()) == 0) {
					char const *next = ptr + boundary.size() + 2;
					if (part_begin) {
						part_end = ptr;
						if (part_begin < part_end && *part_begin == '\n') {
							part_begin++;
						} else if (part_begin < part_end && *part_begin == '\r') {
							part_begin++;
							if (part_begin < part_end && *part_begin == '\n') {
								part_begin++;
							}
						}
						if (part_begin < part_end) {
							std::list<FormPart>::iterator it = parts->insert(parts->end(), FormPart());
							parse_form_part(part_begin, part_end, &*it);
						}
						part_end = 0;
					}
					part_begin = next;
					ptr = next;
					continue;
				}
			}
			ptr++;
		}
	}

	static std::string get(std::vector<NameValue> const *vec, std::string const &name, bool *ok = 0)
	{
		for (std::vector<NameValue>::const_iterator it = vec->begin(); it != vec->end(); it++) {
			if (it->name() == name) {
				if (ok) *ok = true;
				return it->value();
			}
		}
		if (ok) *ok = false;
		return std::string();
	}

	static std::string to_s(std::vector<char> const *vec)
	{
		std::string s;
		if (vec && !vec->empty()) {
			char const *begin = &vec->at(0);
			char const *end = begin + vec->size();
			s.assign(begin, end);
		}
		return s;
	}

	static std::string to_s(size_t n)
	{
		char tmp[100];
		sprintf(tmp, "%u", (unsigned int)n);
		return tmp;
	}

	void setEnvironment(std::vector<NameValue> *list, std::string const &name, std::string const &value)
	{
		for (size_t i = 0; i < list->size(); i++) {
			if (name == list->at(i).name()) {
				list->at(i).setValue(value);
				return;
			}
		}
		list->emplace_back(name, value);
	}

	void makeEnvironment(http_request_t const *request, std::vector<NameValue> *out)
	{
		auto MAKE = [&](std::string const &name){
			setEnvironment(out, name, std::string());
		};
		out->clear();
		setEnvironment(out, "FCGI_ROLE", "RESPONDER");
		MAKE("HTTP_HOST"); // =localhost
		MAKE("HTTP_CONNECTION"); // =keep-alive
		MAKE("HTTP_UPGRADE_INSECURE_REQUESTS"); // =1
		MAKE("HTTP_USER_AGENT"); // =Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/77.0.3865.90 Safari/537.36
		MAKE("HTTP_SEC_FETCH_MODE"); // =navigate
		MAKE("HTTP_SEC_FETCH_USER"); // =?1
		MAKE("HTTP_ACCEPT"); // =text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3
		MAKE("HTTP_SEC_FETCH_SITE"); // =none
		MAKE("HTTP_ACCEPT_ENCODING"); // =gzip, deflate, br
		MAKE("HTTP_ACCEPT_LANGUAGE"); // =ja,en-US;q=0.9,en;q=0.8
		MAKE("PATH"); // =/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
		MAKE("SERVER_SIGNATURE"); // =<address>Apache/2.4.18 (Ubuntu) Server at localhost Port 80</address>
		MAKE("SERVER_SOFTWARE"); // =Apache/2.4.18 (Ubuntu)
		MAKE("SERVER_NAME"); // =localhost
		MAKE("SERVER_ADDR"); // =::1
		MAKE("SERVER_PORT"); // =80
		MAKE("REMOTE_ADDR"); // =::1
		MAKE("DOCUMENT_ROOT"); // =/var/www/html
		MAKE("REQUEST_SCHEME"); // =http
		MAKE("CONTEXT_PREFIX"); // =
		MAKE("CONTEXT_DOCUMENT_ROOT"); // =/var/www/html
		MAKE("SERVER_ADMIN"); // =webmaster@localhost
		MAKE("SCRIPT_FILENAME"); // =proxy:fcgi://localhost:3000/
		MAKE("REMOTE_PORT"); // =38214
		MAKE("GATEWAY_INTERFACE"); // =CGI/1.1
		MAKE("SERVER_PROTOCOL"); // =HTTP/1.1
		MAKE("REQUEST_METHOD"); // =GET
		MAKE("QUERY_STRING"); // =
		MAKE("REQUEST_URI"); // =/app/
		MAKE("SCRIPT_NAME"); // =/app/

		if (request->method == RequestMethod::GET) {
			setEnvironment(out, "REQUEST_METHOD", "GET");
		} else if (request->method == RequestMethod::POST) {
			setEnvironment(out, "REQUEST_METHOD", "POST");
		}

		setEnvironment(out, "SERVER_PROTOCOL", strformat("%s/%u.%u")
					   .s(request->protocol)
					   .u(request->protocol_version.maj)
					   .u(request->protocol_version.min)
					   .str());
		setEnvironment(out, "GATEWAY_INTERFACE", "CGI/1.1");
		setEnvironment(out, "REQUEST_URI", request->uri);
		if (request->content_length > 0) {
			setEnvironment(out, "CONTENT_LENGTH", strformat("%u").u(request->content_length).str());
		}
	}

	struct FCGI_Header {
		unsigned char version;
		unsigned char type;
		unsigned char requestIdB1;
		unsigned char requestIdB0;
		unsigned char contentLengthB1;
		unsigned char contentLengthB0;
		unsigned char paddingLength;
		unsigned char reserved;
	};

	struct FCGI_BeginRequestBody {
		unsigned char roleB1;
		unsigned char roleB0;
		unsigned char flags;
		unsigned char reserved[5];
	};

	struct FCGI_BeginRequestRecord {
		FCGI_Header header;
		FCGI_BeginRequestBody body;
	};

	struct FCGI_EndRequestBody {
		unsigned char appStatusB3;
		unsigned char appStatusB2;
		unsigned char appStatusB1;
		unsigned char appStatusB0;
		unsigned char protocolStatus;
		unsigned char reserved[3];
	};

	struct FCGI_EndRequestRecord {
		FCGI_Header header;
		FCGI_EndRequestBody body;
	};

	enum {
		FCGI_RESPONDER  = 1,
		FCGI_AUTHORIZER = 2,
		FCGI_FILTER     = 3,
	};

	enum {
		FCGI_BEGIN_REQUEST      = 1,
		FCGI_ABORT_REQUEST      = 2,
		FCGI_END_REQUEST        = 3,
		FCGI_PARAMS             = 4,
		FCGI_STDIN              = 5,
		FCGI_STDOUT             = 6,
		FCGI_STDERR             = 7,
		FCGI_DATA               = 8,
		FCGI_GET_VALUES         = 9,
		FCGI_GET_VALUES_RESULT  =10,
	};

	http_status_t const *invoke_fastcgi(http_request_t *request, http_response_t *response, HTTPIO *io)
	{
#ifdef _WIN32
		char const *cmd = "C:/develop/tinyfcgi/app/tinyfcgi.exe";
#else
//		char const *cmd = "./fcgiapp";
//		char const *cmd = "unix:/tmp/foo.sock";
		char const *cmd = "inet:localhost:3000";
#endif

		std::string cwd;
		{
			int i = misc::last_index_of(cmd, '/');
			int j = misc::last_index_of(cmd, '\\');
			if (i < j) i = j;
			if (i > 0) {
				std::string dir(cmd, cmd + i);

				cwd = get_current_dir();
#ifdef _WIN32
				_chdir(dir.c_str());
#else
				chdir(dir.c_str());
#endif
			}
		}
		{
			std::vector<NameValue> env;
			makeEnvironment(request, &env);

			if (strncmp(cmd, "unix:", 5) == 0) {
				proc = std::make_shared<FcgiUnixSocket>(cmd + 5);
				proc->launch("");
			} else if (strncmp(cmd, "inet:", 5) == 0) {
				proc = std::make_shared<FcgiInetSocket>(cmd + 5);
				proc->launch("");
			} else if (!proc.get()) {
#ifdef _WIN32
				proc = std::shared_ptr<FcgiProcess>(new FcgiProcess(pipepath, pipe));
#else
				proc = std::make_shared<FcgiProcess>(pipepath);
#endif
				proc->launch(cmd);
			}
			if (!proc->connect()) {
				return http503_service_unavailable;
			}

			uint16_t reqid = 1;

			auto make_fcgi_header = [&](std::vector<char> *vec, int type){
				FCGI_Header *h = (FCGI_Header *)&vec->at(0);
				h->version = 1;
				h->type = type;
				h->requestIdB1 = reqid >> 8;
				h->requestIdB0 = reqid;
				auto setcontentlength = [](FCGI_Header *h, uint16_t len){
					h->contentLengthB1 = len >> 8;
					h->contentLengthB0 = len;
				};
				setcontentlength(h, vec->size() - sizeof(FCGI_Header));
			};

			auto write_fcgi_begin_request = [&](int role){
				std::vector<char> vec(sizeof(FCGI_BeginRequestRecord));
				FCGI_BeginRequestRecord *beginreq = (FCGI_BeginRequestRecord *)&vec[0];
				beginreq->body.roleB1 = role >> 8;
				beginreq->body.roleB0 = role;
				beginreq->body.flags = 0;
				make_fcgi_header(&vec, FCGI_BEGIN_REQUEST);
				proc->write(&vec[0], vec.size());
			};
			write_fcgi_begin_request(FCGI_RESPONDER);

			auto write_fcgi_params = [&](std::vector<NameValue> const &params){
				std::vector<char> vec;
				vec.reserve(1024);
				vec.resize(sizeof(FCGI_Header));
				auto append_name_value = [&vec](std::string const &name, std::string const &value){
					vec.push_back(name.size());
					vec.push_back(value.size());
					auto append_string = [&vec](std::string const &s){
						char const *begin = s.c_str();
						char const *end = begin + s.size();
						vec.insert(vec.end(), begin, end);

					};
					append_string(name);
					append_string(value);
				};
				for (NameValue const &param : params) {
					append_name_value(param.name(), param.value());
				}
				make_fcgi_header(&vec, FCGI_PARAMS);
				proc->write(&vec[0], vec.size());
			};

			write_fcgi_params(env);
			std::vector<NameValue> params;
			params.clear();
			write_fcgi_params(params);

			if (1) {
				std::vector<char> vec(sizeof(FCGI_Header));
				make_fcgi_header(&vec, FCGI_STDIN);
				proc->write(&vec[0], vec.size());
			}

			{
				size_t pos = 0;
				size_t need = 0;
				FCGI_Header header;
				uint16_t contentlength = 0;
				char tmp[65536 + 256];
				while (1) {
					if (need == 0) {
						need = sizeof(FCGI_Header);
					}
					while (pos < need) {
						int n = proc->read(tmp + pos, need - pos);
						if (n < 0) break;
						pos += n;
					}
					if (pos == sizeof(FCGI_Header)) {
						memcpy(&header, tmp, sizeof(FCGI_Header));
						contentlength = ((uint16_t)header.contentLengthB1 << 8) | header.contentLengthB0;
						if (header.type == FCGI_STDOUT) {
							if (contentlength == 0) {
								pos = 0;
								need = 0;
								continue;
							}
						}
						need = sizeof(FCGI_Header) + contentlength + header.paddingLength;
					} else if (contentlength > 0 && pos == sizeof(FCGI_Header) + contentlength + header.paddingLength) {
						char const *p = tmp + sizeof(FCGI_Header);
						if (header.type == FCGI_STDOUT) {
							io->write(p, contentlength);
						} else if (header.type == FCGI_END_REQUEST) {
							if (pos >= sizeof(FCGI_Header) + sizeof(FCGI_EndRequestBody)) {
								FCGI_EndRequestBody const *h = (FCGI_EndRequestBody const *)&tmp[sizeof(FCGI_Header)];
								uint32_t appstat = (h->appStatusB3 << 24) | (h->appStatusB2 << 16) | (h->appStatusB1 << 8) | h->appStatusB0;
								uint32_t protstat = h->protocolStatus;
								(void)appstat;
								(void)protstat;
							}
							break;
						}
						pos = 0;
						need = 0;
						contentlength = 0;
						continue;
					}
					if (need == sizeof(FCGI_Header)) {
						break;
					}
				}
			}

			proc->disconnect();

			if (!cwd.empty()) {
#ifdef _WIN32
				_chdir(cwd.c_str());
#else
				chdir(cwd.c_str());
#endif
			}
			return http200_ok;
		}
		return nullptr;
	}

public:
	virtual http_status_t const *do_get(HTTP_Server *server, std::string const &url, http_request_t *request, http_response_t *response, HTTPIO *io)
	{
		std::string location = url;
		std::string question;
		{
			char const *left = url.c_str();
			char const *right = strchr(left, '?');
			if (right) {
				question = right + 1;
				while (left < right && right[-1] == '/') right--;
				location.assign(left, right);
			}
		}
		if (location == "/app/") {
			auto p = invoke_fastcgi(request, response, response);
			return p ? p : http502_bad_gateway;
		} else if (location == "/hello/") {
			response->write("Content-Type: text/plain\r\n");
			response->write("Connection: close\r\n");
			response->write("\r\n");
			response->write("Hello, world\r\n");
			return http200_ok;
		} else if (location == "/sock/") {
			std::string sec = request->header_value("Sec-WebSocket-Key");
			if (!sec.empty()) {
				sec += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
				{
					std::vector<char> h;
					uint8_t hash[20];
					SHA1Context c;
					SHA1Reset(&c);
					SHA1Input(&c, (uint8_t const *)sec.c_str(), sec.size());
					SHA1Result(&c, hash);
					base64_encode((char const *)hash, 20, &h);
					sec.assign(h.data(), h.size());
				}
				response->write("Upgrade: websocket\r\n");
				response->write("Connection: upgrade\r\n");
				response->write("Sec-WebSocket-Accept: " + sec + "\r\n");
				response->write("\r\n");
				response->keepalive = ConnectionType::UpgradeWebSocket;
				return http101_switching_protocols;
			}
		}

		return http404_not_found;
	}

	virtual http_status_t const *do_post(HTTP_Server *server, std::string const &url, http_request_t *request, http_response_t *response, HTTPIO *io)
	{
		return do_get(server, url, request, response, io);
	}

	std::string makePipePath(std::string name)
	{
		char tmp[100];
#ifdef _WIN32
		unsigned int pid = GetCurrentProcessId();
		sprintf(tmp, "\\\\.\\pipe\\%s_%u_", name.c_str(), pid);
#else
		unsigned int pid = getpid();
		sprintf(tmp, "/tmp/fcgi_%s_%u_.sock", name.c_str(), pid);
#endif
		return tmp;
	}

	void createPipe()
	{
//		pipe.create(pipepath);
	}

	MyHandler(std::string const &name)
	{
		pipepath = makePipePath(name);
		createPipe();
	}

	~MyHandler()
	{
		unlink(pipepath.c_str());
//		pipe.close();
	}
};



int main()
{
#ifdef _WIN32
	setlocale(LC_ALL, "Japanese_Japan.932");
	{
		int ret;
		WORD ver;
		WSADATA data;
		ver = MAKEWORD(1, 1);
		ret = WSAStartup(ver, &data);
		atexit((void (*)(void))(WSACleanup));
	}
#else
	startlog("tinyfcgiserver");
#endif

	MyHandler handler("tinyfcgiserver");

	HTTP_Server server(&handler);

//#ifdef _WIN32
//	std::string wwwroot = "C:/develop/tinyfcgiserver/wwwroot";
//#else
//	std::string wwwroot = "/home/soramimi/develop/tinyfcgiserver/wwwroot/";
//#endif
	server.setPort(5000);

	return server.run() ? 0 : 1;
}

