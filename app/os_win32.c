
#define WIN32_LEAN_AND_MEAN 
#include <windows.h>
#include <winsock2.h>
//#include <stdlib.h>
//#include <assert.h>
#include <stdio.h>
//#include <sys/timeb.h>
//#include <process.h>


//#include "fcgimisc.h"
//#include "fcgios.h"
#define ASSERT(assertion)

#define WIN32_OPEN_MAX 128 /* XXX: Small hack */

/*
 * An enumeration of the file types
 * supported by the FD_TABLE structure.
 *
 * XXX: Not all currently supported.  This allows for future
 *      functionality.
 */
typedef enum {
	FD_UNUSED,
	FD_PIPE_SYNC,
} FILE_TYPE;

typedef union {
	HANDLE fileHandle;
	SOCKET sock;
	unsigned int value;
} DESCRIPTOR;

/*
 * Structure used to map file handle and socket handle
 * values into values that can be used to create unix-like
 * select bitmaps, read/write for both sockets/files.
 */
struct FD_TABLE {
	DESCRIPTOR fid;
	FILE_TYPE type;
	char *path;
	DWORD Errno;
	unsigned long instance;
	int status;
	int offset;			/* only valid for async file writes */
	DWORD *offsetHighPtr;	/* pointers to offset high and low words */
	DWORD *offsetLowPtr;	/* only valid for async file writes (logs) */
	HANDLE  hMapMutex;		/* mutex handle for multi-proc offset update */
	void * ovList;		/* List of associated OVERLAPPED_REQUESTs */
};

/* 
 * XXX Note there is no dyanmic sizing of this table, so if the
 * number of open file descriptors exceeds WIN32_OPEN_MAX the
 * app will blow up.
 */
static struct FD_TABLE fdTable[WIN32_OPEN_MAX];

static CRITICAL_SECTION  fdTableCritical;

//struct OVERLAPPED_REQUEST {
//	OVERLAPPED overlapped;
//	unsigned long instance;	/* file instance (won't match after a close) */
//	OS_AsyncProc procPtr;	/* callback routine */
//	ClientData clientData;	/* callback argument */
//	ClientData clientData1;	/* additional clientData */
//};
//typedef struct OVERLAPPED_REQUEST *POVERLAPPED_REQUEST;

//static const char *bindPathPrefix = "\\\\.\\pipe\\FastCGI\\";

static FILE_TYPE listenType = FD_UNUSED;

// XXX This should be a DESCRIPTOR
static HANDLE hListen = INVALID_HANDLE_VALUE;

static BOOLEAN libInitialized = FALSE;

/*
 *--------------------------------------------------------------
 *
 * Win32NewDescriptor --
 *
 *	Set up for I/O descriptor masquerading.
 *
 * Results:
 *	Returns "fake id" which masquerades as a UNIX-style "small
 *	non-negative integer" file/socket descriptor.
 *	Win32_* routine below will "do the right thing" based on the
 *	descriptor's actual type. -1 indicates failure.
 *
 * Side effects:
 *	Entry in fdTable is reserved to represent the socket/file.
 *
 *--------------------------------------------------------------
 */
static int Win32NewDescriptor(FILE_TYPE type, int fd, int desiredFd)
{
	int index = -1;

	EnterCriticalSection(&fdTableCritical);

	/*
	 * If desiredFd is set, try to get this entry (this is used for
	 * mapping stdio handles).  Otherwise try to get the fd entry.
	 * If this is not available, find a the first empty slot.  .
	 */
	if (desiredFd >= 0 && desiredFd < WIN32_OPEN_MAX) {
		if (fdTable[desiredFd].type == FD_UNUSED) {
			index = desiredFd;
		}
	} else if (fd > 0) {
		if (fd < WIN32_OPEN_MAX && fdTable[fd].type == FD_UNUSED) {
			index = fd;
		} else {
			int i;

			for (i = 1; i < WIN32_OPEN_MAX; ++i) {
				if (fdTable[i].type == FD_UNUSED) {
					index = i;
					break;
				}
			}
		}
	}

	if (index != -1) {
		fdTable[index].fid.value = fd;
		fdTable[index].type = type;
		fdTable[index].path = NULL;
		fdTable[index].Errno = NO_ERROR;
		fdTable[index].status = 0;
		fdTable[index].offset = -1;
		fdTable[index].offsetHighPtr = fdTable[index].offsetLowPtr = NULL;
		fdTable[index].hMapMutex = NULL;
		fdTable[index].ovList = NULL;
	}

	LeaveCriticalSection(&fdTableCritical);
	return index;
}

