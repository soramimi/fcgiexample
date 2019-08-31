#ifndef FCGIPROCESS_H
#define FCGIPROCESS_H

#include <string>
#include <vector>

#include "misc.h"

#ifdef _WIN32
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

#ifdef _WIN32
class Connection {
public:
	HANDLE hPipe = INVALID_HANDLE_VALUE;
	~Connection()
	{
		close();
	}
	void close()
	{
		CloseHandle(hPipe);
	}
	bool create(std::string const &path)
	{
		hPipe = CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_READMODE_BYTE, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 100, nullptr);
		return hPipe != INVALID_HANDLE_VALUE;
	}
};
#else
class Connection {
public:
	int sock = -1;
	~Connection()
	{
		close();
	}
	void close()
	{
		::close(sock);
	}
	bool create(std::string const &pipepath)
	{
		sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, pipepath.c_str());
		unlink(addr.sun_path);
		sock = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock >= 0) {
			if (bind(sock, (sockaddr *)&addr, sizeof(addr)) == 0) {
				if (listen(sock, SOMAXCONN) == 0) {
					return true;
				}
			}
		}
		return false;
	}
};
#endif


class AbstractFcgi {
public:
	virtual void setEnvironment(std::vector<NameValue> *env) = 0;
	virtual void launch(const std::string &cmd) = 0;
	virtual bool connect() = 0;
	virtual void disconnect() = 0;
	virtual int write(char const *ptr, int len) = 0;
	virtual int read(char *ptr, int len) = 0;
};

class FcgiSocketIO : public AbstractFcgi {
public:
	enum Type {
		UNIX,
		INET,
	};
protected:
	struct Private;
	Private *m;
public:
	FcgiSocketIO(Type type, std::string const &name);
	void setEnvironment(std::vector<NameValue> *env)
	{
	}
	void launch(const std::string &cmd)
	{
	}
	bool connect();
	void disconnect();
	int write(const char *ptr, int len);
	int read(char *ptr, int len);
};

class FcgiUnixSocket : public FcgiSocketIO {
public:
	FcgiUnixSocket(std::string const &pipepath)
		: FcgiSocketIO(UNIX, pipepath)
	{
	}
};

class FcgiInetSocket : public FcgiSocketIO {
public:
	FcgiInetSocket(std::string const &host)
		: FcgiSocketIO(INET, host)
	{
	}
};

class FcgiProcess : public FcgiSocketIO {
	friend class StreamThread;
private:
	FcgiProcess(FcgiProcess const &);
	void operator = (FcgiProcess const &);
public:
	FcgiProcess(std::string const &pipepath);
	~FcgiProcess();
	void setEnvironment(std::vector<NameValue> *env);
	void launch(const std::string &cmd);
};

#endif // FCGIPROCESS_H
