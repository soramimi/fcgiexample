
#ifndef __SOCKET_H
#define __SOCKET_H

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "wsock32.lib")
#pragma warning(disable:4996)
typedef SOCKET socket_t;
typedef int socklen_t;
#define SHUT_RDWR SD_BOTH
#else
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int socket_t;
#define closesocket(s) close(s)
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Set receive/send timeouts (seconds) on a socket. Returns true on success.
// Used to mitigate Slowloris-style attacks and unresponsive upstreams.
inline bool set_socket_timeout(socket_t sock, int recv_sec, int send_sec)
{
#ifdef _WIN32
	DWORD r = static_cast<DWORD>(recv_sec) * 1000;
	DWORD s = static_cast<DWORD>(send_sec) * 1000;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char const *)&r, sizeof(r)) != 0) return false;
	if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char const *)&s, sizeof(s)) != 0) return false;
	return true;
#else
	struct timeval tv;
	tv.tv_sec = recv_sec;
	tv.tv_usec = 0;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char const *)&tv, sizeof(tv)) != 0) return false;
	tv.tv_sec = send_sec;
	if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char const *)&tv, sizeof(tv)) != 0) return false;
	return true;
#endif
}

#endif
