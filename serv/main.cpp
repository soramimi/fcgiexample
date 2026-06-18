
#include "FcgiProcess.h"
#include "debug.h"
#include "httpserver.h"
#include "joinpath.h"
#include "misc.h"
#include "socket.h"
#include <algorithm>
#include <list>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
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
#endif

namespace http {
struct SocketBuffer;
}

// RAII guard to restore the working directory on scope exit (B4).
// Ensures chdir() performed during FastCGI invocation is always reverted,
// even on early-return / exception paths.
class CwdGuard {
private:
	std::string saved_;
	bool valid_;
public:
	CwdGuard()
		: valid_(false)
	{
#ifdef _WIN32
		char tmp[PATH_MAX];
		DWORD n = GetCurrentDirectoryA(PATH_MAX, tmp);
		if (n > 0 && n < PATH_MAX) {
			saved_ = tmp;
			valid_ = true;
		}
#else
		char *p = getcwd(0, 0);
		if (p) {
			saved_ = p;
			valid_ = true;
			free(p);
		}
#endif
	}
	~CwdGuard()
	{
		restore();
	}
	void restore()
	{
		if (valid_ && !saved_.empty()) {
#ifdef _WIN32
			_chdir(saved_.c_str());
#else
			chdir(saved_.c_str());
#endif
			valid_ = false;
		}
	}
	std::string const & saved() const { return saved_; }
};

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
	bool proc_expired = false; // true when proc is a fork-based backend that must re-launch next time

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
		if (name.find('\r') != std::string::npos || name.find('\n') != std::string::npos ||
			value.find('\r') != std::string::npos || value.find('\n') != std::string::npos) {
			return; // prevent header injection
		}
		// Reject NUL bytes in name/value (CGI apps may treat values as C strings)
		if (name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
			return;
		}
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

	static const size_t MAX_STATIC_FILE_SIZE = 8 * 1024 * 1024; // 8MiB

	static std::string static_mime_type(std::string const &path)
	{
		size_t pos = path.rfind('.');
		if (pos == std::string::npos) {
			return "application/octet-stream";
		}
		std::string ext(path, pos + 1);
		struct {
			char const *ext;
			char const *mime;
		} map[] = {
			{ "html", "text/html" },
			{ "htm", "text/html" },
			{ "css", "text/css" },
			{ "js", "application/javascript" },
			{ "json", "application/json" },
			{ "png", "image/png" },
			{ "jpg", "image/jpeg" },
			{ "jpeg", "image/jpeg" },
			{ "gif", "image/gif" },
			{ "svg", "image/svg+xml" },
			{ "txt", "text/plain" },
			{ nullptr, nullptr }
		};
		for (int i = 0; map[i].ext; i++) {
			if (ext.size() == strlen(map[i].ext) && stricmp(ext.c_str(), map[i].ext) == 0) {
				return map[i].mime;
			}
		}
		return "application/octet-stream";
	}

	http_status_t const *serve_static_file(std::string const &normalized_url, http_request_t const *request, http_response_t *response)
	{
		// Hardcoded document root for the /static/ URL prefix.
		static std::string const prefix = "/static/";
		static std::string const root = "/home/soramimi/develop/fcgiexample/static";

		if (normalized_url.size() <= prefix.size() || normalized_url.compare(0, prefix.size(), prefix) != 0) {
			return nullptr;
		}

		if (request->method != RequestMethod::GET) {
			return http405_method_not_allowed;
		}

		std::string relpath = normalized_url.substr(prefix.size());
		if (relpath.empty() || relpath.back() == '/') {
			return nullptr; // directory listing is not supported -> 404
		}

		std::string normalized_relpath;
		if (!misc::normalize_path('/' + relpath, &normalized_relpath)) {
			return nullptr;
		}
		// normalized_relpath always starts with '/' because the input did.
		std::string relative = normalized_relpath.substr(1);
		if (relative.empty()) {
			return nullptr;
		}

		std::string fullpath = joinpath(root, relative);
		// Defensive check: the resolved path must remain inside the document root.
		if (fullpath.size() < root.size() || fullpath.compare(0, root.size(), root) != 0) {
			return nullptr;
		}
		if (fullpath.size() > root.size() && fullpath[root.size()] != '/') {
			return nullptr;
		}

		FILE *fp = fopen(fullpath.c_str(), "rb");
		if (!fp) {
			return nullptr;
		}
		if (fseek(fp, 0, SEEK_END) != 0) {
			fclose(fp);
			return nullptr;
		}
		long size = ftell(fp);
		if (size < 0) {
			fclose(fp);
			return nullptr;
		}
		if (static_cast<size_t>(size) > MAX_STATIC_FILE_SIZE) {
			fclose(fp);
			return http413_request_entity_too_large;
		}
		fseek(fp, 0, SEEK_SET);

		std::vector<char> body;
		if (size > 0) {
			body.resize(static_cast<size_t>(size));
			if (fread(body.data(), 1, body.size(), fp) != body.size()) {
				fclose(fp);
				return nullptr;
			}
		}
		fclose(fp);

		response->write("Content-Type: " + static_mime_type(relative) + "\r\n\r\n");
		if (!body.empty()) {
			response->write(body.data(), body.size());
		}
		return http200_ok;
	}

