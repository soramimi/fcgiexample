#ifdef _WIN32
#include <windows.h>
#endif
#include "FcgiProcess.h"
#include "debug.h"
#include "event.h"
#include "mutex.h"
#include "socket.h"
#include <deque>
#include <vector>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#endif


#define FAILED_(S) throw std::string(S)


#ifdef _WIN32
struct FcgiProcess::Private {
	std::vector<NameValue> envvec;
	bool hasenv = false;
	std::string pipepath;
	Connection connection;
	HANDLE hIO = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION pi;

	Private()
	{
		pi.hProcess = INVALID_HANDLE_VALUE;
		pi.hThread = INVALID_HANDLE_VALUE;
		pi.dwProcessId = 0;
		pi.dwThreadId = 0;
	}
};

FcgiProcess::FcgiProcess(const std::string &pipepath, const Connection &conn)
{
	pv = new Private();
	pv->pipepath = pipepath;
	pv->connection = conn;
	pv->hIO = INVALID_HANDLE_VALUE;
	pv->hasenv = nullptr;
}

FcgiProcess::~FcgiProcess()
{
	closeHandles();
	delete pv;
}

void FcgiProcess::setEnvironment(std::vector<NameValue> *env)
{
	if (env) {
		pv->envvec = *env;
		pv->hasenv = true;
	} else {
		pv->envvec.clear();
		pv->hasenv = false;
	}
}

void FcgiProcess::closeHandles()
{
	disconnect();

	CloseHandle(pv->pi.hThread);
	CloseHandle(pv->pi.hProcess);
	pv->pi.hProcess = INVALID_HANDLE_VALUE;
	pv->pi.hThread = INVALID_HANDLE_VALUE;
	pv->pi.dwProcessId = 0;
	pv->pi.dwThreadId = 0;
}

void FcgiProcess::launch(std::string const &cmd)
{
	closeHandles();

	HANDLE hInputRead = INVALID_HANDLE_VALUE;

	try {
		SECURITY_ATTRIBUTES sa;

		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.lpSecurityDescriptor = 0;
		sa.bInheritHandle = TRUE;

		HANDLE currproc = GetCurrentProcess();

		if (!DuplicateHandle(currproc, pv->connection.hPipe, currproc, &hInputRead, 0, TRUE, DUPLICATE_SAME_ACCESS))
			FAILED_("DupliateHandle");

		STARTUPINFOA si;

		ZeroMemory(&si, sizeof(STARTUPINFO));
		si.cb = sizeof(STARTUPINFO);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdInput = hInputRead;
		si.hStdOutput = INVALID_HANDLE_VALUE;
		si.hStdError = INVALID_HANDLE_VALUE;

		std::vector<char> tmp;
		tmp.resize(cmd.size() + 1);
		strcpy(&tmp[0], cmd.c_str());

		std::vector<char> envvec;
		char *envptr = nullptr;
		if (pv->hasenv) {
			for (NameValue const &nv : pv->envvec) {
				auto INSERT = [&](std::string const &str){
					char const *begin = str.c_str();
					char const *end = begin + str.size();
					envvec.insert(envvec.end(), begin, end);
				};
				INSERT(nv.name());
				envvec.push_back('=');
				INSERT(nv.value());
				envvec.push_back(0);
			}
			envvec.push_back(0);
			envptr = &envvec[0];
		}
		if (!CreateProcessA(0, &tmp[0], 0, 0, TRUE, CREATE_NEW_CONSOLE, envptr, 0, &si, &pv->pi))
			FAILED_("CreateProcess");

		CloseHandle(hInputRead);

	} catch (std::string const &e) { // 例外
		OutputDebugStringA(e.c_str());
	}
}

bool FcgiProcess::connect()
{
	disconnect();
	if (WaitNamedPipeA(pv->pipepath.c_str(), 500)) {
		pv->hIO = CreateFileA(pv->pipepath.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
		if (pv->hIO != INVALID_HANDLE_VALUE) {
			return true;
		}
	}
	return false;
}

void FcgiProcess::disconnect()
{
	if (pv->hIO != INVALID_HANDLE_VALUE) {
		DisconnectNamedPipe(pv->hIO);
		pv->hIO = INVALID_HANDLE_VALUE;
	}
}

int FcgiProcess::write(char const *ptr, int len)
{
	if (ptr && len > 0) {
		DWORD l = 0;
		if (WriteFile(pv->hIO, ptr, len, &l, 0)) {
			return l;
		}
	}
	return 0;
}

int FcgiProcess::read(char *ptr, int len)
{
	if (ptr && len > 0) {
		DWORD l = 0;
		if (ReadFile(pv->hIO, ptr, len, &l, 0)) {
			return l;
		}
	}
	return 0;
}
#else

#include "debug.h"
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace {
void install_sigchld_handler()
{
	static bool installed = false;
	if (!installed) {
		struct sigaction sa = {};
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGCHLD, &sa, nullptr);
		installed = true;
	}
}
int write_all_fd(int fd, char const *ptr, int len)
{
	if (fd < 0 || !ptr || len <= 0) {
		return 0;
	}
	int total = 0;
	while (total < len) {
		ssize_t n = ::write(fd, ptr + total, static_cast<size_t>(len - total));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return total;
		}
		if (n == 0) {
			break;
		}
		total += static_cast<int>(n);
	}
	return total;
}
}

