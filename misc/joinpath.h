
#ifndef __JOINPATH_H
#define __JOINPATH_H

#include <string>

std::string joinpath(char const *left, char const *right);
std::string joinpath(std::string const &left, std::string const &right);
std::u16string joinpath(std::u16string const &left, std::u16string const &right);

static inline std::string operator / (std::string const &left, std::string const &right)
{
	return joinpath(left, right);
}

#endif
