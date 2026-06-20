#include "uuid.h"
#include <random>
#include <chrono>


std::string UUIDv7::generate()
{
	struct D {
		std::random_device rd;
		std::mt19937_64 gen;
		std::uniform_int_distribution<uint64_t> dis;
		D()
			: gen(rd())
		{
		}
	};
	static D d;

	uint64_t time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	uint64_t rand = d.dis(d.gen);
	uint64_t ms = time / 1000;
	int us = time % 1000;

	char buf[37];
	snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012lx",
		(unsigned int)((ms >> 16) & 0xffffffff),
		(unsigned int)(ms & 0xffff),
		(unsigned int)(0x7000 | (us << 2) | ((rand >> 62) & 0x0003)),
		(unsigned int)(0x8000 | ((rand >> 48) & 0x3fff)),
		rand & 0xffffffffffffull);
	return std::string(buf);
}