struct FcgiSocketIO::Private {
	std::vector<NameValue> envvec;
	bool hasenv = false;
	FcgiSocketIO::Type type = FcgiSocketIO::UNIX;
	std::string name;
	int sock_io = -1;
	pid_t pid;

	Private()
	{
	}
};

FcgiSocketIO::FcgiSocketIO(Type type, const std::string &name)
	: m(new Private)
{
	m->type = type;
	m->name = name;
	m->hasenv = false;
}

bool FcgiSocketIO::connect()
{
	disconnect();
	if (m->type == FcgiSocketIO::UNIX) {
		sockaddr_un addr = {};
		addr.sun_family = AF_UNIX;
		if (m->name.size() >= sizeof(addr.sun_path)) {
			// path too long for sun_path
			return false;
		}
		strcpy(addr.sun_path, m->name.c_str());
		m->sock_io = socket(AF_UNIX, SOCK_STREAM, 0);
		if (m->sock_io < 0) {
			return false;
		}
		set_socket_timeout(m->sock_io, 30, 30);
		int r = ::connect(m->sock_io, (sockaddr *)&addr, sizeof(addr));
		if (r != 0) {
			int e = errno;
			closesocket(m->sock_io);
			m->sock_io = -1;
			return false;
		}
		return true;
	} else if (m->type == FcgiSocketIO::INET) {
		char const *hostp = m->name.c_str();
		char const *portp = strchr(hostp, ':');
		std::string host = portp ? std::string(hostp, portp - hostp) : std::string(hostp);
		int port = 3000;
		if (portp) {
			char *endptr = nullptr;
			long p = strtol(portp + 1, &endptr, 10);
			if (endptr == portp + 1 || *endptr != '\0' || p < 1 || p > 65535) {
				return false;
			}
			port = static_cast<int>(p);
		}
		sockaddr_in addr = {};
		{
			struct addrinfo hints = {};
			struct addrinfo *res = nullptr;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_family = AF_INET;
			int r = getaddrinfo(host.c_str(), nullptr, &hints, &res);
			if (r != 0 || !res) {
				return false;
			}
			if (res->ai_family != AF_INET) {
				freeaddrinfo(res);
				return false;
			}
			addr = *reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
			freeaddrinfo(res);
		}
		addr.sin_port = htons(port);
		m->sock_io = socket(AF_INET, SOCK_STREAM, 0);
		if (m->sock_io < 0) {
			return false;
		}
		set_socket_timeout(m->sock_io, 30, 30);
		int r = ::connect(m->sock_io, (sockaddr *)&addr, sizeof(addr));
		if (r != 0) {
			closesocket(m->sock_io);
			m->sock_io = -1;
			return false;
		}
		return true;
	}
	return false;
}

void FcgiSocketIO::disconnect()
{
	if (m->sock_io >= 0) {
		close(m->sock_io);
		m->sock_io = -1;
	}
}

int FcgiSocketIO::write(const char *ptr, int len)
{
	return write_all_fd(m->sock_io, ptr, len);
}

int FcgiSocketIO::read(char *ptr, int len)
{
	return ::read(m->sock_io, ptr, len);
}

//



FcgiProcess::FcgiProcess(const std::string &pipepath)
	: FcgiSocketIO(UNIX, pipepath)
{
}

FcgiProcess::~FcgiProcess()
{
	disconnect();
	if (m->pid > 0) {
		kill(m->pid, SIGTERM);
		waitpid(m->pid, nullptr, WNOHANG);
	}
	std::string pipepath = m->name;
	unlink(pipepath.c_str());
}

void FcgiProcess::setEnvironment(std::vector<NameValue> *env)
{
	if (env) {
		m->envvec = *env;
		m->hasenv = true;
	} else {
		m->envvec.clear();
		m->hasenv = false;
	}
}

void FcgiProcess::launch(std::string const &cmd)
{
	disconnect();

	Connection listener_pipe;
	if (!listener_pipe.create(m->name.c_str())) {
		// Failed to create listening socket; cannot launch backend.
		m->pid = 0;
		return;
	}

	install_sigchld_handler();

	m->pid = 0;

	int pid = fork();
	if (pid < 0) {
		return;
	}

	if (pid == 0) { // child
		if (listener_pipe.sock < 0) {
			_exit(127);
		}
		dup2(listener_pipe.sock, 0);
		listener_pipe.close();

		std::vector<std::string> args;
		std::vector<char const *> argv;
		{
			char const *begin = cmd.c_str();
			char const *end = begin + cmd.size();
			char const *ptr = begin;
			char const *left = ptr;
			while (1) {
				int c = 0;
				if (ptr < end) {
					c = *ptr & 0xff;
				}
				if (c == 0 || isspace(c)) {
					if (left < ptr) {
						std::string s(left, ptr);
						args.push_back(s);
					}
					if (c == 0) break;
					ptr++;
					left = ptr;
				} else {
					ptr++;
				}
			}
		}
		for (std::string const &s : args) {
			argv.push_back(s.c_str());
		}
		argv.push_back(nullptr);
		execvp(argv[0], const_cast<char *const *>(argv.data()));
		_exit(127);
	}

	m->pid = pid;
}

#endif

