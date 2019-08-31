
#include "httpstatus.h"

#define HTTP_STATUS(CODE, KEY, TEXT) http_status_t http_##CODE##KEY[1] = { CODE, TEXT };
#include "httpstatus.txt"
#undef HTTP_STATUS

