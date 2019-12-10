
#ifndef __HTMLENCODE_H
#define __HTMLENCODE_H

#include <string>

std::string html_encode(char const *ptr, char const *end);
std::string html_decode(char const *ptr, char const *end);

std::string html_encode(char const *ptr, size_t len);
std::string html_decode(char const *ptr, size_t len);

std::string html_encode(char const *ptr);
std::string html_decode(char const *ptr);

std::string html_encode(std::string const &str);
std::string html_decode(std::string const &str);

#endif
