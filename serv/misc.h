
#ifndef __MISC_H
#define __MISC_H

#include <string>
#include <vector>

class misc {
private:
	misc()
	{
	}
public:
	static void split_words(char const *begin, char const *end, char c, std::vector<std::string> *out);
	static void split_words(const std::string &str, char c, std::vector<std::string> *out);
	static void split_words_by_space(const std::string &str, std::vector<std::string> *out);
	static int last_index_of(const char *ptr, unsigned short c);
};

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
