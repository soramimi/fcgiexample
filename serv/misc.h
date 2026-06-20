
#ifndef __MISC_H
#define __MISC_H

#include <string>
#include <vector>
#include <optional>

namespace misc {

void split_words(char const *begin, char const *end, char c, std::vector<std::string> *out);
void split_words(const std::string &str, char c, std::vector<std::string> *out);
void split_words_by_space(const std::string &str, std::vector<std::string> *out);
int last_index_of(const char *ptr, unsigned short c);
std::optional<std::string> normalize_path(const std::string &path);

bool start_with(std::string_view str, std::string_view with);
bool end_with(std::string_view str, std::string_view with);

std::string_view trim_ascii(std::string_view value);
bool same_header_name(std::string const &left, std::string const &right);
bool iequals_ascii(std::string_view left, std::string_view right);

bool header_has_token(std::string const &value, std::string_view token);

int decode_base64_char(unsigned char ch);



}

class NameValue {
private:
	struct Data {
		std::string name;
		std::string value;
		Data(std::string const &name, std::string const &value)
			: name(name)
			, value(value)
		{
		}
	} data;
public:
	NameValue(std::string const &name = std::string(), std::string const &value= std::string())
		: data(name, value)
	{
	}
	std::string const &name() const
	{
		return data.name;
	}
	std::string const &value() const
	{
		return data.value;
	}
	void setValue(std::string const &value)
	{
		data.value = value;
	}
};


#endif
