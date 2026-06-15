
#include "misc.h"
#include <vector>
#ifdef _WIN32
#include <mbctype.h>
#endif
#include <functional>

int misc::last_index_of(char const *ptr, unsigned short c)
{
	int index = -1;
	int i = 0;
	while (ptr[i]) {
		int d = ptr[i] & 0xff;
		if (d == c) {
			index = i;
		}
#ifdef _WIN32
		if (_ismbblead(d)) {
			int e = ptr[i + 1] & 0xff;
			if (_ismbbtrail(e)) {
				if (((d << 8) | e) == c) {
					index = i;
				}
				i += 2;
			} else {
				i++;
			}
		}
		else
#endif
		{
			i++;
		}
	}
	return index;
}

static inline void split_(char const *begin, char const *end, std::function<bool(int c)> fn, std::vector<std::string> *out)
{
	out->clear();
	char const *ptr = begin;
	while (isspace(*ptr)) {
		ptr++;
	}
	char const *left = ptr;
	while (true) {
		int c = -1;
		if (ptr < end) {
			c = *ptr & 0xff;
		}
		if (fn(c) || c < 0) {
			char const *right = ptr;
			while (left < right && isspace(*left & 0xff)) left++;
			while (left < right && isspace(right[-1] & 0xff)) right--;
			if (left < right) {
				std::string line(left, right);
				out->push_back(line);
			}
			if (c < 0) return;
			ptr++;
			left = ptr;
		} else {
			ptr++;
		}
	}
}

void misc::split_words(char const *begin, char const *end, char sep, std::vector<std::string> *out)
{
	int s = sep & 0xff;
	split_(begin, end, [&](int c){ return c == s; }, out);
}

void misc::split_words(std::string const &str, char c, std::vector<std::string> *out)
{
	char const *begin = str.c_str();
	char const *end = begin + str.size();
	misc::split_words(begin, end, c, out);
}

void misc::split_words_by_space(std::string const &str, std::vector<std::string> *out)
{
	char const *begin = str.c_str();
	char const *end = begin + str.size();
	split_(begin, end, [&](char c){ return isspace(c); }, out);
}

bool misc::normalize_path(std::string const &input, std::string *out)
{
	out->clear();
	if (input.empty()) return false;

	std::vector<std::string> parts;
	char const *begin = input.c_str();
	char const *end = begin + input.size();
	char const *ptr = begin;
	char const *left = ptr;
	while (true) {
		int c = 0;
		if (ptr < end) {
			c = *ptr & 0xff;
		}
		if (c == '/' || c == 0) {
			if (left < ptr) {
				std::string part(left, ptr);
				if (part == "..") {
					if (parts.empty()) {
						return false; // traversal below root
					}
					parts.pop_back();
				} else if (part != ".") {
					parts.push_back(part);
				}
			}
			if (c == 0) break;
			ptr++;
			left = ptr;
		} else {
			ptr++;
		}
	}

	if (input[0] == '/') {
		out->push_back('/');
	}
	for (size_t i = 0; i < parts.size(); i++) {
		if (i > 0 || input[0] != '/') {
			out->push_back('/');
		}
		out->append(parts[i]);
	}
	if (out->empty()) {
		out->push_back('/');
	}
	// Preserve trailing slash so that "/app/" stays "/app/"
	if (input.size() > 1 && input[input.size() - 1] == '/' && !out->empty() && (*out)[out->size() - 1] != '/') {
		out->push_back('/');
	}
	return true;
}

