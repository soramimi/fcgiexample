#include "thread.h"

#ifdef _WIN32
unsigned int __stdcall Thread::run_(void *arg)
{
	Thread *me = (Thread *)arg;
	me->run();
	me->_running = false;
	return 0;
}
#else
void *Thread::run_(void *arg)
{
	Thread *me = (Thread *)arg;
	me->run();
	me->_running = false;
	return 0;
}
#endif

void Thread::start()
{
	_interrupted = false;
	_running = true;
#ifdef _WIN32
	_thread_handle = (HANDLE)_beginthreadex(0, 0, run_, this, CREATE_SUSPENDED, 0);
	ResumeThread(_thread_handle);
#else
	pthread_create(&_thread_handle, nullptr, run_, this);
#endif
}

void Thread::stop()
{
	_interrupted = true;
}

void Thread::join()
{
	if (_running) {
#ifdef _WIN32
		WaitForSingleObject(_thread_handle, INFINITE);
#else
		pthread_join(_thread_handle, 0);
#endif
	}
}

void Thread::terminate()
{
	if (_running) {
#ifdef _WIN32
		TerminateThread(_thread_handle, 0);
#else
		pthread_cancel(_thread_handle);
#endif
	}
}

void Thread::detach()
{
	if (_thread_handle != 0) {
#ifdef _WIN32
		CloseHandle(_thread_handle);
		_thread_handle = 0;
#else
		pthread_detach(_thread_handle);
		_thread_handle = 0;
#endif
	}
	_running = false;
}
