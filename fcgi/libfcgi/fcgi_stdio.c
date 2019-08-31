/*
 * fcgi_stdio.c --
 *
 *      FastCGI-stdio compatibility package
 *
 *
 * Copyright (c) 1996 Open Market, Inc.
 *
 * See the file "LICENSE.TERMS" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 */

#ifndef lint
static const char rcsid[] = "$Id: fcgi_stdio.c,v 1.14 2001/09/01 01:09:30 robs Exp $";
#endif /* not lint */

#include <errno.h>  /* for errno */
#include <stdarg.h> /* for va_arg */
#include <stdlib.h> /* for malloc */
#include <string.h> /* for strerror */

#include "fcgi_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef _WIN32
#define DLLAPI  __declspec(dllexport)
#endif

#include "fcgiapp.h"
#include "fcgios.h"
#include "fcgimisc.h"

#define NO_FCGI_DEFINES
#include "fcgi_stdio.h"
#undef NO_FCGI_DEFINES

#ifndef _WIN32

#ifdef HAVE_FILENO_PROTO
#include <stdio.h>
#else
extern int fileno(FILE *stream);
#endif

extern FILE *fdopen(int fildes, const char *type);
extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);

#else /* _WIN32 */

#define popen _popen
#define pclose _pclose

#endif /* _WIN32 */

char **fcgi_environ;

FCGI_FILE _fcgi_sF[3];




/*
 *----------------------------------------------------------------------
 *
 * FCGI_Accept --
 *
 *      Accepts a new request from the HTTP server and creates
 *      a conventional execution environment for the request.
 *
 *      If the application was invoked as a FastCGI server,
 *      the first call to FCGI_Accept indicates that the application
 *      has completed its initialization and is ready to accept
 *      a request.  Subsequent calls to FCGI_Accept indicate that
 *      the application has completed its processing of the
 *      current request and is ready to accept a new request.
 *
 *      If the application was invoked as a CGI program, the first
 *      call to FCGI_Accept is essentially a no-op and the second
 *      call returns EOF (-1).
 *
 * Results:
 *    0 for successful call, -1 for error (application should exit).
 *
 * Side effects:
 *      If the application was invoked as a FastCGI server,
 *      and this is not the first call to this procedure,
 *      FCGI_Accept first performs the equivalent of FCGI_Finish.
 *
 *      On every call, FCGI_Accept accepts the new request and
 *      reads the FCGI_PARAMS stream into an environment array,
 *      i.e. a NULL-terminated array of strings of the form
 *      ``name=value''.  It assigns a pointer to this array
 *      to the global variable environ, used by the standard
 *      library function getenv.  It creates new FCGI_FILE *s
 *      representing input from the HTTP server, output to the HTTP
 *      server, and error output to the HTTP server, and assigns these
 *      new files to stdin, stdout, and stderr respectively.
 *
 *      DO NOT mutate or retain pointers to environ or any values
 *      contained in it (e.g. to the result of calling getenv(3)),
 *      since these are freed by the next call to FCGI_Finish or
 *      FCGI_Accept.  In particular do not use setenv(3) or putenv(3)
 *      in conjunction with FCGI_Accept.
 *
 *----------------------------------------------------------------------
 */
static int acceptCalled = FALSE;
static int isCGI = FALSE;

int FCGI_Accept(void)
{
    if(!acceptCalled) {
        /*
         * First call to FCGI_Accept.  Is application running
         * as FastCGI or as CGI?
         */
        isCGI = FCGX_IsCGI();
        acceptCalled = TRUE;
        atexit(&FCGI_Finish);
    } else if(isCGI) {
        /*
         * Not first call to FCGI_Accept and running as CGI means
         * application is done.
         */
        return(EOF);
    }
    if(isCGI) {
        FCGI_stdin->stdio_stream = stdin;
        FCGI_stdin->fcgx_stream = NULL;
        FCGI_stdout->stdio_stream = stdout;
        FCGI_stdout->fcgx_stream = NULL;
        FCGI_stderr->stdio_stream = stderr;
        FCGI_stderr->fcgx_stream = NULL;
    } else {
        FCGX_Stream *in, *out, *error;
        FCGX_ParamArray envp;
        int acceptResult = FCGX_Accept(&in, &out, &error, &envp);
        if(acceptResult < 0) {
            return acceptResult;
        }
        FCGI_stdin->stdio_stream = NULL;
        FCGI_stdin->fcgx_stream = in;
        FCGI_stdout->stdio_stream = NULL;
        FCGI_stdout->fcgx_stream = out;
        FCGI_stderr->stdio_stream = NULL;
        FCGI_stderr->fcgx_stream = error;
		fcgi_environ = envp;
    }
    return 0;
}

void FCGI_Finish(void)
{
    if(!acceptCalled || isCGI) {
        return;
    }
    FCGX_Finish();
    FCGI_stdin->fcgx_stream = NULL;
    FCGI_stdout->fcgx_stream = NULL;
    FCGI_stderr->fcgx_stream = NULL;
	fcgi_environ = NULL;
}

int FCGI_fflush(FCGI_FILE *fp)
{
    if(fp == NULL)
    	return fflush(NULL);
    if(fp->stdio_stream)
        return fflush(fp->stdio_stream);
    else if(fp->fcgx_stream)
        return FCGX_FFlush(fp->fcgx_stream);
    return EOF;
}

size_t FCGI_fread(void *ptr, size_t size, size_t nmemb, FCGI_FILE *fp)
{
    int n;
    if(fp->stdio_stream)
        return fread(ptr, size, nmemb, fp->stdio_stream);
    else if(fp->fcgx_stream) {
        if((size * nmemb) == 0) {
            return 0;
        }
        n = FCGX_GetStr((char *) ptr, size * nmemb, fp->fcgx_stream);
        return (n/size);
    }
    return (size_t)EOF;
}

size_t FCGI_fwrite(void *ptr, size_t size, size_t nmemb, FCGI_FILE *fp)
{
    int n;
    if(fp->stdio_stream)
        return fwrite(ptr, size, nmemb, fp->stdio_stream);
    else if(fp->fcgx_stream) {
        if((size * nmemb) == 0) {
            return 0;
        }
        n = FCGX_PutStr((char *) ptr, size * nmemb, fp->fcgx_stream);
        return (n/size);
    }
    return (size_t)EOF;
}

