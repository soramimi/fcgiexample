
#include "myfcgi.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

// cgi mode:
// $ ./fcgiapp
//
// tcp/ip mode:
// $ ./fcgiapp -p 3000
//
// unix socket mode:
// $ ./fcgiapp -p /tmp/foo.sock

int main(int argc, char **argv)
{
	std::string path;

	int argi = 1;
	while (argi < argc) {
		char *arg = argv[argi];
		argi++;
		if (strcmp(arg, "-p") == 0) { // tcp port or socket path
			if (argi < argc) {
				path = argv[argi];
				argi++;
			}
		}
	}

#ifndef _WIN32
	{
		int port = atoi(path.c_str());
		if (port > 0 && port < 65536) {
			serv_inet_socket(port);
		} else if (path.c_str()[0] == '/') {
			serv_unix_socket(path.c_str());
		}
	}
#endif

	while (FCGI_Accept() >= 0) {

		auto Write = [](char const *ptr, size_t len) {
			FCGI_fwrite((void *)ptr, 1, len, FCGI_stdout);
		};

		{
			std::string s = "Content-Type: text/plain\r\n";
			Write(s.c_str(), s.size());
			Write("\r\n", 2);
		}

		{
			time_t x = time(0);
			struct tm *t = localtime(&x);
			char const *s = asctime(t);
			Write(s, strlen(s));
			Write("\r\n", 2);
		}
		{
			char const *content_length = getenv("CONTENT_LENGTH");
			long len = content_length ? strtol(content_length, nullptr, 10) : 0;
			if (len > 0) {
				std::vector<char> body(static_cast<size_t>(len));
				size_t total = 0;
				while (total < body.size()) {
					int n = FCGI_fread(body.data() + total, 1, body.size() - total, FCGI_stdin);
					if (n <= 0) {
						break;
					}
					total += static_cast<size_t>(n);
				}
				Write("Request-Body:\r\n", 15);
				if (total > 0) {
					Write(body.data(), total);
				}
				Write("\r\n\r\n", 4);
			}
		}
		Write("\r\n", 2);
		char **e = fcgi_environ ? fcgi_environ : environ;
		if (e) {
			while (*e) {
				std::string s = *e;
				s += "\r\n";
				Write(s.c_str(), s.size());
				e++;
			}
		}
	}

	return 0;
}
