#include "myfcgi.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef _WIN32
#include <signal.h>
#endif

typedef int SOCKET;
#define closesocket(S) close(S)

static const char *unix_socket_path = 0;

static void remove_unix_socket(void)
{
	if (unix_socket_path) {
		remove(unix_socket_path);
		unix_socket_path = 0;
	}
}

#ifndef _WIN32
static void unix_socket_signal_handler(int signo)
{
	(void)signo;
	remove_unix_socket();
	_exit(0);
}
#endif

void serv_inet_socket(int port)
{
	SOCKET sock;
	struct sockaddr_in sin;
	int ret;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		perror("socket");
		exit(1);
	}

	int one = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char const *)&one, sizeof(one)) == -1) {
		perror("setsockopt");
		exit(1);
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		perror("bind");
		exit(1);
	}

	ret = listen(sock, SOMAXCONN);
	if (ret == -1) {
		perror("listen");
		exit(1);
	}

	dup2(sock, STDIN_FILENO);
}

void serv_unix_socket(char const *path)
{
	SOCKET sock;
	struct sockaddr_un sa;
	int ret;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1) {
		perror("socket");
		exit(1);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	remove(sa.sun_path);

	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		exit(1);
	}

	ret = listen(sock, SOMAXCONN);
	if (ret == -1) {
		perror("listen");
		exit(1);
	}

	chmod(sa.sun_path, 0777);
	unix_socket_path = path;
	atexit(remove_unix_socket);
#ifndef _WIN32
	signal(SIGTERM, unix_socket_signal_handler);
	signal(SIGINT, unix_socket_signal_handler);
#endif
	dup2(sock, STDIN_FILENO);
}

