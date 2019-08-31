#ifndef MYFCGI_H
#define MYFCGI_H

#define NO_FCGI_DEFINES
#include "fcgi_config.h"
#include "fcgi_stdio.h"

#ifdef __cplusplus
extern "C" {
#endif

void serv_inet_socket(int port);
void serv_unix_socket(const char *path);

#ifdef __cplusplus
}
#endif

#endif // MYFCGI_H