/*
 *--------------------------------------------------------------
 *
 * OS_LibInit --
 *
 *	Set up the OS library for use.
 *
 * Results:
 *	Returns 0 if success, -1 if not.
 *
 * Side effects:
 *	Sockets initialized, pseudo file descriptors setup, etc.
 *
 *--------------------------------------------------------------
 */
int OS_LibInit(int stdioFds[3])
{
	if (libInitialized)
		return 0;

	InitializeCriticalSection(&fdTableCritical);

	if ((GetStdHandle(STD_OUTPUT_HANDLE) == INVALID_HANDLE_VALUE) &&
			(GetStdHandle(STD_ERROR_HANDLE)  == INVALID_HANDLE_VALUE) &&
			(GetStdHandle(STD_INPUT_HANDLE)  != INVALID_HANDLE_VALUE) )
	{
		DWORD pipeMode = PIPE_READMODE_BYTE | PIPE_WAIT;
		HANDLE oldStdIn = GetStdHandle(STD_INPUT_HANDLE);

		if (!DuplicateHandle(GetCurrentProcess(), oldStdIn, GetCurrentProcess(), &hListen, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
			return -1;
		}

		if (!SetStdHandle(STD_INPUT_HANDLE, hListen)) {
			return -1;
		}

		CloseHandle(oldStdIn);

		SetNamedPipeHandleState(hListen, &pipeMode, NULL, NULL);
		listenType = FD_PIPE_SYNC;
	}

	libInitialized = 1;
	return 0;


	return 0;
}

/*
 *--------------------------------------------------------------
 *
 * Win32FreeDescriptor --
 *
 *	Free I/O descriptor entry in fdTable.
 *
 * Results:
 *	Frees I/O descriptor entry in fdTable.
 *
 * Side effects:
 *	None.
 *
 *--------------------------------------------------------------
 */
static void Win32FreeDescriptor(int fd)
{
	/* Catch it if fd is a bogus value */
	ASSERT((fd >= 0) && (fd < WIN32_OPEN_MAX));

	EnterCriticalSection(&fdTableCritical);

	if (fdTable[fd].type != FD_UNUSED) {

		ASSERT(fdTable[fd].path == NULL);

		fdTable[fd].type = FD_UNUSED;
		fdTable[fd].path = NULL;
		fdTable[fd].Errno = NO_ERROR;
		fdTable[fd].offsetHighPtr = fdTable[fd].offsetLowPtr = NULL;

		if (fdTable[fd].hMapMutex != NULL)
		{
			CloseHandle(fdTable[fd].hMapMutex);
			fdTable[fd].hMapMutex = NULL;
		}
	}

	LeaveCriticalSection(&fdTableCritical);

	return;
}

static short getPort(const char * bindPath)
{
	short port = 0;
	char * p = strchr(bindPath, ':');

	if (p && *++p) {
		char buf[6];

		strncpy(buf, p, 6);
		buf[5] = '\0';

		port = (short) atoi(buf);
	}

	return port;
}

/*
 *--------------------------------------------------------------
 *
 * OS_Read --
 *
 *	Pass through to the appropriate NT read function.
 *
 * Results:
 *	Returns number of byes read. Mimics unix read:.
 *		n bytes read, 0 or -1 failure: errno contains actual error
 *
 * Side effects:
 *	None.
 *
 *--------------------------------------------------------------
 */
int OS_Read(int fd, char * buf, size_t len)
{
	DWORD bytesRead;
	int ret = -1;

	ASSERT((fd >= 0) && (fd < WIN32_OPEN_MAX));

	if (ReadFile(hListen, buf, len, &bytesRead, NULL)) {
//	if (ReadFile(fdTable[fd].fid.fileHandle, buf, len, &bytesRead, NULL)) {
		ret = bytesRead;
	} else {
		fdTable[fd].Errno = GetLastError();
	}


	return ret;
}

/*
 *--------------------------------------------------------------
 *
 * OS_Write --
 *
 *	Perform a synchronous OS write.
 *
 * Results:
 *	Returns number of bytes written. Mimics unix write:
 *		n bytes written, 0 or -1 failure (??? couldn't find man page).
 *
 * Side effects:
 *	none.
 *
 *--------------------------------------------------------------
 */
int OS_Write(int fd, char * buf, size_t len)
{
	DWORD bytesWritten;
	int ret = -1;

	ASSERT(fd >= 0 && fd < WIN32_OPEN_MAX);

	if (WriteFile(hListen, buf, len, &bytesWritten, NULL)) {
//	if (WriteFile(fdTable[fd].fid.fileHandle, buf, len, &bytesWritten, NULL)) {
		ret = bytesWritten;
	} else {
		fdTable[fd].Errno = GetLastError();
	}

	return ret;
}

/*
 *--------------------------------------------------------------
 *
 * OS_Close --
 *
 *	Closes the descriptor with routine appropriate for
 *      descriptor's type.
 *
 * Results:
 *	Socket or file is closed. Return values mimic Unix close:
 *		0 success, -1 failure
 *
 * Side effects:
 *	Entry in fdTable is marked as free.
 *
 *--------------------------------------------------------------
 */
int OS_Close(int fd)
{
	int ret = 0;

	Win32FreeDescriptor(fd);
	return ret;
}


static void printLastError(const char * text)
{
	void *buf;

	FormatMessage(
				FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_FROM_SYSTEM |
				FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				GetLastError(),
				0,
				(LPTSTR) &buf,
				0,
				NULL
				);

	fprintf(stderr, "%s: %s\n", text, (LPCTSTR) buf);
	LocalFree(buf);
}

static int acceptNamedPipe()
{
	int ipcFd = -1;

	if (! ConnectNamedPipe(hListen, NULL))
	{
		switch (GetLastError())
		{
		case ERROR_PIPE_CONNECTED:

			// A client connected after CreateNamedPipe but
			// before ConnectNamedPipe. Its a good connection.

			break;

		case ERROR_IO_PENDING:

			// The NamedPipe was opened with an Overlapped structure
			// and there is a pending io operation.  mod_fastcgi
			// did this in 2.2.12 (fcgi_pm.c v1.52).

		case ERROR_PIPE_LISTENING:

			// The pipe handle is in nonblocking mode.

		case ERROR_NO_DATA:

			// The previous client closed its handle (and we failed
			// to call DisconnectNamedPipe)

		default:

			printLastError("unexpected ConnectNamedPipe() error");
		}
	}

	ipcFd = Win32NewDescriptor(FD_PIPE_SYNC, (int) hListen, -1);
	if (ipcFd == -1)
	{
		DisconnectNamedPipe(hListen);
	}

	return ipcFd;
}

/*
 *----------------------------------------------------------------------
 *
 * OS_Accept --
 *
 *  Accepts a new FastCGI connection.  This routine knows whether
 *  we're dealing with TCP based sockets or NT Named Pipes for IPC.
 *
 *  fail_on_intr is ignored in the Win lib.
 *
 * Results:
 *      -1 if the operation fails, otherwise this is a valid IPC fd.
 *
 *----------------------------------------------------------------------
 */
int OS_Accept(int listen_sock, int fail_on_intr, const char *webServerAddrs)
{
	int ipcFd = -1;

	listen_sock = 0;
	fail_on_intr = 0;

	ipcFd = acceptNamedPipe();

	return ipcFd;
}

/*
 *----------------------------------------------------------------------
 *
 * OS_IpcClose
 *
 *	OS IPC routine to close an IPC connection.
 *
 * Results:
 *
 *
 * Side effects:
 *      IPC connection is closed.
 *
 *----------------------------------------------------------------------
 */
int OS_IpcClose(int ipcFd)
{
	if (ipcFd == -1) return 0;

	/*
	 * Catch it if fd is a bogus value
	 */
	ASSERT((ipcFd >= 0) && (ipcFd < WIN32_OPEN_MAX));
	ASSERT(fdTable[ipcFd].type != FD_UNUSED);

	switch (listenType)
	{
	case FD_PIPE_SYNC:
		/*
		 * Make sure that the client (ie. a Web Server in this case) has
		 * read all data from the pipe before we disconnect.
		 */
		if (! FlushFileBuffers(fdTable[ipcFd].fid.fileHandle)) return -1;

		if (! DisconnectNamedPipe(fdTable[ipcFd].fid.fileHandle)) return -1;

		OS_Close(ipcFd);
		break;

	case FD_UNUSED:
	default:

		exit(106);
		break;
	}

	return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * OS_IsFcgi --
 *
 *	Determines whether this process is a FastCGI process or not.
 *
 * Results:
 *      Returns 1 if FastCGI, 0 if not.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int OS_IsFcgi(int sock)
{
	// Touch args to prevent warnings
	sock = 0;

	/* XXX This is broken for sock */

	return (listenType != FD_UNUSED);
}

