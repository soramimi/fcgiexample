
#include "FcgiProcess.h"
#include "base64.h"
#include "debug.h"
#include "httpserver.h"
#include "joinpath.h"
#include "misc.h"
#include "sha1.h"
#include "socket.h"
#include "strformat.h"
#include <algorithm>
#include <ctype.h>
#include <fcntl.h>
#include <list>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <mbctype.h>
#include <shlobj.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/unistd.h>
#define strnicmp(A, B, C) strncasecmp(A, B, C)
#define O_BINARY 0
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
	std::string const &saved() const { return saved_; }
};

namespace {

std::string_view trim_ascii(std::string_view value)
{
	while (!value.empty() && isspace(static_cast<unsigned char>(value.front()))) {
		value.remove_prefix(1);
	}
	while (!value.empty() && isspace(static_cast<unsigned char>(value.back()))) {
		value.remove_suffix(1);
	}
	return value;
}

bool iequals_ascii(std::string_view left, std::string_view right)
{
	if (left.size() != right.size()) {
		return false;
	}
	for (size_t i = 0; i < left.size(); i++) {
		if (tolower(static_cast<unsigned char>(left[i])) != tolower(static_cast<unsigned char>(right[i]))) {
			return false;
		}
	}
	return true;
}

bool is_http_token_char(unsigned char ch)
{
	if (isalnum(ch)) return true;
	switch (ch) {
	case '!':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '*':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~':
		return true;
	}
	return false;
}

bool is_valid_header_name(std::string_view name)
{
	if (name.empty()) {
		return false;
	}
	for (unsigned char ch : name) {
		if (!is_http_token_char(ch)) {
			return false;
		}
	}
	return true;
}

bool is_valid_header_value(std::string_view value)
{
	for (unsigned char ch : value) {
		if (ch == '\0' || ch == '\r' || ch == '\n') {
			return false;
		}
	}
	return true;
}

bool is_hop_by_hop_response_header(std::string_view name)
{
	return iequals_ascii(name, "Connection") || iequals_ascii(name, "Keep-Alive") || iequals_ascii(name, "Proxy-Authenticate") || iequals_ascii(name, "Proxy-Authorization") || iequals_ascii(name, "TE") || iequals_ascii(name, "Trailer") || iequals_ascii(name, "Transfer-Encoding") || iequals_ascii(name, "Upgrade") || iequals_ascii(name, "Content-Length");
}

http_status_t const *http_status_from_code(int code)
{
	switch (code) {
#define HTTP_STATUS(CODE, KEY, TEXT) \
	case CODE: return http##CODE##KEY;
#include "httpstatus.txt"
#undef HTTP_STATUS
	default:
		return nullptr;
	}
}

bool split_header_block(std::vector<char> const &data, size_t *header_end, std::vector<std::string> *header_lines)
{
	*header_end = 0;
	header_lines->clear();
	for (size_t i = 0; i + 1 < data.size(); i++) {
		size_t terminator_len = 0;
		if (i + 3 < data.size() && data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
			terminator_len = 4;
		} else if (data[i] == '\n' && data[i + 1] == '\n') {
			terminator_len = 2;
		}
		if (terminator_len == 0) {
			continue;
		}
		*header_end = i + terminator_len;
		size_t line_begin = 0;
		while (line_begin < i) {
			size_t line_end = line_begin;
			while (line_end < i && data[line_end] != '\n') {
				line_end++;
			}
			size_t content_end = line_end;
			if (content_end > line_begin && data[content_end - 1] == '\r') {
				content_end--;
			}
			header_lines->emplace_back(&data[line_begin], &data[content_end]);
			line_begin = line_end + 1;
		}
		return true;
	}
	return false;
}

bool sanitize_fcgi_response(std::vector<char> const &raw_stdout, http_response_t *response, http_status_t const **status_out)
{
	*status_out = http200_ok;
	response->content.clear();

	size_t header_end = 0;
	std::vector<std::string> header_lines;
	if (!split_header_block(raw_stdout, &header_end, &header_lines)) {
		return false;
	}

	bool seen_status = false;
	for (std::string const &line : header_lines) {
		size_t pos = line.find(':');
		if (pos == std::string::npos) {
			return false;
		}
		std::string_view name = trim_ascii(std::string_view(line.data(), pos));
		std::string_view value = trim_ascii(std::string_view(line.data() + pos + 1, line.size() - pos - 1));
		if (!is_valid_header_name(name) || !is_valid_header_value(value)) {
			return false;
		}
		if (iequals_ascii(name, "Status")) {
			if (seen_status) {
				return false;
			}
			seen_status = true;
			std::string value_str(value);
			char *endptr = nullptr;
			long code = strtol(value_str.c_str(), &endptr, 10);
			if (endptr == value_str.c_str() || code < 100 || code > 599) {
				return false;
			}
			http_status_t const *mapped = http_status_from_code(static_cast<int>(code));
			if (!mapped) {
				return false;
			}
			*status_out = mapped;
			continue;
		}
		if (is_hop_by_hop_response_header(name)) {
			continue;
		}
		response->write(std::string(name) + ": " + std::string(value) + "\r\n");
	}
	response->write("\r\n");
	if (header_end < raw_stdout.size()) {
		response->write(raw_stdout.data() + header_end, raw_stdout.size() - header_end);
	}
	return true;
}

struct RequestTarget {
	std::string path;
	std::string query;
};

RequestTarget parse_request_target(std::string const &uri)
{
	RequestTarget target;
	size_t end = uri.find_first_of("?#");
	target.path = end == std::string::npos ? uri : uri.substr(0, end);
	if (end != std::string::npos && uri[end] == '?') {
		size_t query_end = uri.find('#', end + 1);
		target.query = uri.substr(end + 1, query_end == std::string::npos ? std::string::npos : query_end - (end + 1));
	}
	return target;
}

bool make_http_env_name(std::string_view header_name, std::string *out)
{
	out->clear();
	if (header_name.empty()) {
		return false;
	}
	out->reserve(header_name.size() + 5);
	out->append("HTTP_");
	for (unsigned char ch : header_name) {
		if (ch == '-') {
			out->push_back('_');
		} else if (isalnum(ch)) {
			out->push_back(static_cast<char>(toupper(ch)));
		} else {
			return false;
		}
	}
	return true;
}

};

