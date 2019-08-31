#ifndef DEBUG_H
#define DEBUG_H

#include <string>

void startlog(char const *name);
void printlog(char const *text);
void printlog(std::string const &text);

#endif // DEBUG_H

