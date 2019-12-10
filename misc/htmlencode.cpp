
#include "htmlencode.h"
#include "charvec.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef WIN32
#pragma warning(disable:4996)
#endif

static void html_encode_(char const *ptr, char const *end, std::vector<char> *vec)
{
	while (ptr < end) {
		int c = *ptr & 0xff;
		ptr++;
		switch (c) {
		case '&':
			print(vec, "&amp;");
			break;
		case '<':
			print(vec, "&lt;");
			break;
		case '>':
			print(vec, "&gt;");
			break;
		case '\"':
			print(vec, "&quot;");
			break;
		case '\'':
			print(vec, "&apos;");
			break;
		case '\t':
		case '\n':
			print(vec, c);
			break;
		default:
			if (c < 0x20 || c == '\'') {
				char tmp[10];
				sprintf(tmp, "&#%u;", c);
				print(vec, tmp);
			} else {
				print(vec, c);
			}
		}
	}
}

static void html_decode_(char const *ptr, char const *end, std::vector<char> *vec)
{
	while (ptr < end) {
		int c = *ptr & 0xff;
		ptr++;
		if (c == '&') {
			char const *next = strchr(ptr, ';');
			if (!next) {
				break;
			}
			std::string t(ptr, next);
			if (t[0] == '#') {
				c = atoi(t.c_str() + 1);
				print(vec, c);
			} else if (t == "amp") {
				print(vec, '&');
			} else if (t == "lt") {
				print(vec, '<');
			} else if (t == "gt") {
				print(vec, '>');
			} else if (t == "quot") {
				print(vec, '\"');
			} else if (t == "apos") {
				print(vec, '\'');
			}
			ptr = next + 1;
		} else {
			print(vec, c);
		}
	}
}

std::string html_encode(char const *ptr, char const *end)
{
	std::vector<char> vec;
	vec.reserve((end - ptr) * 2);
	html_encode_(ptr, end, &vec);
	return to_stdstr(vec);
}

std::string html_decode(char const *ptr, char const *end)
{
	std::vector<char> vec;
	vec.reserve((end - ptr) * 2);
	html_decode_(ptr, end, &vec);
	return to_stdstr(vec);
}

std::string html_encode(char const *ptr, size_t len)
{
	return html_encode(ptr, ptr + len);
}

std::string html_decode(char const *ptr, size_t len)
{
	return html_decode(ptr, ptr + len);
}

std::string html_encode(char const *ptr)
{
	return html_encode(ptr, strlen(ptr));
}

std::string html_decode(char const *ptr)
{
	return html_decode(ptr, strlen(ptr));
}

std::string html_encode(std::string const &str)
{
	char const *begin = str.c_str();
	char const *end = begin + str.size();
	char const *ptr = begin;
	while (ptr < end) {
		int c = *ptr & 0xff;
		if (isspace(c) || strchr("&<>\"\'", c)) {
			break;
		}
		ptr++;
	}
	if (ptr == end) {
		return str;
	}
	std::vector<char> vec;
	vec.reserve(str.size() * 2);
	vec.insert(vec.end(), begin, ptr);
	html_encode_(ptr, end, &vec);
	begin = &vec[0];
	end = begin + vec.size();
	return std::string(begin, end);
}

std::string html_decode(std::string const &str)
{
	char const *begin = str.c_str();
	char const *end = begin + str.size();
	char const *ptr = begin;
	while (ptr < end) {
		int c = *ptr & 0xff;
		if (c == '&') {
			break;
		}
		ptr++;
	}
	if (ptr == end) {
		return str;
	}
	std::vector<char> vec;
	vec.reserve(str.size() * 2);
	vec.insert(vec.end(), begin, ptr);
	html_decode_(ptr, end, &vec);
	begin = &vec[0];
	end = begin + vec.size();
	return std::string(begin, end);
}