class AbstractHandler {
public:
	virtual ~AbstractHandler() { }
	virtual http_status_t const *operator()(http_response_t *response) const = 0;
};
class HelloHandler : public AbstractHandler {
public:
	http_status_t const *operator()(http_response_t *response) const
	{
		response->write("Content-Type: text/plain\r\n");
		response->write("Connection: close\r\n");
		response->write("\r\n");
		response->write("Hello, world\r\n");
		return http200_ok;
	}
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
		if (name.find('\r') != std::string::npos || name.find('\n') != std::string::npos || value.find('\r') != std::string::npos || value.find('\n') != std::string::npos) {
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
		RequestTarget target = parse_request_target(request->uri);
		std::string script_name;
		std::string path_info;
		if (target.path == "/app") {
			script_name = "/app";
		} else if (target.path.size() > 4 && target.path.compare(0, 4, "/app") == 0 && target.path[4] == '/') {
			script_name = "/app";
			path_info = target.path.substr(4);
		}

		out->clear();

		if (request->method == RequestMethod::GET) {
			setEnvironment(out, "REQUEST_METHOD", "GET");
		} else if (request->method == RequestMethod::POST) {
			setEnvironment(out, "REQUEST_METHOD", "POST");
		}

		setEnvironment(out, "SERVER_PROTOCOL", strformat("%s/%u.%u").s(request->protocol).u(request->protocol_version.maj).u(request->protocol_version.min).str());
		setEnvironment(out, "GATEWAY_INTERFACE", "CGI/1.1");
		setEnvironment(out, "REQUEST_SCHEME", request->scheme.empty() ? "http" : request->scheme);
		setEnvironment(out, "REQUEST_URI", request->uri);
		setEnvironment(out, "QUERY_STRING", target.query);
		setEnvironment(out, "SCRIPT_NAME", script_name);
		if (!path_info.empty()) {
			setEnvironment(out, "PATH_INFO", path_info);
		}
		setEnvironment(out, "SCRIPT_FILENAME", "./fcgiapp");
		{
			std::string content_type = request->header_value("Content-Type");
			if (!content_type.empty()) {
				setEnvironment(out, "CONTENT_TYPE", content_type);
			}
		}
		if (request->content_length > 0) {
			setEnvironment(out, "CONTENT_LENGTH", strformat("%u").u(request->content_length).str());
		}
		for (std::string const &line : request->header) {
			size_t pos = line.find(':');
			if (pos == std::string::npos) {
				continue;
			}
			std::string_view name = trim_ascii(std::string_view(line.data(), pos));
			std::string_view value = trim_ascii(std::string_view(line.data() + pos + 1, line.size() - pos - 1));
			if (iequals_ascii(name, "Content-Type") || iequals_ascii(name, "Content-Length")) {
				continue;
			}
			std::string env_name;
			if (!make_http_env_name(name, &env_name)) {
				continue;
			}
			setEnvironment(out, env_name, std::string(value));
		}
	}

