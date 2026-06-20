
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
