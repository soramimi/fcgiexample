#ifdef _WIN32
#include <windows.h>
#endif
#include "event.h"
#include "mutex.h"
#include "FcgiProcess.h"
#include "thread.h"
#include <deque>
#include <vector>

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
		strcpy(addr.sun_path, m->name.c_str());
		m->sock_io = socket(AF_UNIX, SOCK_STREAM, 0);
		int r = ::connect(m->sock_io, (sockaddr *)&addr, sizeof(addr));
		return r == 0;
	} else if (m->type == FcgiSocketIO::INET) {
		char const *hostp = m->name.c_str();
		char const *portp = strchr(hostp, ':');
		std::string host = portp ? std::string(hostp, portp - hostp) : std::string(hostp);
		int port = portp ? atoi(portp + 1) : 3000;
		sockaddr_in addr = {};
		{
			struct addrinfo hints = {};
			struct addrinfo *res = nullptr;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_family = AF_INET;
			getaddrinfo(host.c_str(), nullptr, &hints, &res);
			if (res) {
				if (res->ai_family == AF_INET) {
					addr = *reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
				}
				freeaddrinfo(res);
			}
		}
		addr.sin_port = htons(port);
		m->sock_io = socket(AF_INET, SOCK_STREAM, 0);
		int r = ::connect(m->sock_io, (sockaddr *)&addr, sizeof(addr));
		return r == 0;
	}
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
	return ::write(m->sock_io, ptr, len);
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
	listener_pipe.create(m->name.c_str());

	m->pid = 0;

	int pid = fork();
	if (pid < 0) {
		return;
	}

	if (pid == 0) { // child
		dup2(listener_pipe.sock, 0);
		listener_pipe.close();

		std::vector<std::string> args;
		std::vector<char *> argv;
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
			for (std::string const &s : args) {
				argv.push_back(const_cast<char *>(s.c_str()));
			}
			argv.push_back(nullptr);
		}
		if (execvp(argv[0], &argv[0]) < 0) {
		}
		return;
	}

	m->pid = pid;
}

#endif