	static size_t const MAX_STATIC_FILE_SIZE = 8 * 1024 * 1024; // 8MiB

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

	http_status_t const *serve_static_file(std::string const &path, http_request_t const *request, http_response_t *response)
	{
		// Document root for static file serving (fallback for unmatched paths).
		static std::string const root = "/home/soramimi/develop/fcgiexample/static";

		if (request->method != RequestMethod::GET) {
			return http405_method_not_allowed;
		}

		std::string relpath = path;
		if (relpath.empty() || relpath.back() == '/') {
			return nullptr; // directory listing is not supported -> 404
		}

		auto normalized_relpath = misc::normalize_path('/' + relpath);
		if (!normalized_relpath) {
			return nullptr;
		}
		// normalized_relpath always starts with '/' because the input did.
		std::string relative = normalized_relpath->substr(1);
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

		int fd = open(fullpath.c_str(), O_RDONLY | O_BINARY);
		if (fd != -1) {
			bool ok = false;
			struct stat st;
			if (fstat(fd, &st) == 0) {
				if (S_ISREG(st.st_mode)) {
					if (static_cast<size_t>(st.st_size) <= MAX_STATIC_FILE_SIZE) {
						char buffer[65536];
						response->write("Content-Type: " + static_mime_type(relative) + "\r\n\r\n");
						size_t remaining = st.st_size;
						while (remaining > 0) {
							size_t to_read = std::min<size_t>(remaining, sizeof(buffer));
							ssize_t n = read(fd, buffer, to_read);
							if (n < 1) {
								break;
							}
							response->write(buffer, n);
							remaining -= n;
						}
						ok = true;
					}
				}
			}
			close(fd);
			if (ok) {
				return http200_ok;
			}
		}
		return nullptr;
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
		FCGI_RESPONDER = 1,
		FCGI_AUTHORIZER = 2,
		FCGI_FILTER = 3,
	};

	enum {
		FCGI_BEGIN_REQUEST = 1,
		FCGI_ABORT_REQUEST = 2,
		FCGI_END_REQUEST = 3,
		FCGI_PARAMS = 4,
		FCGI_STDIN = 5,
		FCGI_STDOUT = 6,
		FCGI_STDERR = 7,
		FCGI_DATA = 8,
		FCGI_GET_VALUES = 9,
		FCGI_GET_VALUES_RESULT = 10,
	};

	// Maximum total bytes accepted from the FastCGI upstream (response body + overhead).
	// Prevents unbounded memory growth from a malicious/buggy responder.
	static size_t const FCGI_MAX_RESPONSE_BYTES = 64 * 1024 * 1024; // 64MiB
	// Per-read timeout (seconds) for FastCGI upstream communication.
	static int const FCGI_TIMEOUT_SEC = 30;
	// Maximum FastCGI record count to bound loop iterations.
	static size_t const FCGI_MAX_RECORDS = 100000;

	http_status_t const *invoke_fastcgi(http_request_t *request, http_response_t *response, HTTPIO *io)
	{
		(void)io;
#ifdef _WIN32
		char const *cmd = "C:/develop/tinyfcgi/app/tinyfcgi.exe";
#else
		char const *cmd = "./fcgiapp"; // process backend (fork+exec per request)
		// char const *cmd = "unix:/tmp/foo.sock";
		// char const *cmd = "inet:localhost:3000";
#endif

		CwdGuard cwd_guard;

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

		std::vector<NameValue> env;
		makeEnvironment(request, &env);

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
			is_process_backend = true;
			proc = std::make_shared<FcgiProcess>(pipepath);
			proc->launch(cmd);
		}
		if (!proc->connect()) {
			proc.reset();
			return http503_service_unavailable;
		}
		proc_expired = is_process_backend;

