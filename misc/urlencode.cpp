
#include "urlencode.h"
#include "charvec.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#ifdef WIN32
#pragma warning(disable:4996)
#endif


static void url_encode(char const *ptr, char const *end, std::vector<char> *out, bool path_mode)
{
	while (ptr < end) {
		int c = (unsigned char)*ptr;
		ptr++;
		if (isalnum(c) || strchr("_.-~", c)) {
			print(out, c);
		} else if (c == ' ') {
			print(out, '+');
		} else if (path_mode && (c == ':' || c == '/')) {
			print(out, c);
		} else {
			char tmp[10];
			sprintf(tmp, "%%%02X", c);
			print(out, tmp[0]);
			print(out, tmp[1]);
			print(out, tmp[2]);
		}
	}
}

std::string url_encode(char const *str, char const *end, bool path_mode)
{
	if (!str) {
		return std::string();
	}

	std::vector<char> out;
	out.reserve(end - str + 10);

	url_encode(str, end, &out, path_mode);

	return to_stdstr(out);
}

std::string url_encode(char const *str, size_t len, bool path_mode)
{
	return url_encode(str, str + len, path_mode);
}

std::string url_encode(char const *str, bool path_mode)
{
	return url_encode(str, strlen(str), path_mode);
}

std::string url_encode(std::string const &str, bool path_mode)
{
	char const *begin = str.c_str();
	char const *end = begin + str.size();
	char const *ptr = begin;

	while (ptr < end) {
		int c = (unsigned char)*ptr;
		if (isalnum(c) || strchr("_.-~", c)) {
			// thru
		} else {
			break;
		}
		ptr++;
	}
	if (ptr == end) {
		return str;
	}

	std::vector<char> out;
	out.reserve(str.size() + 10);

	out.insert(out.end(), begin, ptr);
	url_encode(ptr, end, &out, path_mode);

	return to_stdstr(out);
}

static void url_decode(char const *ptr, char const *end, std::vector<char> *out)
{
	while (ptr < end) {
		int c = (unsigned char)*ptr;
		ptr++;
		if (c == '+') {
			c = ' ';
		} else if (c == '%' && isxdigit((unsigned char)ptr[0]) && isxdigit((unsigned char)ptr[1])) {
			char tmp[3]; // '%XX'
			tmp[0] = ptr[0];
			tmp[1] = ptr[1];
			tmp[2] = 0;
			c = strtol(tmp, NULL, 16);
			ptr += 2;
		}
		print(out, c);
	}
}

std::string url_decode(char const *str, char const *end)
{
	if (!str) {
		return std::string();
	}

	std::vector<char> out;
	out.reserve(end - str + 10);

	url_decode(str, end, &out);

	return to_stdstr(out);
}

std::string url_decode(char const *str, size_t len)
{
	return url_decode(str, str + len);
}

std::string url_decode(char const *str)
{
	return url_decode(str, strlen(str));
}

std::string url_decode(std::string const &str)
{
	char const *begin = str.c_str();
	char const *end = begin + str.size();
	char const *ptr = begin;

	while (ptr < end) {
		int c = *ptr & 0xff;
		if (c == '+' || c == '%') {
			break;
		}
		ptr++;
	}
	if (ptr == end) {
		return str;
	}


	std::vector<char> out;
	out.reserve(str.size() + 10);

	out.insert(out.end(), begin, ptr);
	url_decode(ptr, end, &out);

	return to_stdstr(out);
}

std::string url_encode(const char *str, const char *end)
{
	return url_encode(str, end, false);
}

std::string url_encode(const char *str, size_t len)
{
	return url_encode(str, len, false);
}

std::string url_encode(const char *str)
{
	return url_encode(str, false);
}

std::string url_encode(const std::string &str)
{
	return url_encode(str, false);
}

std::string path_encode(const std::string &str)
{
	return url_encode(str, true);
}
