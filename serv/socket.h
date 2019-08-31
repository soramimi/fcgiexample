
#ifndef __SOCKET_H
#define __SOCKET_H

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "wsock32.lib")
#pragma warning(disable:4996)
typedef SOCKET socket_t;
typedef int socklen_t;
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

#endif