		auto finish_with = [&](http_status_t const *status) {
			proc->disconnect();
			if (proc_expired) {
				proc.reset();
			}
			return status;
		};

		uint16_t reqid = 1;
		auto make_fcgi_header = [&](std::vector<char> *vec, int type) {
			FCGI_Header *h = (FCGI_Header *)&vec->at(0);
			h->version = 1;
			h->type = type;
			h->requestIdB1 = reqid >> 8;
			h->requestIdB0 = reqid;
			auto setcontentlength = [](FCGI_Header *header, uint16_t len) {
				header->contentLengthB1 = len >> 8;
				header->contentLengthB0 = len;
			};
			setcontentlength(h, vec->size() - sizeof(FCGI_Header));
		};

		auto write_fcgi_record = [&](int type, char const *data, size_t len) {
			if (len > 65535) {
				return false;
			}
			std::vector<char> vec(sizeof(FCGI_Header) + len);
			if (len > 0) {
				memcpy(&vec[sizeof(FCGI_Header)], data, len);
			}
			make_fcgi_header(&vec, type);
			return proc->write(&vec[0], vec.size()) == static_cast<int>(vec.size());
		};

		{
			std::vector<char> vec(sizeof(FCGI_BeginRequestRecord));
			FCGI_BeginRequestRecord *beginreq = (FCGI_BeginRequestRecord *)&vec[0];
			beginreq->body.roleB1 = FCGI_RESPONDER >> 8;
			beginreq->body.roleB0 = FCGI_RESPONDER;
			beginreq->body.flags = 0;
			make_fcgi_header(&vec, FCGI_BEGIN_REQUEST);
			if (proc->write(&vec[0], vec.size()) != static_cast<int>(vec.size())) {
				return finish_with(http502_bad_gateway);
			}
		}

		auto write_fcgi_params = [&](std::vector<NameValue> const &params) {
			std::vector<char> body;
			body.reserve(1024);
			auto append_length = [&body](size_t len) {
				if (len < 128) {
					body.push_back(static_cast<char>(len));
				} else {
					body.push_back(static_cast<char>((len >> 24) | 0x80));
					body.push_back(static_cast<char>(len >> 16));
					body.push_back(static_cast<char>(len >> 8));
					body.push_back(static_cast<char>(len));
				}
			};
			auto append_name_value = [&](std::string const &name, std::string const &value) {
				append_length(name.size());
				append_length(value.size());
				body.insert(body.end(), name.begin(), name.end());
				body.insert(body.end(), value.begin(), value.end());
			};
			for (NameValue const &param : params) {
				append_name_value(param.name(), param.value());
			}

			size_t const max_content = 65535;
			size_t pos = 0;
			if (body.empty()) {
				return write_fcgi_record(FCGI_PARAMS, nullptr, 0);
			}
			while (pos < body.size()) {
				size_t chunk = std::min(body.size() - pos, max_content);
				if (!write_fcgi_record(FCGI_PARAMS, &body[pos], chunk)) {
					return false;
				}
				pos += chunk;
			}
			return true;
		};

		if (!write_fcgi_params(env)) {
			return finish_with(http502_bad_gateway);
		}
		if (!write_fcgi_record(FCGI_PARAMS, nullptr, 0)) {
			return finish_with(http502_bad_gateway);
		}

		if (request->content_length > 0) {
			size_t remaining = request->content_length;
			while (remaining > 0) {
				size_t chunk_len = std::min<size_t>(remaining, 65535);
				std::vector<char> chunk;
				request->read_content(&chunk, chunk_len);
				if (chunk.empty()) {
					return finish_with(http400_bad_request);
				}
				if (!write_fcgi_record(FCGI_STDIN, chunk.data(), chunk.size())) {
					return finish_with(http502_bad_gateway);
				}
				remaining -= chunk.size();
			}
		}
		if (!write_fcgi_record(FCGI_STDIN, nullptr, 0)) {
			return finish_with(http502_bad_gateway);
		}

