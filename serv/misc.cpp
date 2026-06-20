
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

std::optional<std::string> misc::normalize_path(std::string const &path)
{
	std::string ret;
	if (path.empty()) return std::nullopt;

	std::vector<std::string> parts;
	char const *begin = path.c_str();
	char const *end = begin + path.size();
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
						return std::nullopt; // traversal below root
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

	if (path[0] == '/') {
		ret.push_back('/');
	}
	for (size_t i = 0; i < parts.size(); i++) {
		if (i > 0 || path[0] != '/') {
			ret.push_back('/');
		}
		ret.append(parts[i]);
	}
	if (ret.empty()) {
		ret.push_back('/');
	}
	// Preserve trailing slash so that "/app/" stays "/app/"
	if (path.size() > 1 && path[path.size() - 1] == '/' && !ret.empty() && (ret)[ret.size() - 1] != '/') {
		ret.push_back('/');
	}
	
	return ret;
}

bool misc::start_with(std::string_view str, std::string_view with)
{
	if (str.size() < with.size()) {
		return false;
	}
	return str.substr(0, with.size()) == with;
}

bool misc::end_with(std::string_view str, std::string_view with)
{
	if (str.size() < with.size()) {
		return false;
	}
	return str.substr(str.size() - with.size(), with.size()) == with;
}