#pragma pack(push, 1)
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
#pragma pack(pop)
	static_assert(sizeof(FCGI_Header) == 8, "FCGI_Header must be 8 bytes");
	static_assert(sizeof(FCGI_BeginRequestRecord) == 16, "FCGI_BeginRequestRecord must be 16 bytes");
	static_assert(sizeof(FCGI_EndRequestBody) == 8, "FCGI_EndRequestBody must be 8 bytes");

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

	// Maximum total bytes accepted from the FastCGI upstream (response body + overhead).
	// Prevents unbounded memory growth from a malicious/buggy responder.
	static const size_t FCGI_MAX_RESPONSE_BYTES = 64 * 1024 * 1024; // 64MiB
	// Per-read timeout (seconds) for FastCGI upstream communication.
	static const int    FCGI_TIMEOUT_SEC = 30;
	// Maximum FastCGI record count to bound loop iterations.
	static const size_t FCGI_MAX_RECORDS = 100000;

	http_status_t const *invoke_fastcgi(http_request_t *request, http_response_t *response, HTTPIO *io)
	{
#ifdef _WIN32
		char const *cmd = "C:/develop/tinyfcgi/app/tinyfcgi.exe";
#else
		// char const *cmd = "./fcgiapp"; // process backend (fork+exec per request)
		char const *cmd = "unix:/tmp/foo.sock";
		// char const *cmd = "inet:localhost:3000";
#endif

		CwdGuard cwd_guard; // B4: restore cwd on any return path

		{
			int i = misc::last_index_of(cmd, '/');
			int j = misc::last_index_of(cmd, '\\');
			if (i < j) i = j;
			if (i > 0) {
				std::string dir(cmd, cmd + i);

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

			// D3: always (re)establish the FastCGI backend per request when it is a
			// process-type backend. Socket-type backends (unix:/inet:) are safe to reuse.
			bool is_process_backend = false;
			if (strncmp(cmd, "unix:", 5) == 0) {
				if (!proc || proc_expired) {
					proc = std::make_shared<FcgiUnixSocket>(cmd + 5);
					proc->launch("");
				}
			} else if (strncmp(cmd, "inet:", 5) == 0) {
				if (!proc || proc_expired) {
					proc = std::make_shared<FcgiInetSocket>(cmd + 5);
					proc->launch("");
				}
			} else {
				// Process backend: a fresh fork+exec is required each time because the
				// listening socket is consumed by accept() on first connect.
				is_process_backend = true;
				proc = std::make_shared<FcgiProcess>(pipepath);
				proc->launch(cmd);
			}
			if (!proc->connect()) {
				proc.reset();
				return http503_service_unavailable;
			}
			// Process backends must re-launch every request (listening socket consumed);
			// socket backends can be reused. Clear the flag only for socket backends.
			proc_expired = is_process_backend;

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

			// C5: FastCGI params must be split across multiple FCGI_PARAMS records
			// when the serialized payload exceeds the 65535-byte content length limit.
			auto write_fcgi_params = [&](std::vector<NameValue> const &params){
				// First, serialize all name/value pairs into one buffer (without header).
				std::vector<char> body;
				body.reserve(1024);
				auto append_length = [&body](size_t len){
					if (len < 128) {
						body.push_back(static_cast<char>(len));
					} else {
						body.push_back(static_cast<char>((len >> 24) | 0x80));
						body.push_back(static_cast<char>(len >> 16));
						body.push_back(static_cast<char>(len >> 8));
						body.push_back(static_cast<char>(len));
					}
				};
				auto append_name_value = [&](std::string const &name, std::string const &value){
					append_length(name.size());
					append_length(value.size());
					body.insert(body.end(), name.begin(), name.end());
					body.insert(body.end(), value.begin(), value.end());
				};
				for (NameValue const &param : params) {
					append_name_value(param.name(), param.value());
				}

				// Split into chunks no larger than 65535 bytes per record content.
				const size_t MAX_CONTENT = 65535;
				size_t pos = 0;
				if (body.empty()) {
					// Empty params record (terminator).
					std::vector<char> vec(sizeof(FCGI_Header));
					make_fcgi_header(&vec, FCGI_PARAMS);
					proc->write(&vec[0], vec.size());
					return;
				}
				while (pos < body.size()) {
					size_t chunk = std::min(body.size() - pos, MAX_CONTENT);
					std::vector<char> vec(sizeof(FCGI_Header) + chunk);
					memcpy(&vec[sizeof(FCGI_Header)], &body[pos], chunk);
					make_fcgi_header(&vec, FCGI_PARAMS);
					proc->write(&vec[0], vec.size());
					pos += chunk;
				}
			};

			write_fcgi_params(env);
			// Empty params record to signal end of params stream.
			{
				std::vector<NameValue> empty;
				write_fcgi_params(empty);
			}

			// Empty STDIN record to signal end of request body (no body in this sample).
			{
				std::vector<char> vec(sizeof(FCGI_Header));
				make_fcgi_header(&vec, FCGI_STDIN);
				proc->write(&vec[0], vec.size());
			}

			// A1: parse response with timeout, END_REQUEST requirement, and total size cap.
			bool got_end_request = false;
			bool upstream_error = false;
			{
				std::vector<char> tmp;
				tmp.reserve(65536 + 256);
				size_t pos = 0;
				size_t need = 0;
				FCGI_Header header = {};
				uint16_t contentlength = 0;
				size_t total_received = 0;
				size_t record_count = 0;

				while (1) {
					if (need == 0) {
						need = sizeof(FCGI_Header);
					}
					if (need > tmp.size()) {
						tmp.resize(need);
					}
					while (pos < need) {
						int n = proc->read(&tmp[pos], need - pos);
						if (n <= 0) {
							// read error or timeout
							upstream_error = true;
							break;
						}
						pos += n;
						total_received += n;
						if (total_received > FCGI_MAX_RESPONSE_BYTES) {
							upstream_error = true;
							break;
						}
					}
					if (upstream_error) break;

					if (pos == sizeof(FCGI_Header)) {
						memcpy(&header, tmp.data(), sizeof(FCGI_Header));
						contentlength = ((uint16_t)header.contentLengthB1 << 8) | header.contentLengthB0;
						if (header.type == FCGI_STDOUT && contentlength == 0) {
							// Empty STDOUT record: not necessarily end of stream; keep reading.
							pos = 0;
							need = 0;
							continue;
						}
						need = sizeof(FCGI_Header) + contentlength + header.paddingLength;
						if (need > FCGI_MAX_RESPONSE_BYTES) {
							upstream_error = true;
							break;
						}
						if (need > tmp.size()) {
							tmp.resize(need);
						}
					} else if (pos == need) {
						record_count++;
						if (record_count > FCGI_MAX_RECORDS) {
							upstream_error = true;
							break;
						}
						char const *p = tmp.data() + sizeof(FCGI_Header);
						if (header.type == FCGI_STDOUT) {
							io->write(p, contentlength);
						} else if (header.type == FCGI_STDERR) {
							std::string err(p, contentlength);
							printlog("fcgi stderr: " + err);
						} else if (header.type == FCGI_END_REQUEST) {
							if (pos >= sizeof(FCGI_Header) + sizeof(FCGI_EndRequestBody)) {
								FCGI_EndRequestBody const *h = (FCGI_EndRequestBody const *)&tmp[sizeof(FCGI_Header)];
								uint32_t appstat = (h->appStatusB3 << 24) | (h->appStatusB2 << 16) | (h->appStatusB1 << 8) | h->appStatusB0;
								uint32_t protstat = h->protocolStatus;
								(void)appstat;
								(void)protstat;
							}
							got_end_request = true;
							break;
						}
						pos = 0;
						need = 0;
						contentlength = 0;
						continue;
					}
				}
			}

			proc->disconnect();

			// If this was a process backend, drop the handle so the next request re-launches.
			if (proc_expired) {
				proc.reset();
			}

			if (upstream_error || !got_end_request) {
				return http502_bad_gateway;
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
		std::string normalized_location;
		if (!misc::normalize_path(location, &normalized_location)) {
			return http400_bad_request;
		}
		if (normalized_location == "/app/") {
			auto p = invoke_fastcgi(request, response, response);
			return p ? p : http502_bad_gateway;
		} else if (normalized_location == "/hello/") {
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

		http_status_t const *static_status = serve_static_file(normalized_location, request, response);
		if (static_status) {
			return static_status;
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

