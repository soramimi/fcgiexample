/* 
 * fcgi_stdio.h --
 *
 *      FastCGI-stdio compatibility package
 *
 *
 * Copyright (c) 1996 Open Market, Inc.
 *
 * See the file "LICENSE.TERMS" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * $Id: fcgi_stdio.h,v 1.5 2001/06/22 13:21:15 robs Exp $
 */

#ifndef _FCGI_STDIO
#define _FCGI_STDIO 1

#include <stdio.h>
#include <sys/types.h>
#include "fcgiapp.h"

#if defined (c_plusplus) || defined (__cplusplus)
extern "C" {
#endif

extern char **fcgi_environ;

#ifndef DLLAPI
#ifdef _WIN32
#define DLLAPI __declspec(dllimport)
#else
#define DLLAPI
#endif
#endif


/*
 * Wrapper type for FILE
 */

typedef struct {
    FILE *stdio_stream;
    FCGX_Stream *fcgx_stream;
} FCGI_FILE;

/*
 * The four new functions and two new macros
 */

DLLAPI int FCGI_Accept(void);
DLLAPI void FCGI_Finish(void);

/*
 * Wrapper stdin, stdout, and stderr variables, set up by FCGI_Accept()
 */

DLLAPI extern	FCGI_FILE	_fcgi_sF[];
#define FCGI_stdin	(&_fcgi_sF[0])
#define FCGI_stdout	(&_fcgi_sF[1])
#define FCGI_stderr	(&_fcgi_sF[2])

/*
 * Wrapper function prototypes, grouped according to sections
 * of Harbison & Steele, "C: A Reference Manual," fourth edition,
 * Prentice-Hall, 1995.
 */

DLLAPI int        FCGI_fflush(FCGI_FILE *fp);
DLLAPI size_t     FCGI_fread(void *ptr, size_t size, size_t nmemb, FCGI_FILE *fp);
DLLAPI size_t     FCGI_fwrite(void *ptr, size_t size, size_t nmemb, FCGI_FILE *fp);

#if defined (__cplusplus) || defined (c_plusplus)
} /* terminate extern "C" { */
#endif

#endif /* _FCGI_STDIO */

