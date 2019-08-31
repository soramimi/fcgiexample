#include "debug.h"

#ifdef _WIN32
#include <windows.h>
void startlog(char const *name)
{
}

void printlog(char const *text)
{
	OutputDebugStringA(text);
}
#else
#include <syslog.h>
void startlog(char const *name)
{
	openlog(name, 0, LOG_USER);
	syslog(LOG_DEBUG, "start");
}

void printlog(char const *text)
{
	syslog(LOG_DEBUG, "%s", text);
}
#endif

void printlog(std::string const &text)
{
	printlog(text.c_str());
}