		bool got_end_request = false;
		bool upstream_error = false;
		std::vector<char> raw_stdout;
		std::vector<char> tmp;
		tmp.reserve(65536 + 256);
		size_t pos = 0;
		size_t need = 0;
		FCGI_Header header = { };
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
					raw_stdout.insert(raw_stdout.end(), p, p + contentlength);
					if (raw_stdout.size() > FCGI_MAX_RESPONSE_BYTES) {
						upstream_error = true;
						break;
					}
				} else if (header.type == FCGI_STDERR) {
					std::string err(p, contentlength);
					printlog("fcgi stderr: " + err);
				} else if (header.type == FCGI_END_REQUEST) {
					got_end_request = true;
					break;
				}
				pos = 0;
				need = 0;
				contentlength = 0;
			}
		}

		if (upstream_error || !got_end_request) {
			return finish_with(http502_bad_gateway);
		}

		http_status_t const *upstream_status = http200_ok;
		if (!sanitize_fcgi_response(raw_stdout, response, &upstream_status)) {
			return finish_with(http502_bad_gateway);
		}
		return finish_with(upstream_status);
	}

private:
	std::map<std::string, std::function<http_status_t *(http_response_t *response)>> handlers_;

public:
	template <typename T> void emplace_handler(std::string const &path, std::function<http_status_t *(HTTPIO *response)> fn)
	{
		handlers_.emplace(path, fn);
	}

	virtual http_status_t const *do_get(HTTP_Server *server, std::string const &url, http_request_t *request, http_response_t *response, HTTPIO *io)
	{
		(void)server;
		(void)io;
		printlog("do_get url=" + url);
		std::string path = url;
		std::string question;
		std::string sharp;
		{
			char const *left = url.data();
			char const *right = left + url.size();
			char const *q = strchr(left, '?');
			char const *s = strchr(left, '#');
			if (q) {
				question = q + 1;
			}
			if (s) {
				sharp = s + 1;
			}
			if (q && s) {
				right = std::min(q, s);
			} else if (q) {
				right = q;
			} else if (s) {
				right = s;
			}
			if (right) {
				while (left < right && right[-1] == '/')
					right--;
				path.assign(left, right);
			}
		}

		{
			std::string_view pathview;
			auto normalized = misc::normalize_path(path);
			if (normalized) {
				pathview = *normalized;
				while (!pathview.empty() && misc::end_with(pathview, "/")) {
					pathview.remove_suffix(1);
				}
			}
			if (pathview.empty()) {
				path = "/index.html";
			} else {
				path = pathview;
			}
		}

		if (path == "/app" || (path.size() > 4 && path.compare(0, 4, "/app") == 0 && path[4] == '/')) {
			auto p = invoke_fastcgi(request, response, response);
			return p ? p : http502_bad_gateway;
		}

		printlog("do_get path=" + path);
		if (path == "/sock") {
			printlog("matched /sock");
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
				response->write("Connection: Upgrade\r\n");
				response->write("Sec-WebSocket-Accept: " + sec + "\r\n");
				response->write("\r\n");
				response->keepalive = ConnectionType::UpgradeWebSocket;
				return http101_switching_protocols;
			}
		}

		{ // dispatch to user defined handlers
			auto it = handlers_.find(path);
			if (it != handlers_.end()) {
				return it->second.operator()(response);
			}
		}

		{ // static file serving for unmatched paths
			http_status_t const *static_status = serve_static_file(path, request, response);
			if (static_status) {
				return static_status;
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
	chdir("/home/soramimi/develop/fcgiexample/_bin");
	startlog("tinyfcgiserver");
#endif

	MyHandler handler("tinyfcgiserver");
	handler.emplace_handler<HelloHandler>("/hello", [&](HTTPIO *io) {
		io->write("Content-Type: text/plain\r\n");
		io->write("Connection: close\r\n");
		io->write("\r\n");
		io->write("Hello, world\r\n");
		return http200_ok;
	});

	HTTP_Server server(&handler);

	// #ifdef _WIN32
	//	std::string wwwroot = "C:/develop/tinyfcgiserver/wwwroot";
	// #else
	//	std::string wwwroot = "/home/soramimi/develop/tinyfcgiserver/wwwroot/";
	// #endif
	server.setPort(5000);

	return server.run() ? 0 : 1;
}
