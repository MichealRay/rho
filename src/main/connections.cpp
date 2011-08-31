/*CXXR $Id$
 *CXXR
 *CXXR This file is part of CXXR, a project to refactor the R interpreter
 *CXXR into C++.  It may consist in whole or in part of program code and
 *CXXR documentation taken from the R project itself, incorporated into
 *CXXR CXXR (and possibly MODIFIED) under the terms of the GNU General Public
 *CXXR Licence.
 *CXXR 
 *CXXR CXXR is Copyright (C) 2008-11 Andrew R. Runnalls, subject to such other
 *CXXR copyrights and copyright restrictions as may be stated below.
 *CXXR 
 *CXXR CXXR is not part of the R project, and bugs and other issues should
 *CXXR not be reported via r-bugs or other R project channels; instead refer
 *CXXR to the CXXR website.
 *CXXR */

/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 2000-11   The R Development Core Team.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

/* Notes on so-called 'Large File Support':

   The C stdio functions such as fseek and ftell are defined using
   'long' for file positioning: also fread/fwrite use size_t for size
   and number of items.  (The latter can cause problems with
   reading/writing large blocks, but not in R.)  POSIX introduced
   off_t and fseeko/ftello to allow larger file sizes, since 'long'
   may limit file positioning to 2GB.  (C99 introduced fpos_t and
   f[gs]etpos.)

   Note that the issue really only arises if 'long' is 32-bit, which
   is not the case on all known 64-bit platforms except Windows.
   However, off_t (defined in sys/types.h) is itself often 32-bit,
   which has led to workarounds.  On Linux systems, the macros

   __USE_FILE_OFFSET64
   __USE_LARGEFILE64

   select between __off_t and __off64_t.  Since these are different
   types, the functions using them have to be remapped, and the
   __off64_t versions call fopen64, fseeko64, ftello64 and so on.

   These macros are not intended to be used directly but via (features.h)

   _FILE_OFFSET_BITS=N  Select default filesystem interface.
   _LARGEFILE_SOURCE    Some more functions for correct standard I/O.
   _LARGEFILE64_SOURCE  Additional functionality from LFS for large files.

   The last makes system calls like open64 visible directly, and so
   should not be needed in R.

   This is commonly known as LFS; _but_ 'LFS Linux' is something else.
   See http://en.wikipedia.org/wiki/Large_file_support and
   http://www.suse.de/~aj/linux_lfs.html

   Solaris has a similar scheme: see 'man lf64', 'man lfcompile' and
   'man lfcompile64'.

   On Mac OS X, off_t is typedef-ed to __darwin_off_t, which is
   __int64_t, so the issue never arises.  Similarly on FreeBSD.

   The situation with Windows is similar, but off64_t, fseeko64 etc
   need to be selected explicitly (even on Win64).

   There are also issues with the glob(), readdir(), stat() system
   calls: see platform.c and sysutils.c

   saveload.c uses f[gs]etpos: they have 64-bit versions on LFS Linux
   and Solaris.  But this only used for pre-1.4.0 formats, and fpos_t
   is 64-bit on Windows.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <Defn.h>
#include <Fileio.h>
#include <Rconnections.h>
#include <R_ext/Complex.h>
#include <R_ext/R-ftp-http.h>
#include <R_ext/RS.h>		/* R_chk_calloc and Free */
#include <R_ext/Riconv.h>
#include <basedecl.h>
#include <cstdarg>

using namespace std;

#undef ERROR			/* for compilation on Windows */

int attribute_hidden R_OutputCon; /* used in printutils.c */

static void con_destroy(int i);

#include <errno.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
/* Solaris and AIX define open as open64 under some circumstances */
# undef open
/* AIX defines truncate as truncate64 under some circumstances */
# undef truncate
#endif

/* This works on Win64 where long is 4 bytes but long long is 8 bytes. */
#if defined __GNUC__ && __GNUC__ >= 2
__extension__ typedef long long int _lli_t;
#else
typedef long long int _lli_t;
#endif

/* Win32 does have popen, but it does not work in GUI applications,
   so test that later */
#ifdef Win32
# include <R_ext/RStartup.h>
  extern UImode  CharacterMode;
#endif

#define NCONNECTIONS 128 /* snow needs one per slave node */
#define NSINKS 21

static Rconnection Connections[NCONNECTIONS];
static SEXP OutTextData;

static int R_SinkNumber;
static int SinkCons[NSINKS], SinkConsClose[NSINKS], R_SinkSplit[NSINKS];

/* We need a unique id for a connection to ensure that the finalizer
   does not try to close it after it is already closed.  And that id
   will be passed as a pointer, so it seemed easiest to use void *.
*/
static void * current_id = NULL;

#include <zlib.h>
typedef struct gzconn {
    Rconnection con;
    int cp; /* compression level */
    z_stream s;
    int z_err, z_eof;
    uLong crc;
    Byte *inbuf, *outbuf;
    int nsaved;
    char saved[2];
    Rboolean allow;
} *Rgzconn;


/* ------------- admin functions (see also at end) ----------------- */

static int NextConnection(void)
{
    int i;
    for(i = 3; i < NCONNECTIONS; i++)
	if(!Connections[i]) break;
    if(i >= NCONNECTIONS) {
	R_gc(); /* Try to reclaim unused ones */
	for(i = 3; i < NCONNECTIONS; i++)
	    if(!Connections[i]) break;
	if(i >= NCONNECTIONS)
	    error(_("all connections are in use"));
    }
    return i;
}

static int ConnIndex(Rconnection con)
{
    int i;
    for(i = 0; i < NCONNECTIONS; i++)
	if(Connections[i] == con) break;
    if(i >= NCONNECTIONS)
	error(_("connection not found"));
    return i;
}

/* internal, not the same as R function getConnection */
attribute_hidden
Rconnection getConnection(int n)
{
    Rconnection con = NULL;

    if(n < 0 || n >= NCONNECTIONS || n == NA_INTEGER ||
       !(con = Connections[n]))
	error(_("invalid connection"));
    return con;

}

attribute_hidden
int getActiveSink(int n)
{
    if (n >= R_SinkNumber || n < 0)
	return 0;
    if (R_SinkSplit[R_SinkNumber - n])
	return SinkCons[R_SinkNumber - n - 1];
    else
	return 0;
}

static void conFinalizer(SEXP ptr)
{
    int i, ncon;
    void *cptr = R_ExternalPtrAddr(ptr);

    if(!cptr) return;

    for(i = 3; i < NCONNECTIONS; i++)
	if(Connections[i] && Connections[i]->id == cptr) {
	    ncon = i;
	    break;
	}
    if(i >= NCONNECTIONS) return;
    /* printf("closing unused connection %d (%s)\n", ncon,
       getConnection(ncon)->description); */
    warning(_("closing unused connection %d (%s)\n"), ncon,
	    getConnection(ncon)->description);

    con_destroy(ncon);
    R_ClearExternalPtr(ptr); /* not really needed */
}


/* for use in REvprintf */
attribute_hidden
Rconnection getConnection_no_err(int n)
{
    Rconnection con = NULL;

    if(n < 0 || n >= NCONNECTIONS || n == NA_INTEGER ||
       !(con = Connections[n]))
	return NULL;
    return con;

}

static void set_iconv_error(Rconnection con, CXXRCONST char* from, CXXRCONST char* to)
{
    char buf[100];
    snprintf(buf, 100, _("unsupported conversion from '%s' to '%s'"), from, to);
    con_destroy(ConnIndex(con));
    error(buf);
}

void set_iconv(Rconnection con)
{
    void *tmp;

    /* need to test if this is text, open for reading to writing or both,
       and set inconv and/or outconv */
    if(!con->text || !strlen(con->encname) ||
       strcmp(con->encname, "native.enc") == 0) {
	con->UTF8out = FALSE;
	return;
    }
    if(con->canread) {
	size_t onb = 50;
	char *ob = con->oconvbuff;
	/* UTF8out is set in readLines() and scan()
	   Was Windows-only until 2.12.0, but we now require iconv.
	 */
	Rboolean useUTF8 = CXXRCONSTRUCT(Rboolean, !utf8locale && con->UTF8out);
	tmp = Riconv_open(useUTF8 ? "UTF-8" : "", con->encname);
	if(tmp != reinterpret_cast<void *>(-1)) con->inconv = tmp;
	else set_iconv_error(con, con->encname, useUTF8 ? "UTF-8" : "");
	con->EOF_signalled = FALSE;
	/* initialize state, and prepare any initial bytes */
	Riconv(tmp, NULL, NULL, &ob, &onb);
	con->navail = 50-onb; con->inavail = 0;
	/* libiconv can handle BOM marks on Windows Unicode files, but
	   glibc's iconv cannot. Aargh ... */
	if(streql(con->encname, "UCS-2LE") || 
	   streql(con->encname, "UTF-16LE")) con->inavail = -2;
    }
    if(con->canwrite) {
	size_t onb = 25;
	char *ob = con->init_out;
	tmp = Riconv_open(con->encname, "");
	if(tmp != reinterpret_cast<void *>(-1)) con->outconv = tmp;
	else set_iconv_error(con, con->encname, "");
	/* initialize state, and prepare any initial bytes */
	Riconv(tmp, NULL, NULL, &ob, &onb);
	ob[25-onb] = '\0';
    }
}


/* ------------------- null connection functions --------------------- */

static Rboolean null_open(Rconnection con)
{
    error(_("%s not enabled for this connection"), "open");
    return FALSE;		/* -Wall */
}

static void null_close(Rconnection con)
{
    con->isopen = FALSE;
}

static void null_destroy(Rconnection con)
{
    if(con->connprivate) free(con->connprivate);
}

static int null_vfprintf(Rconnection con, const char *format, va_list ap)
{
    error(_("%s not enabled for this connection"), "printing");
    return 0;			/* -Wall */
}

/* va_copy is C99, but a draft standard had __va_copy.  Glibc has
   __va_copy declared uncondiitonally */


// 2007/06/19 arr: Gives -pedantic error.  FIXME : handle in configure script
#if defined(HAVE_VASPRINTF) && !HAVE_DECL_VASPRINTF
//int vasprintf(char **strp, const char *fmt, va_list ap);
#endif

// 2007/06/14 arr: C++98 ain't C99.  FIXME : handle in configure script
#ifdef __cplusplus
#undef HAVE_VA_COPY
#endif

#if !HAVE_VA_COPY && HAVE___VA_COPY
# define va_copy __va_copy
# undef HAVE_VA_COPY
# define HAVE_VA_COPY 1
#endif

#ifdef HAVE_VA_COPY
# define BUFSIZE 10000
#else
# define BUFSIZE 100000
#endif
int dummy_vfprintf(Rconnection con, const char *format, va_list ap)
{
    char buf[BUFSIZE], *b = buf;
    int res;
#ifdef HAVE_VA_COPY
    const void *vmax = vmaxget();
    int usedRalloc = FALSE, usedVasprintf = FALSE;
    va_list aq;

    va_copy(aq, ap);
    res = vsnprintf(buf, BUFSIZE, format, aq);
    va_end(aq);
#ifdef HAVE_VASPRINTF
    if(res >= BUFSIZE || res < 0) {
	res = vasprintf(&b, format, ap);
	if (res < 0) {
	    b = buf;
	    buf[BUFSIZE-1] = '\0';
	    warning(_("printing of extremely long output is truncated"));
	} else usedVasprintf = TRUE;
    }
#else
    if(res >= BUFSIZE) { /* res is the desired output length */
	usedRalloc = TRUE;
	/* apparently some implementations count short,
	   <http://unixpapa.com/incnote/stdio.html>
	   so add some margin here */
	b = R_alloc(res + 101, sizeof(char));
	vsnprintf(b, res+100, format, ap);
    } else if(res < 0) { /* just a failure indication */
	usedRalloc = TRUE;
	b = R_alloc(10*BUFSIZE, sizeof(char));
	res = vsnprintf(b, 10*BUFSIZE, format, ap);
	if (res < 0) {
	    b[10*BUFSIZE - 1] = '\0';
	    warning(_("printing of extremely long output is truncated"));
	    res = 10*BUFSIZE;
	}
    }
#endif /* HAVE_VASPRINTF */
#else  /* no VA_COPY */
    res = vsnprintf(buf, BUFSIZE, format, ap);
    if(res >= BUFSIZE || res < 0) {
	/* res is the desired output length or just a failure indication */
	    buf[BUFSIZE - 1] = '\0';
	    warning(_("printing of extremely long output is truncated"));
	    res = BUFSIZE;
    }
#endif /* HAVE_VA_COPY */
    if(con->outconv) { /* translate the buffer */
	char outbuf[BUFSIZE+1], *ob;
	const char *ib = b;
	size_t inb = res, onb, ires;
	Rboolean again = FALSE;
	int ninit = strlen(con->init_out);
	do {
	    onb = BUFSIZE; /* leave space for nul */
	    ob = outbuf;
	    if(ninit) {
		strcpy(ob, con->init_out);
		ob += ninit; onb -= ninit; ninit = 0;
	    }
	    ires = Riconv(con->outconv, &ib, &inb, &ob, &onb);
	    if(ires == size_t((-1)) && errno == E2BIG) again = TRUE;
	    if(ires == size_t((-1)) && errno != E2BIG)
		/* is this safe? */
		warning(_("invalid char string in output conversion"));
	    *ob = '\0';
	    con->write(outbuf, 1, strlen(outbuf), con);
	} while(again && inb > 0);  /* it seems some iconv signal -1 on
				       zero-length input */
    } else
	con->write(b, 1, res, con);
#ifdef HAVE_VA_COPY
    if(usedRalloc) vmaxset(vmax);
    if(usedVasprintf) free(b);
#endif
    return res;
}

int dummy_fgetc(Rconnection con)
{
    int c;
    Rboolean checkBOM = FALSE;

    if(con->inconv) {
	if(con->navail <= 0) {
	    unsigned int i, inew = 0;
	    char *p, *ob;
	    const char *ib;
	    size_t inb, onb, res;

	    if(con->EOF_signalled) return R_EOF;
	    if(con->inavail == -2) {
		con->inavail = 0;
		checkBOM = TRUE;
	    }
	    p = con->iconvbuff + con->inavail;
	    for(i = con->inavail; i < 25; i++) {
		c = con->fgetc_internal(con);
		if(c == R_EOF){ con->EOF_signalled = TRUE; break; }
		*p++ = c;
		con->inavail++;
		inew++;
	    }
	    if(inew == 0) return R_EOF;
	    if(checkBOM && con->inavail >= 2 &&
	       (int(con->iconvbuff[0]) & 0xff) == 255 &&
	       (int(con->iconvbuff[1]) & 0xff) == 254) {
		con->inavail -= 2;
		memmove(con->iconvbuff, con->iconvbuff+2, con->inavail);
	    }
	    ib = con->iconvbuff; inb = con->inavail;
	    ob = con->oconvbuff; onb = 50;
	    res = Riconv(con->inconv, &ib, &inb, &ob, &onb);
	    con->inavail = inb;
	    if(res == size_t(-1)) { /* an error condition */
		if(errno == EINVAL || errno == E2BIG) {
		    /* incomplete input char or no space in output buffer */
		    memmove(con->iconvbuff, ib, inb);
		} else {/*  EILSEQ invalid input */
		    warning(_("invalid input found on input connection '%s'"),
			    con->description);
		    con->inavail = 0;
		    con->EOF_signalled = TRUE;
		}
	    }
	    con->next = con->oconvbuff;
	    con->navail = 50 - onb;
	}
	con->navail--;
	return *con->next++;
    } else
	return con->fgetc_internal(con);
}

static int null_fgetc(Rconnection con)
{
    error(_("%s not enabled for this connection"), "'getc'");
    return 0;			/* -Wall */
}

static double null_seek(Rconnection con, double where, int origin, int rw)
{
    error(_("%s not enabled for this connection"), "'seek'");
    return 0.;			/* -Wall */
}

static void null_truncate(Rconnection con)
{
    error(_("%s not enabled for this connection"), "truncation");
}

static int null_fflush(Rconnection con)
{
    return 0;
}

static size_t null_read(void *ptr, size_t size, size_t nitems,
			Rconnection con)
{
    error(_("%s not enabled for this connection"), "'read'");
    return 0;			/* -Wall */
}

static size_t null_write(const void *ptr, size_t size, size_t nitems,
			 Rconnection con)
{
    error(_("%s not enabled for this connection"), "'write'");
    return 0;			/* -Wall */
}

void init_con(Rconnection newconn, const char *description, int enc,
	      const char * const mode)
{
    strcpy(newconn->description, description);
    newconn->enc = enc;
    strncpy(newconn->mode, mode, 4); newconn->mode[4] = '\0';
    newconn->isopen = newconn->incomplete = newconn->blocking = newconn->isGzcon = FALSE;
    newconn->canread = newconn->canwrite = TRUE; /* in principle */
    newconn->canseek = FALSE;
    newconn->text = TRUE;
    newconn->open = &null_open;
    newconn->close = &null_close;
    newconn->destroy = &null_destroy;
    newconn->vfprintf = &null_vfprintf;
    newconn->fgetc = newconn->fgetc_internal = &null_fgetc;
    newconn->seek = &null_seek;
    newconn->truncate = &null_truncate;
    newconn->fflush = &null_fflush;
    newconn->read = &null_read;
    newconn->write = &null_write;
    newconn->nPushBack = 0;
    newconn->save = newconn->save2 = -1000;
    newconn->connprivate = NULL;
    newconn->inconv = newconn->outconv = NULL;
    newconn->UTF8out = FALSE;
    /* increment id, avoid NULL */
    current_id = reinterpret_cast<void *>(size_t( current_id)+1);
    if(!current_id) current_id = reinterpret_cast<void *>( 1);
    newconn->id = current_id;
    newconn->ex_ptr = NULL;
}

/* ------------------- file connections --------------------- */

#if defined(HAVE_OFF_T) && defined(HAVE_FSEEKO)
#define f_seek fseeko
#define f_tell ftello
#else
#ifdef Win32
#define f_seek fseeko64
#define f_tell ftello64
#else
#define f_seek fseek
#define f_tell ftell
#endif
#endif

#ifdef Win32
size_t Rf_utf8towcs(wchar_t *wc, const char *s, size_t n);
#endif

typedef struct fileconn {
    FILE *fp;
#if defined(HAVE_OFF_T) && defined(HAVE_FSEEKO)
    off_t rpos, wpos;
#else
#ifdef Win32
    off64_t rpos, wpos;
#else
    long rpos, wpos;
#endif
#endif
    Rboolean last_was_write;
    Rboolean raw;
#ifdef Win32
    Rboolean anon_file;
    char name[PATH_MAX+1];
#endif
} *Rfileconn;

static Rboolean file_open(Rconnection con)
{
    const char *name;
    FILE *fp = NULL;
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
    Rboolean temp = FALSE;
#ifdef HAVE_FCNTL
    int fd, flags;
#endif
    int mlen = strlen(con->mode);

    if(strlen(con->description) == 0) {
	temp = TRUE;
	name = R_tmpnam("Rf", R_TempDir);
    } else name = R_ExpandFileName(con->description);
    errno = 0; /* some systems require this */
    if(strcmp(name, "stdin")) {
#ifdef Win32
	if(con->enc == CE_UTF8) {
	    int n = strlen(name);
	    wchar_t wname[2 * (n+1)], wmode[10];
	    R_CheckStack();
	    Rf_utf8towcs(wname, name, n+1);
	    mbstowcs(wmode, con->mode, 10);
	    fp = _wfopen(wname, wmode);
	} else
#endif
	    fp = R_fopen(name, con->mode);
    } else {  /* use file("stdin") to refer to the file and not the console */
#ifdef HAVE_FDOPEN
	fp = fdopen(0, con->mode);
#else
	warning(_("cannot open file '%s': %s"), name,
		"fdopen is not supported on this platform");
#endif
    }
    if(!fp) {
	warning(_("cannot open file '%s': %s"), name, strerror(errno));
	return FALSE;
    }
    if(temp) {
	/* This will fail on Windows, so arrange to remove in
	 * file_close.  An alternative strategy would be to manipulate
	 * the underlying file handle to add FILE_SHARE_DELETE (so the
	 * unlink is valid) or FILE_FLAG_DELETE_ON_CLOSE.  E.g. create
	 * via CreateFile, get an fd by _open_osfhandle and a file
	 * stream by fdopen.  See
	 * e.g. http://www.codeproject.com/KB/files/handles.aspx
	 */
	unlink(name);
#ifdef Win32
	strncpy(thisconn->name, name, PATH_MAX);
#endif
	free(const_cast<char *>( name)); /* only free if allocated by R_tmpnam */
    }
#ifdef Win32
    thisconn->anon_file = temp;
#endif
    thisconn->fp = fp;
    con->isopen = TRUE;
    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w' || con->mode[0] == 'a'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    if(mlen >= 2 && con->mode[1] == '+')
	con->canread = con->canwrite = TRUE;
    thisconn->last_was_write = CXXRCONSTRUCT(Rboolean, !con->canread);
    thisconn->rpos = 0;
    if(con->canwrite) thisconn->wpos = f_tell(fp);
    if(mlen >= 2 && con->mode[mlen-1] == 'b') con->text = FALSE;
    else con->text = TRUE;
    con->save = -1000;
    set_iconv(con);

#ifdef HAVE_FCNTL
    if(!con->blocking) {
	fd = fileno(fp);
	flags = fcntl(fd, F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
    }
#endif
    return TRUE;
}

static void file_close(Rconnection con)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
    if(con->isopen && strcmp(con->description, "stdin")) fclose(thisconn->fp);
    con->isopen = FALSE;
#ifdef Win32
    if(thisconn->anon_file) unlink(thisconn->name);
#endif
}

static int file_vfprintf(Rconnection con, const char *format, va_list ap)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);

    if(!thisconn->last_was_write) {
	thisconn->rpos = f_tell(thisconn->fp);
	thisconn->last_was_write = TRUE;
	f_seek(thisconn->fp, thisconn->wpos, SEEK_SET);
    }
    if(con->outconv) return dummy_vfprintf(con, format, ap);
    else return vfprintf(thisconn->fp, format, ap);
}

static int file_fgetc_internal(Rconnection con)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
    FILE *fp = thisconn->fp;
    int c;

    if(thisconn->last_was_write) {
	thisconn->wpos = f_tell(thisconn->fp);
	thisconn->last_was_write = FALSE;
	f_seek(thisconn->fp, thisconn->rpos, SEEK_SET);
    }
    c =fgetc(fp);
    return feof(fp) ? R_EOF : c;
}

static double file_seek(Rconnection con, double where, int origin, int rw)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
    FILE *fp = thisconn->fp;
#if defined(HAVE_OFF_T) && defined(HAVE_FSEEKO)
    off_t pos;
#else
#ifdef Win32
    off64_t pos;
#else
    long pos;
#endif
#endif
    int whence = SEEK_SET;

    /* make sure both positions are set */
    pos = f_tell(fp);
    if(thisconn->last_was_write) thisconn->wpos = pos; else thisconn->rpos = pos;
    if(rw == 1) {
	if(!con->canread) error(_("connection is not open for reading"));
	pos = thisconn->rpos;
	thisconn->last_was_write = FALSE;
    }
    if(rw == 2) {
	if(!con->canwrite) error(_("connection is not open for writing"));
	pos = thisconn->wpos;
	thisconn->last_was_write = TRUE;
    }
    if(ISNA(where)) return pos;

    switch(origin) {
    case 2: whence = SEEK_CUR; break;
    case 3: whence = SEEK_END;
#ifdef Win32
	    /* work around a bug in MinGW runtime 3.8 fseeko64, PR#7896 */
	    if(con->canwrite) fflush(fp);
#endif
	    break;
    default: whence = SEEK_SET;
    }
    f_seek(fp, CXXRCONSTRUCT(int, where), whence);
    if(thisconn->last_was_write) thisconn->wpos = f_tell(thisconn->fp);
    else thisconn->rpos = f_tell(thisconn->fp);
    return pos;
}

static void file_truncate(Rconnection con)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
#ifdef HAVE_FTRUNCATE
    FILE *fp = thisconn->fp;
    int fd = fileno(fp);
#ifdef HAVE_OFF_T
    off_t size = lseek(fd, 0, SEEK_CUR);
#else
#ifdef Win32
    __int64 size = lseek64(fd, 0, SEEK_CUR);
#else
    int size = lseek(fd, 0, SEEK_CUR);
#endif
#endif
#endif

    if(!con->isopen || !con->canwrite)
	error(_("can only truncate connections open for writing"));

    if(!thisconn->last_was_write) thisconn->rpos = f_tell(thisconn->fp);
#ifdef HAVE_FTRUNCATE
    if(ftruncate(fd, size))
	error(_("file truncation failed"));
#else
    error(_("file truncation unavailable on this platform"));
#endif
    thisconn->last_was_write = TRUE;
    thisconn->wpos = f_tell(thisconn->fp);
}

static int file_fflush(Rconnection con)
{
    FILE *fp = (static_cast<Rfileconn>((con->connprivate)))->fp;

    return fflush(fp);
}

static size_t file_read(void *ptr, size_t size, size_t nitems,
			Rconnection con)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
    FILE *fp = thisconn->fp;

    if(thisconn->last_was_write) {
	thisconn->wpos = f_tell(thisconn->fp);
	thisconn->last_was_write = FALSE;
	f_seek(thisconn->fp, thisconn->rpos, SEEK_SET);
    }
    return fread(ptr, size, nitems, fp);
}

static size_t file_write(const void *ptr, size_t size, size_t nitems,
			 Rconnection con)
{
    Rfileconn thisconn = CXXRCONSTRUCT(static_cast<fileconn*>, con->connprivate);
    FILE *fp = thisconn->fp;

    if(!thisconn->last_was_write) {
	thisconn->rpos = f_tell(thisconn->fp);
	thisconn->last_was_write = TRUE;
	f_seek(thisconn->fp, thisconn->wpos, SEEK_SET);
    }
    return fwrite(ptr, size, nitems, fp);
}

static Rconnection newfile(const char *description, int enc, const char *mode,
			   int raw)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of file connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("file") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of file connection failed"));
    }
    strcpy(newconn->connclass, "file");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of file connection failed"));
    }
    init_con(newconn, description, enc, mode);
    newconn->open = &file_open;
    newconn->close = &file_close;
    newconn->vfprintf = &file_vfprintf;
    newconn->fgetc_internal = &file_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->seek = &file_seek;
    newconn->truncate = &file_truncate;
    newconn->fflush = &file_fflush;
    newconn->read = &file_read;
    newconn->write = &file_write;
    newconn->canseek = CXXRCONSTRUCT(Rboolean, (raw == 0));
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct fileconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of file connection failed"));
    }
    static_cast<Rfileconn>(newconn->connprivate)->raw = CXXRCONSTRUCT(Rboolean, raw);
    return newconn;
}

/* file() is now implemented as an op of do_url */

/* ------------------- fifo connections --------------------- */

#if defined(HAVE_MKFIFO) && defined(HAVE_FCNTL_H)

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

# include <errno.h>

typedef struct fifoconn {
    int fd;
} *Rfifoconn;


static Rboolean fifo_open(Rconnection con)
{
    const char *name;
    Rfifoconn thisconn = CXXRCONSTRUCT(static_cast<fifoconn*>, con->connprivate);
    int fd, flags, res;
    int mlen = strlen(con->mode);
    struct stat sb;
    Rboolean temp = FALSE;

    if(strlen(con->description) == 0) {
	temp = TRUE;
	name = R_tmpnam("Rf", R_TempDir);
    } else name = R_ExpandFileName(con->description);
    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w' || con->mode[0] == 'a'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    if(mlen >= 2 && con->mode[1] == '+') con->canread = TRUE;

    /* if we are to write, create the fifo if needed */
    if(con->canwrite) {
	res = stat(name, &sb);
	if(res) { /* error, does not exist? */
	    errno = 0;
	    res = mkfifo(name, 00644);
	    if(res) {
		warning(_("cannot create fifo '%s', reason '%s'"), name,
			strerror(errno));
	    }
	    if(res) return FALSE;
	} else {
	    if(!(sb.st_mode & S_IFIFO)) {
		warning(_("'%s' exists but is not a fifo"), name);
		return FALSE;
	    }
	}
    }

    if(con->canread && con->canwrite) flags = O_RDWR;
    else if(con->canread) flags = O_RDONLY;
    else flags = O_WRONLY;
    if(!con->blocking) flags |= O_NONBLOCK;
    if(con->mode[0] == 'a') flags |= O_APPEND;
    fd = open(name, flags);
    if(fd < 0) {
	if(errno == ENXIO) warning(_("fifo '%s' is not ready"), name);
	else warning(_("cannot open fifo '%s'"), name);
	return FALSE;
    }
    if(temp) {
	unlink(name);
	free(const_cast<char *>( name)); /* allocated by R_tmpnam */
    }

    thisconn->fd = fd;
    con->isopen = TRUE;

    if(mlen >= 2 && con->mode[mlen-1] == 'b') con->text = FALSE;
    else con->text = TRUE;
    set_iconv(con);
    con->save = -1000;
    return TRUE;
}

static void fifo_close(Rconnection con)
{
    close((static_cast<Rfifoconn>((con->connprivate)))->fd);
    con->isopen = FALSE;
}

static int fifo_fgetc_internal(Rconnection con)
{
    Rfifoconn thisconn = CXXRSCAST(Rfifoconn, con->connprivate);
    unsigned char c;
    int n;

    n = read(thisconn->fd, CXXRNOCAST(char *)&c, 1);
    return (n == 1) ? c : R_EOF;
}

static size_t fifo_read(void *ptr, size_t size, size_t nitems,
			Rconnection con)
{
    Rfifoconn thisconn = CXXRSCAST(Rfifoconn, con->connprivate);

    /* uses 'size_t' for len */
    if (double( size) * double( nitems) > SSIZE_MAX)
	error(_("too large a block specified"));
    return read(thisconn->fd, ptr, size * nitems)/size;
}

static size_t fifo_write(const void *ptr, size_t size, size_t nitems,
			 Rconnection con)
{
    Rfifoconn thisconn = CXXRSCAST(Rfifoconn, con->connprivate);

    /* uses 'size_t' for len */
    if (double( size) * double( nitems) > SSIZE_MAX)
	error(_("too large a block specified"));
    return write(thisconn->fd, ptr, size * nitems)/size;
}


static Rconnection newfifo(const char *description, const char *mode)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of fifo connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("fifo") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of fifo connection failed"));
    }
    strcpy(newconn->connclass, "fifo");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of fifo connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);
    newconn->open = &fifo_open;
    newconn->close = &fifo_close;
    newconn->vfprintf = &dummy_vfprintf;
    newconn->fgetc_internal = &fifo_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->seek = &null_seek;
    newconn->truncate = &null_truncate;
    newconn->fflush = &null_fflush;
    newconn->read = &fifo_read;
    newconn->write = &fifo_write;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct fifoconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of fifo connection failed"));
    }
    return newconn;
}
#endif

SEXP attribute_hidden do_fifo(SEXP call, SEXP op, SEXP args, SEXP env)
{
#if defined(HAVE_MKFIFO) && defined(HAVE_FCNTL_H)
    SEXP sfile, sopen, ans, connclass, enc;
    const char *file, *open;
    int ncon, block;
    Rconnection con = NULL;

    checkArity(op, args);
    sfile = CAR(args);
    if(!isString(sfile) || length(sfile) < 1)
	error(_("invalid '%s' argument"), "description");
    if(length(sfile) > 1)
	warning(_("only first element of 'description' argument used"));
    file = translateChar(STRING_ELT(sfile, 0)); /* for now, like fopen */
    sopen = CADR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    block = asLogical(CADDR(args));
    if(block == NA_LOGICAL)
	error(_("invalid '%s' argument"), "block");
    enc = CADDDR(args);
    if(!isString(enc) || length(enc) != 1 ||
       strlen(CHAR(STRING_ELT(enc, 0))) > 100) /* ASCII */
	error(_("invalid '%s' argument"), "encoding");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    if(strlen(file) == 0) {
	if(!strlen(open)) open ="w+";
	if(strcmp(open, "w+") != 0 && strcmp(open, "w+b") != 0) {
	    open ="w+";
	    warning(_("fifo(\"\") only supports open = \"w+\" and open = \"w+b\": using the former"));
	}
    }
    ncon = NextConnection();
    con = Connections[ncon] = newfifo(file, strlen(open) ? open : CXXRCONSTRUCT(const_cast<char*>, "r"));
    con->blocking = CXXRCONSTRUCT(Rboolean, block);
    strncpy(con->encname, CHAR(STRING_ELT(enc, 0)), 100); /* ASCII */

    /* open it if desired */
    if(strlen(open)) {
	Rboolean success = con->open(con);
	if(!success) {
	    con_destroy(ncon);
	    error(_("cannot open the connection"));
	}
    }

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("fifo"));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);

    return ans;
#else
    error(_("fifo connections are not available on this system"));
    return R_NilValue;		/* -Wall */
#endif
}

/* ------------------- pipe connections --------------------- */

static Rboolean pipe_open(Rconnection con)
{
    FILE *fp;
    char mode[3];

#ifdef Win32
    strncpy(mode, con->mode, 2);
    mode[2] = '\0';
#else
    mode[0] = con->mode[0];
    mode[1] = '\0';
#endif
    errno = 0;
#ifdef Win32
    if(con->enc == CE_UTF8) {
	int n = strlen(con->description);
	wchar_t wname[2 * (n+1)], wmode[10];
	R_CheckStack();
	Rf_utf8towcs(wname, con->description, n+1);
	mbstowcs(wmode, con->mode, 10);
	fp = _wpopen(wname, wmode);
    } else
#endif
	fp = R_popen(con->description, mode);
    if(!fp) {
	warning(_("cannot open pipe() cmd '%s': %s"), con->description,
			strerror(errno));
	return FALSE;
    }
    (static_cast<Rfileconn>((con->connprivate)))->fp = fp;
    con->isopen = TRUE;
    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    if(strlen(con->mode) >= 2 && con->mode[1] == 'b') con->text = FALSE;
    else con->text = TRUE;
    set_iconv(con);
    con->save = -1000;
    return TRUE;
}

static void pipe_close(Rconnection con)
{
    pclose((static_cast<Rfileconn>((con->connprivate)))->fp);
    con->isopen = FALSE;
}

static Rconnection
newpipe(const char *description, int ienc, const char *mode)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of pipe connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("pipe") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of pipe connection failed"));
    }
    strcpy(newconn->connclass, "pipe");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of pipe connection failed"));
    }
    init_con(newconn, description, ienc, mode);
    newconn->open = &pipe_open;
    newconn->close = &pipe_close;
    newconn->vfprintf = &file_vfprintf;
    newconn->fgetc_internal = &file_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->fflush = &file_fflush;
    newconn->read = &file_read;
    newconn->write = &file_write;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct fileconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of pipe connection failed"));
    }
    return newconn;
}

#ifdef Win32
extern Rconnection
newWpipe(const char *description, int enc, const char *mode);
#endif

SEXP attribute_hidden do_pipe(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP scmd, sopen, ans, connclass, enc;
    const char *file, *open;
    int ncon;
    cetype_t ienc = CE_NATIVE;
    Rconnection con = NULL;

    checkArity(op, args);
    scmd = CAR(args);
    if(!isString(scmd) || length(scmd) < 1)
	error(_("invalid '%s' argument"), "description");
    if(length(scmd) > 1)
	warning(_("only first element of 'description' argument used"));
#ifdef Win32
    if( !strIsASCII(CHAR(STRING_ELT(scmd, 0))) ) {
	ienc = CE_UTF8;
	file = translateCharUTF8(STRING_ELT(scmd, 0));
    } else {
	ienc = CE_NATIVE;
	file = translateChar(STRING_ELT(scmd, 0));
    }
#else
    file = translateChar(STRING_ELT(scmd, 0));
#endif
    sopen = CADR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    enc = CADDR(args);
    if(!isString(enc) || length(enc) != 1 ||
       strlen(CHAR(STRING_ELT(enc, 0))) > 100) /* ASCII */
	error(_("invalid '%s' argument"), "encoding");

    ncon = NextConnection();
#ifdef Win32
    if(CharacterMode != RTerm)
	con = newWpipe(file, ienc, strlen(open) ? open : "r");
    else
#endif
	con = newpipe(file, ienc, strlen(open) ? open : "r");
    Connections[ncon] = con;
    strncpy(con->encname, CHAR(STRING_ELT(enc, 0)), 100); /* ASCII */

    /* open it if desired */
    if(strlen(open)) {
	Rboolean success = con->open(con);
	if(!success) {
	    con_destroy(ncon);
	    error(_("cannot open the connection"));
	}
    }

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("pipe"));
#ifdef Win32
    if(CharacterMode != RTerm)
	SET_STRING_ELT(connclass, 0, mkChar("pipeWin32"));
#endif
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);

    return ans;
}

/* ------------------- [bgx]zipped file connections --------------------- */

typedef struct gzfileconn {
    void *fp;
    int compress;
} *Rgzfileconn;

static Rboolean gzfile_open(Rconnection con)
{
    gzFile fp;
    char mode[6];
    Rgzfileconn gzcon = CXXRSCAST(Rgzfileconn, con->connprivate);

    strcpy(mode, con->mode);
    /* Must open as binary */
    if(strchr(con->mode, 'w')) sprintf(mode, "wb%1d", gzcon->compress);
    else if (con->mode[0] == 'a') sprintf(mode, "ab%1d", gzcon->compress);
    else strcpy(mode, "rb");
    fp = gzopen(R_ExpandFileName(con->description), mode);
    if(!fp) {
	warning(_("cannot open compressed file '%s', probable reason '%s'"),
		R_ExpandFileName(con->description), strerror(errno));
	return FALSE;
    }
    (static_cast<Rgzfileconn>((con->connprivate)))->fp = fp;
    con->isopen = TRUE;
    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w' || con->mode[0] == 'a'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    con->text = strchr(con->mode, 'b') ? FALSE : TRUE;
    set_iconv(con);
    con->save = -1000;
    return TRUE;
}

static void gzfile_close(Rconnection con)
{
    gzclose((static_cast<Rgzfileconn>((con->connprivate)))->fp);
    con->isopen = FALSE;
}

static int gzfile_fgetc_internal(Rconnection con)
{
    gzFile fp = (static_cast<Rgzfileconn>((con->connprivate)))->fp;
    int c;

    /* Looks like eof is signalled one char early
     -- sometimes! gzgetc may still return EOF 
    if(gzeof(fp)) return R_EOF;
    
    Removed for zlib 1.2.4
    */
    c = gzgetc(fp);
    return (c == EOF) ? R_EOF : c;
}

/* This can only seek forwards when writing (when it writes nul bytes).
   When reading, it either seeks forwards of rewinds and reads again */
static double gzfile_seek(Rconnection con, double where, int origin, int rw)
{
    gzFile  fp = (static_cast<Rgzfileconn>((con->connprivate)))->fp;
    z_off_t pos = gztell(fp);
    int res, whence = SEEK_SET;

    if (ISNA(where)) return double( pos);

    switch(origin) {
    case 2: whence = SEEK_CUR; break;
    case 3: error(_("whence = \"end\" is not implemented for gzfile connections"));
    default: whence = SEEK_SET;
    }
    res = gzseek(fp, z_off_t( where), whence);
    if(res == -1)
	warning(_("seek on a gzfile connection returned an internal error"));
    return double( pos);
}

static int gzfile_fflush(Rconnection con)
{
    /* Degrades compression too much, as Rvprintf calls fflush.

       gzFile fp = ((Rgzfileconn)(con->connprivate))->fp;

       return gzflush(fp, Z_SYNC_FLUSH); */

    return 0;
}

static size_t gzfile_read(void *ptr, size_t size, size_t nitems,
			Rconnection con)
{
    gzFile fp = (static_cast<Rgzfileconn>((con->connprivate)))->fp;
    /* uses 'unsigned' for len */
    if (double( size) * double( nitems) > UINT_MAX)
	error(_("too large a block specified"));
    return gzread(fp, ptr, size*nitems)/size;
}

static size_t gzfile_write(const void *ptr, size_t size, size_t nitems,
			   Rconnection con)
{
    gzFile fp = (static_cast<Rgzfileconn>((con->connprivate)))->fp;
    /* uses 'unsigned' for len */
    if (double( size) * double( nitems) > UINT_MAX)
	error(_("too large a block specified"));
    return gzwrite(fp, CXXRNOCAST(voidp)ptr, size*nitems)/size;
}

static Rconnection newgzfile(const char *description, const char *mode,
			     int compress)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of gzfile connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("gzfile") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of gzfile connection failed"));
    }
    strcpy(newconn->connclass, "gzfile");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of gzfile connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);

    newconn->canseek = TRUE;
    newconn->open = &gzfile_open;
    newconn->close = &gzfile_close;
    newconn->vfprintf = &dummy_vfprintf;
    newconn->fgetc_internal = &gzfile_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->seek = &gzfile_seek;
    newconn->fflush = &gzfile_fflush;
    newconn->read = &gzfile_read;
    newconn->write = &gzfile_write;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct gzfileconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of gzfile connection failed"));
    }
    static_cast<Rgzfileconn>(newconn->connprivate)->compress = compress;
    return newconn;
}

#include <bzlib.h>
typedef struct bzfileconn {
    FILE *fp;
    BZFILE *bfp;
    int compress;
} *Rbzfileconn;

static Rboolean bzfile_open(Rconnection con)
{
    Rbzfileconn bz = static_cast<Rbzfileconn>( con->connprivate);
    FILE* fp;
    BZFILE* bfp;
    int bzerror;
    char mode[] = "rb";

    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w' || con->mode[0] == 'a'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    /* regardless of the R view of the file, the file must be opened in
       binary mode where it matters */
    mode[0] = con->mode[0];
    fp = R_fopen(R_ExpandFileName(con->description), mode);
    if(!fp) {
	warning(_("cannot open bzip2-ed file '%s', probable reason '%s'"),
		R_ExpandFileName(con->description), strerror(errno));
	return FALSE;
    }
    if(con->canread) {
	bfp = BZ2_bzReadOpen(&bzerror, fp, 0, 0, NULL, 0);
	if(bzerror != BZ_OK) {
	    BZ2_bzReadClose(&bzerror, bfp);
	    fclose(fp);
	    warning(_("file '%s' appears not to be compressed by bzip2"),
		    R_ExpandFileName(con->description));
	    return FALSE;
	}
    } else {
	bfp = BZ2_bzWriteOpen(&bzerror, fp, bz->compress, 0, 0);
	if(bzerror != BZ_OK) {
	    BZ2_bzWriteClose(&bzerror, bfp, 0, NULL, NULL);
	    fclose(fp);
	    warning(_("initializing bzip2 compression for file '%s' failed"),
		    R_ExpandFileName(con->description));
	    return FALSE;
	}
    }
    bz->fp = fp;
    bz->bfp = bfp;
    con->isopen = TRUE;
    con->text = strchr(con->mode, 'b') ? FALSE : TRUE;
    set_iconv(con);
    con->save = -1000;
    return TRUE;
}

static void bzfile_close(Rconnection con)
{
    int bzerror;
    Rbzfileconn bz = CXXRSCAST(Rbzfileconn, con->connprivate);

    if(con->canread)
	BZ2_bzReadClose(&bzerror, bz->bfp);
    else
	BZ2_bzWriteClose(&bzerror, bz->bfp, 0, NULL, NULL);
    fclose(bz->fp);
    con->isopen = FALSE;
}

static size_t bzfile_read(void *ptr, size_t size, size_t nitems,
			  Rconnection con)
{
    Rbzfileconn bz = CXXRSCAST(Rbzfileconn, con->connprivate);
    size_t nread = 0, nleft;
    int bzerror;

    /* BZ2 uses 'int' for len */
    if (double( size) * double( nitems) > INT_MAX)
	error(_("too large a block specified"));

    nleft = size * nitems;
    /* we try to fill the buffer, because fgetc can interact with the stream boundaries
       resulting in truncated text streams while binary streams work fine */
    while (nleft > 0) {
	/* Need a cast as 'nread' needs to be interpreted in bytes */
	size_t n = BZ2_bzRead(&bzerror, bz->bfp, 
			      static_cast<char *>(ptr) + nread, nleft);
	if (bzerror == BZ_STREAM_END) { /* this could mean multiple streams so we need to check */
	    char *unused, *next_unused = NULL;
	    int nUnused;
	    BZ2_bzReadGetUnused(&bzerror, bz->bfp, reinterpret_cast<void**>( &unused), &nUnused);
	    if (bzerror == BZ_OK) {
		if (nUnused > 0) { /* unused bytes present - need to retain them */
		    /* given that this should be rare I don't want to add that overhead
		       to the entire bz structure so we allocate memory temporarily */
		    next_unused = static_cast<char*>( malloc(nUnused));
		    if (!next_unused)
			error(_("allocation of overflow buffer for bzfile failed"));
		    memcpy(next_unused, unused, nUnused);
		}
		if (nUnused > 0 || !feof(bz->fp)) {
		    BZ2_bzReadClose(&bzerror, bz->bfp);	
		    bz->bfp = BZ2_bzReadOpen(&bzerror, bz->fp, 0, 0, next_unused, nUnused);
		    if(bzerror != BZ_OK)
			warning(_("file '%s' has trailing content that appears not to be compressed by bzip2"),
				R_ExpandFileName(con->description));
		}
		if (next_unused) free(next_unused);
	    }
	} else if (bzerror != BZ_OK) {
	    /* bzlib docs say in this case n is invalid - but historically
	       we still used n in that case, so I keep it for now */
	    nread += n;
	    break;
	}
	nread += n;
	nleft -= n;
    }
    
    return nread / size;
}

static int bzfile_fgetc_internal(Rconnection con)
{
    char buf[1];
    size_t size;

    size = bzfile_read(buf, 1, 1, con);
    return (size < 1) ? R_EOF : (buf[0] % 256);
}

static size_t bzfile_write(const void *ptr, size_t size, size_t nitems,
			   Rconnection con)
{
    Rbzfileconn bz = CXXRSCAST(Rbzfileconn, con->connprivate);
    int bzerror;

    /* uses 'int' for len */
    if (double( size) * double( nitems) > INT_MAX)
	error(_("too large a block specified"));
    BZ2_bzWrite(&bzerror, bz->bfp, const_cast<voidp>( ptr), size*nitems);
    if(bzerror != BZ_OK) return 0;
    else return nitems;
}

static Rconnection newbzfile(const char *description, const char *mode,
			     int compress)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of bzfile connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("bzfile") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of bzfile connection failed"));
    }
    strcpy(newconn->connclass, "bzfile");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of bzfile connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);

    newconn->canseek = FALSE;
    newconn->open = &bzfile_open;
    newconn->close = &bzfile_close;
    newconn->vfprintf = &dummy_vfprintf;
    newconn->fgetc_internal = &bzfile_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->seek = &null_seek;
    newconn->fflush = &null_fflush;
    newconn->read = &bzfile_read;
    newconn->write = &bzfile_write;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct bzfileconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of bzfile connection failed"));
    }
    static_cast<Rbzfileconn>(newconn->connprivate)->compress = compress;
    return newconn;
}

#include <lzma.h>

typedef struct xzfileconn {
    FILE *fp;
    lzma_stream stream;
    lzma_action action;
    int compress;
    int type;
    lzma_filter filters[2];
    lzma_options_lzma opt_lzma;
    unsigned char buf[BUFSIZE];
} *Rxzfileconn;

static Rboolean xzfile_open(Rconnection con)
{
    Rxzfileconn xz = CXXRSCAST(Rxzfileconn, con->connprivate);
    lzma_ret ret;
    char mode[] = "rb";

    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w' || con->mode[0] == 'a'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    /* regardless of the R view of the file, the file must be opened in
       binary mode where it matters */
    mode[0] = con->mode[0];
    xz->fp = R_fopen(R_ExpandFileName(con->description), mode);
    if(!xz->fp) {
	warning(_("cannot open compressed file '%s', probable reason '%s'"),
		R_ExpandFileName(con->description), strerror(errno));
	return FALSE;
    }
    if(con->canread) {
	xz->action = LZMA_RUN;
	/* probably about 80Mb is required, but 512Mb seems OK as a limit */
	if (xz->type == 1)
	    ret = lzma_alone_decoder(&xz->stream, 536870912);
	else
	    ret = lzma_stream_decoder(&xz->stream, 536870912,
				      LZMA_CONCATENATED);
	if (ret != LZMA_OK) {
	    warning(_("cannot initialize lzma decoder, error %d"), ret);
	    return FALSE;
	}
	xz->stream.avail_in = 0;
    } else {
	lzma_stream *strm = &xz->stream;
	size_t preset_number = abs(xz->compress);
	if(xz->compress < 0) preset_number |= LZMA_PRESET_EXTREME;
	if(lzma_lzma_preset(&xz->opt_lzma, preset_number))
	    error("problem setting presets");
	xz->filters[0].id = LZMA_FILTER_LZMA2;
	xz->filters[0].options = &(xz->opt_lzma);
	xz->filters[1].id = LZMA_VLI_UNKNOWN;

	ret = lzma_stream_encoder(strm, xz->filters, LZMA_CHECK_CRC32);
	if (ret != LZMA_OK) {
	    warning(_("cannot initialize lzma encoder, error %d"), ret);
	    return FALSE;
	}
    }
    con->isopen = TRUE;
    con->text = strchr(con->mode, 'b') ? FALSE : TRUE;
    set_iconv(con);
    con->save = -1000;
    return TRUE;
}

static void xzfile_close(Rconnection con)
{
    Rxzfileconn xz = CXXRSCAST(Rxzfileconn, con->connprivate);

    if(con->canwrite) {
	lzma_ret ret;
	lzma_stream *strm = &(xz->stream);
	size_t nout, res;
	unsigned char buf[BUFSIZE];
	while(1) {
	    strm->avail_out = BUFSIZE; strm->next_out = buf;
	    ret = lzma_code(strm, LZMA_FINISH);
	    nout = BUFSIZE - strm->avail_out;
	    res = fwrite(buf, 1, nout, xz->fp);
	    if (res != nout) error("fwrite error");
	    if (ret != LZMA_OK) break;
	}
    }
    lzma_end(&(xz->stream));
    fclose(xz->fp);
    con->isopen = FALSE;
}

static size_t xzfile_read(void *ptr, size_t size, size_t nitems,
			  Rconnection con)
{
    Rxzfileconn xz = CXXRSCAST(Rxzfileconn, con->connprivate);
    lzma_stream *strm = &(xz->stream);
    lzma_ret ret;
    size_t s = size*nitems, have, given = 0;
    unsigned char *p = CXXRSCAST(unsigned char*, ptr);

    if (!s) return 0;

    while(1) {
	if (strm->avail_in == 0 && xz->action != LZMA_FINISH) {
	    strm->next_in = xz->buf;
	    strm->avail_in = fread(xz->buf, 1, BUFSIZ, xz->fp);
	    if (feof(xz->fp)) xz->action = LZMA_FINISH;
	}
	strm->avail_out = s; strm->next_out = p;
	ret = lzma_code(strm, xz->action);
	have = s - strm->avail_out;  given += have;
	//printf("available: %d, ready: %d/%d\n", strm->avail_in, given, s);
	if (ret != LZMA_OK) {
	    if (ret != LZMA_STREAM_END) {
		switch(ret) {
		case LZMA_MEM_ERROR:
		case LZMA_MEMLIMIT_ERROR:
		    warning("lzma decoder needed more memory");
		    break;
		case LZMA_FORMAT_ERROR:
		    warning("lzma decoder format error");
		    break;
		case LZMA_DATA_ERROR:
		    warning("lzma decoder corrupt data");
		    break;
		default:
		    warning("lzma decoding result %d", ret);
		}
	    }
	    return given/size;
	}
	s -= have;
	if (!s) return nitems;
	p += have;
    }
}

static int xzfile_fgetc_internal(Rconnection con)
{
    char buf[1];
    size_t size = xzfile_read(buf, 1, 1, con);

    return (size < 1) ? R_EOF : (buf[0] % 256);
}


static size_t xzfile_write(const void *ptr, size_t size, size_t nitems,
			   Rconnection con)
{
    Rxzfileconn xz = CXXRSCAST(Rxzfileconn, con->connprivate);
    lzma_stream *strm = &(xz->stream);
    lzma_ret ret;
    size_t s = size*nitems, nout, res;
    const unsigned char *p = CXXRSCAST(const unsigned char*, ptr);
    unsigned char buf[BUFSIZE];

    if (!s) return 0;

    strm->avail_in = s;
    strm->next_in = p;
    while(1) {
	strm->avail_out = BUFSIZE; strm->next_out = buf;
	ret = lzma_code(strm, LZMA_RUN);
	if (ret > 1) {
	    switch(ret) {
	    case LZMA_MEM_ERROR:
		warning("lzma encoder needed more memory");
		break;
	    default:
		warning("lzma encoding result %d", ret);
	    }
	    return 0;
	}
	nout = BUFSIZE - strm->avail_out;
	res = fwrite(buf, 1, nout, xz->fp);
	if (res != nout) error("fwrite error");
	if (strm->avail_in == 0) return nitems;
    }
}

static Rconnection
newxzfile(const char *description, const char *mode, int type, int compress)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of xzfile connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("xzfile") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of xzfile connection failed"));
    }
    strcpy(newconn->connclass, "xzfile");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of xzfile connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);

    newconn->canseek = FALSE;
    newconn->open = &xzfile_open;
    newconn->close = &xzfile_close;
    newconn->vfprintf = &dummy_vfprintf;
    newconn->fgetc_internal = &xzfile_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->seek = &null_seek;
    newconn->fflush = &null_fflush;
    newconn->read = &xzfile_read;
    newconn->write = &xzfile_write;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct xzfileconn));
    memset(newconn->connprivate, 0, sizeof(struct xzfileconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of xzfile connection failed"));
    }
    (static_cast<Rxzfileconn>( newconn->connprivate))->type = type;
    (static_cast<Rxzfileconn>( newconn->connprivate))->compress = compress;
    return newconn;
}

/* op 0 is gzfile, 1 is bzfile, 2 is xv/lzma */
SEXP attribute_hidden do_gzfile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP sfile, sopen, ans, connclass, enc;
    const char *file, *open;
    int ncon, compress = 9;
    Rconnection con = NULL;
    int type = PRIMVAL(op);
    int subtype = 0;

    checkArity(op, args);
    sfile = CAR(args);
    if(!isString(sfile) || length(sfile) < 1)
	error(_("invalid '%s' argument"), "description");
    if(length(sfile) > 1)
	warning(_("only first element of 'description' argument used"));
    file = translateChar(STRING_ELT(sfile, 0));
    sopen = CADR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    enc = CADDR(args);
    if(!isString(enc) || length(enc) != 1 ||
       strlen(CHAR(STRING_ELT(enc, 0))) > 100) /* ASCII */
	error(_("invalid '%s' argument"), "encoding");
    if(type < 2) {
	compress = asInteger(CADDDR(args));
	if(compress == NA_LOGICAL || compress < 0 || compress > 9)
	    error(_("invalid '%s' argument"), "compress");
    }
    if(type == 2) {
	compress = asInteger(CADDDR(args));
	if(compress == NA_LOGICAL || abs(compress) > 9)
	    error(_("invalid '%s' argument"), "compress");
    }
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    if (type == 0 && (!open[0] || open[0] == 'r')) {
	/* check magic no */
	FILE *fp = fopen(R_ExpandFileName(file), "rb");
	char buf[7];
	if (fp) {
	    int res;
	    memset(buf, 0, 7); res = fread(buf, 5, 1, fp); fclose(fp);
	    if(res == 1) {
		if(!strncmp(buf, "BZh", 3)) type = 1;
		if((buf[0] == '\xFD') && !strncmp(buf+1, "7zXZ", 4)) type = 2;
		if((buf[0] == '\xFF') && !strncmp(buf+1, "LZMA", 4)) {
		    type = 2; subtype = 1;
		}
		if(!memcmp(buf, "]\0\0\200\0", 5)) {
		    type = 2; subtype = 1;
		}
		if((buf[0] == '\x89') && !strncmp(buf+1, "LZO", 3))
		    error(_("this is a %s-compressed file which this build of R does not support"), "lzop");
	    }
	}
    }
    switch(type) {
    case 0:
	con = newgzfile(file, strlen(open) ? open : "rb", compress);
	break;
    case 1:
	con = newbzfile(file, strlen(open) ? open : "rb", compress);
	break;
    case 2:
	con = newxzfile(file, strlen(open) ? open : "rb", subtype, compress);
	break;
    }
    ncon = NextConnection();
    Connections[ncon] = con;
    strncpy(con->encname, CHAR(STRING_ELT(enc, 0)), 100); /* ASCII */

    /* see the comment in do_url */
    if (con->encname[0] && !streql(con->encname, "native.enc"))
	con->canseek = CXXRFALSE;

    /* open it if desired */
    if(strlen(open)) {
	Rboolean success = con->open(con);
	if(!success) {
	    con_destroy(ncon);
	    error(_("cannot open the connection"));
	}
    }

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    switch(type) {
    case 0:
	SET_STRING_ELT(connclass, 0, mkChar("gzfile"));
	break;
    case 1:
	SET_STRING_ELT(connclass, 0, mkChar("bzfile"));
	break;
    case 2:
	SET_STRING_ELT(connclass, 0, mkChar("xzfile"));
	break;
    }
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"), R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);

    return ans;
}

/* ------------------- clipboard connections --------------------- */

#ifdef Win32
# define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
extern int GA_clipboardhastext(void); /* from ga.h */
#endif

#ifdef Unix
// Defined in unix/X11.c :
extern "C" {
Rboolean R_ReadClipboard(Rclpconn clpcon, char *type);
}
#endif

static Rboolean clp_open(Rconnection con)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);

    con->isopen = TRUE;
    con->canwrite = CXXRCONSTRUCT(Rboolean, (con->mode[0] == 'w' || con->mode[0] == 'a'));
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    thisconn->pos = 0;
    if(con->canread) {
	/* copy the clipboard contents now */
#ifdef Win32
	HGLOBAL hglb;
	char *pc;
	if(GA_clipboardhastext() &&
	   OpenClipboard(NULL) &&
	   (hglb = GetClipboardData(CF_TEXT)) &&
	   (pc = (char *)GlobalLock(hglb))) {
	    int len = strlen(pc);
	    thisconn->buff = (char *)malloc(len + 1);
	    thisconn->last = thisconn->len = len;
	    if(thisconn->buff) {
		strcpy(thisconn->buff, pc);
		GlobalUnlock(hglb);
		CloseClipboard();
	    } else {
		GlobalUnlock(hglb);
		CloseClipboard();
		thisconn->buff = NULL; thisconn->last = thisconn->len = 0;
		warning(_("memory allocation to copy clipboard failed"));
		return FALSE;
	    }
	} else {
	    thisconn->buff = NULL; thisconn->last = thisconn->len = 0;
	    warning(_("clipboard cannot be opened or contains no text"));
	    return FALSE;
	}
#else
	Rboolean res = R_ReadClipboard(thisconn, con->description);
	if(!res) return FALSE;
#endif
    } else {
	int len = (thisconn->sizeKB)*1024;
	thisconn->buff = static_cast<char *>( malloc(len + 1));
	if(!thisconn->buff) {
	    warning(_("memory allocation to open clipboard failed"));
	    return FALSE;
	}
	thisconn->len = len;
	thisconn->last = 0;
    }
    con->text = TRUE;
    set_iconv(con);
    con->save = -1000;
    thisconn->warned = FALSE;

    return TRUE;
}

static void clp_writeout(Rconnection con)
{
#ifdef Win32
    Rclpconn thisconn = con->connprivate;

    HGLOBAL hglb;
    char *s, *p;
    if ( (hglb = GlobalAlloc(GHND, thisconn->len)) &&
	 (s = (char *)GlobalLock(hglb)) ) {
	p = thisconn->buff;
	while(p < thisconn->buff + thisconn->pos) *s++ = *p++;
	*s = '\0';
	GlobalUnlock(hglb);
	if (!OpenClipboard(NULL) || !EmptyClipboard()) {
	    warning(_("Unable to open the clipboard"));
	    GlobalFree(hglb);
	} else {
	    if(!SetClipboardData(CF_TEXT, hglb)) {
		warning(_("Unable to write to the clipboard"));
		GlobalFree(hglb);
	    }
	    CloseClipboard();
	}
    }
#endif
}

static void clp_close(Rconnection con)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);

    con->isopen = FALSE;
    if(con->canwrite)
	clp_writeout(con);
    if(thisconn-> buff) free(thisconn->buff);
}

static int clp_fgetc_internal(Rconnection con)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);

    if (thisconn->pos >= thisconn->len) return R_EOF;
    return thisconn->buff[thisconn->pos++];
}

static double clp_seek(Rconnection con, double where, int origin, int rw)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);
    int newpos, oldpos = thisconn->pos;

    if(ISNA(where)) return oldpos;

    switch(origin) {
    case 2: newpos = thisconn->pos + int(where); break;
    case 3: newpos = thisconn->last + int(where); break;
    default: newpos = CXXRCONSTRUCT(int, where);
    }
    if(newpos < 0 || newpos >= thisconn->last)
	error(_("attempt to seek outside the range of the clipboard"));
    else thisconn->pos = newpos;

    return double( oldpos);
}

static void clp_truncate(Rconnection con)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);

    if(!con->isopen || !con->canwrite)
	error(_("can only truncate connections open for writing"));
    thisconn->last = thisconn->pos;
}

static int clp_fflush(Rconnection con)
{
    if(!con->isopen || !con->canwrite) return 1;
    clp_writeout(con);
    return 0;
}

static size_t clp_read(void *ptr, size_t size, size_t nitems,
			Rconnection con)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);
    int available = thisconn->len - thisconn->pos, request = size*nitems, used;
    if (double( size) * double( nitems) > INT_MAX)
	error(_("too large a block specified"));
    used = (request < available) ? request : available;
    strncpy(CXXRCONSTRUCT(static_cast<char*>, ptr), thisconn->buff, used);
    thisconn->pos += used;
    return size_t( used)/size;
}

static size_t clp_write(const void *ptr, size_t size, size_t nitems,
			 Rconnection con)
{
    Rclpconn thisconn = CXXRCONSTRUCT(static_cast<clpconn*>, con->connprivate);
    int i, len = size * nitems, used = 0;
    char c, *p = static_cast<char *>( CXXRCONSTRUCT(const_cast<void *>, ptr)), *q = thisconn->buff + thisconn->pos;

    if(!con->canwrite)
	error(_("clipboard connection is open for reading only"));
    if (double( size) * double( nitems) > INT_MAX)
	error(_("too large a block specified"));

    for(i = 0; i < len; i++) {
	if(thisconn->pos >= thisconn->len) break;
	c = *p++;
#ifdef Win32
    /* clipboard requires CRLF termination */
	if(c == '\n') {
	    *q++ = '\r';
	    thisconn->pos++;
	    if(thisconn->pos >= thisconn->len) break;
	}
#endif
	*q++ = c;
	thisconn->pos++;
	used++;
    }
    if (used < len && !thisconn->warned) {
	warning(_("clipboard buffer is full and output lost"));
	thisconn->warned = TRUE;
    }
    if(thisconn->last < thisconn->pos) thisconn->last = thisconn->pos;
    return size_t( used)/size;
}

static Rconnection newclp(const char *url, const char *inmode)
{
    Rconnection newconn;
    const char *description;
    int sizeKB = 32;
    char mode[4];

    mode[3] = '\0';
    strncpy(mode, inmode, 3);

    if(strlen(mode) == 2 && mode[1] == 't') mode[1] = '\0';

    if(strlen(mode) != 1 ||
       (mode[0] != 'r' && mode[0] != 'w'))
	error(_("'mode' for the clipboard must be 'r' or 'w'"));
#ifdef Unix
    if(mode[0] != 'r')
	error(_("'mode' for the clipboard must be 'r' on Unix"));
#endif
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of clipboard connection failed"));
    if(strncmp(url, "clipboard", 9) == 0) description = "clipboard";
    else description = url;
    newconn->connclass = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of clipboard connection failed"));
    }
    strcpy(newconn->connclass, description);
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of clipboard connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);
    newconn->open = &clp_open;
    newconn->close = &clp_close;
    newconn->vfprintf = &dummy_vfprintf;
    newconn->fgetc_internal = &clp_fgetc_internal;
    newconn->fgetc = &dummy_fgetc;
    newconn->seek = &clp_seek;
    newconn->truncate = &clp_truncate;
    newconn->fflush = &clp_fflush;
    newconn->read = &clp_read;
    newconn->write = &clp_write;
    newconn->canseek = TRUE;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct clpconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of clipboard connection failed"));
    }
    (static_cast<Rclpconn>(newconn->connprivate))->buff = NULL;
    if (strncmp(url, "clipboard-", 10) == 0) {
	sizeKB = atoi(url+10);
	if(sizeKB < 32) sizeKB = 32;
	/* Rprintf("setting clipboard size to %dKB\n", sizeKB); */
    }
    (static_cast<Rclpconn>(newconn->connprivate))->sizeKB = sizeKB;
    return newconn;
}

/* ------------------- terminal connections --------------------- */

static unsigned char  ConsoleBuf[CONSOLE_BUFFER_SIZE+1];
static unsigned char *ConsoleBufp;
static int  ConsoleBufCnt;

static int ConsoleGetchar(void)
{
    if (--ConsoleBufCnt < 0) {
	ConsoleBuf[CONSOLE_BUFFER_SIZE] = '\0';
	if (R_ReadConsole("", ConsoleBuf, CONSOLE_BUFFER_SIZE, 0) == 0) {
	    R_ClearerrConsole();
	    return R_EOF;
	}
	ConsoleBufp = ConsoleBuf;
	ConsoleBufCnt = strlen(reinterpret_cast<char *>(ConsoleBuf));
	ConsoleBufCnt--;
    }
    return *ConsoleBufp++;
}

static int stdin_fgetc(Rconnection con)
{
    return ConsoleGetchar();
}

static int stdout_vfprintf(Rconnection con, const char *format, va_list ap)
{
    if(R_Outputfile) vfprintf(R_Outputfile, format, ap);
    else Rcons_vprintf(format, ap);
    return 0;
}

static int stdout_fflush(Rconnection con)
{
    if(R_Outputfile) return fflush(R_Outputfile);
    return 0;
}

static int stderr_vfprintf(Rconnection con, const char *format, va_list ap)
{
    REvprintf(format, ap);
    return 0;
}

static int stderr_fflush(Rconnection con)
{
    /* normally stderr and hence unbuffered, but it needs not be,
       e.g. it is stdout on Win9x */
    if(R_Consolefile) return fflush(R_Consolefile);
    return 0;
}

static Rconnection newterminal(const char *description, const char *mode)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of terminal connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("terminal") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of terminal connection failed"));
    }
    strcpy(newconn->connclass, "terminal");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of terminal connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);
    newconn->isopen = TRUE;
    newconn->canread = CXXRCONSTRUCT(Rboolean, (strcmp(mode, "r") == 0));
    newconn->canwrite = CXXRCONSTRUCT(Rboolean, (strcmp(mode, "w") == 0));
    newconn->destroy = &null_close;
    newconn->connprivate = NULL;
    return newconn;
}


SEXP attribute_hidden do_stdin(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, connclass;
    Rconnection con = getConnection(0);

    checkArity(op, args);
    PROTECT(ans = ScalarInteger(0));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar(con->connclass));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    UNPROTECT(2);
    return ans;
}

SEXP attribute_hidden do_stdout(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, connclass;
    Rconnection con = getConnection(R_OutputCon);

    checkArity(op, args);
    PROTECT(ans = ScalarInteger(R_OutputCon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar(con->connclass));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    UNPROTECT(2);
    return ans;
}


SEXP attribute_hidden do_stderr(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, connclass;
    Rconnection con = getConnection(2);

    checkArity(op, args);
    PROTECT(ans = ScalarInteger(2));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar(con->connclass));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    UNPROTECT(2);
    return ans;
}

/* isatty is in unistd.h, or io.h on Windows */
#ifdef Win32
# include <io.h>
#endif
SEXP attribute_hidden do_isatty(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int con;
    /* FIXME: is this correct for consoles? */
    checkArity(op, args);
    con = asInteger(CAR(args));
    return ScalarLogical(con == NA_LOGICAL ? FALSE : isatty(con) );
}

/* ------------------- raw connections --------------------- */

/* Possible future redesign: store nbytes as TRUELENGTH */

typedef struct rawconn {
    SEXP data; /* all the data, stored as a raw vector */
    /* replace nbytes by TRUELENGTH in due course? */
    size_t pos, nbytes; /* current pos and number of bytes
			   (same pos for read and write) */
} *Rrawconn;


/* copy a raw vector into a buffer */
static void raw_init(Rconnection con, SEXP raw)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);

    thisconn->data = NAMED(raw) ? duplicate(raw) : raw;
    R_PreserveObject(thisconn->data);
    thisconn->nbytes = LENGTH(thisconn->data);
    thisconn->pos = 0;
}

static Rboolean raw_open(Rconnection con)
{
    return TRUE;
}

static void raw_close(Rconnection con)
{
}

static void raw_destroy(Rconnection con)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);

    R_ReleaseObject(thisconn->data);
    free(thisconn);
}

static void raw_resize(Rrawconn thisconn, size_t needed)
{
    size_t nalloc = 64;
    SEXP tmp;

    if (needed > 8192) nalloc = 1.2*needed; /* 20% over-allocation */
    else while(nalloc < needed) nalloc *= 2;  /* use powers of 2 if small */
    PROTECT(tmp = allocVector(RAWSXP, nalloc));
    memcpy(RAW(tmp), RAW(thisconn->data), thisconn->nbytes);
    R_ReleaseObject(thisconn->data);
    thisconn->data = tmp;
    R_PreserveObject(thisconn->data);
    UNPROTECT(1);
}

static size_t raw_write(const void *ptr, size_t size, size_t nitems,
			Rconnection con)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);
    size_t freespace = LENGTH(thisconn->data) - thisconn->pos, bytes = size*nitems;

    if (double( size) * double( nitems) + double( thisconn->pos) > R_LEN_T_MAX)
	error(_("attempting to add too many elements to raw vector"));
    /* resize may fail, when this will give an error */
    if(bytes >= freespace) raw_resize(thisconn, bytes + thisconn->pos);
    /* the source just might be this raw vector */
    memmove(RAW(thisconn->data) + thisconn->pos, ptr, bytes);
    thisconn->pos += bytes;
    if(thisconn->nbytes < thisconn->pos) thisconn->nbytes = thisconn->pos;
    return nitems;
}

static void raw_truncate(Rconnection con)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);
    thisconn->nbytes = thisconn->pos;
}

static size_t raw_read(void *ptr, size_t size, size_t nitems,
		       Rconnection con)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);
    size_t available = thisconn->nbytes - thisconn->pos, request = size*nitems, used;

    if (double( size) * double( nitems) + double( thisconn->pos) > R_LEN_T_MAX)
	error(_("too large a block specified"));
    used = (request < available) ? request : available;
    memmove(ptr, RAW(thisconn->data) + thisconn->pos, used);
    thisconn->pos += used;
    return used/size;
}

static int raw_fgetc(Rconnection con)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);
    if(thisconn->pos >= thisconn->nbytes) return R_EOF;
    else return int( RAW(thisconn->data)[thisconn->pos++]);
}

static double raw_seek(Rconnection con, double where, int origin, int rw)
{
    Rrawconn thisconn = CXXRSCAST(Rrawconn, con->connprivate);
    double newpos;
    size_t oldpos = thisconn->pos;

    if(ISNA(where)) return double( oldpos);

    /* Do the calculations here as double to avoid integer overflow */
    switch(origin) {
    case 2: newpos = thisconn->pos + where; break;
    case 3: newpos = thisconn->nbytes + where; break;
    default: newpos = where;
    }
    if(newpos < 0 || newpos > thisconn->nbytes)
	error(_("attempt to seek outside the range of the raw connection"));
    else thisconn->pos = size_t( newpos);

    return double( oldpos);
}

static Rconnection newraw(const char *description, SEXP raw, const char *mode)
{
    Rconnection newconn;

    newconn = Rconnection( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of raw connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("rawConnection") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of raw connection failed"));
    }
    strcpy(newconn->connclass, "rawConnection");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of raw connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);
    newconn->isopen = TRUE;
    newconn->text = FALSE;
    newconn->blocking = TRUE;
    newconn->canseek = TRUE;
    newconn->canwrite = CXXRCONSTRUCT(Rboolean, (mode[0] == 'w' || mode[0] == 'a'));
    newconn->canread = CXXRCONSTRUCT(Rboolean, mode[0] == 'r');
    if(strlen(mode) >= 2 && mode[1] == '+') newconn->canread = newconn->canwrite = TRUE;
    newconn->open = &raw_open;
    newconn->close = &raw_close;
    newconn->destroy = &raw_destroy;
    if(newconn->canwrite) {
	newconn->write = &raw_write;
	newconn->vfprintf = &dummy_vfprintf;
	newconn->truncate = &raw_truncate;
    }
    if(newconn->canread) {
	newconn->read = &raw_read;
	newconn->fgetc = &raw_fgetc;
    }
    newconn->seek = &raw_seek;
    newconn->connprivate = CXXRNOCAST(void*) malloc(sizeof(struct rawconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of raw connection failed"));
    }
    raw_init(newconn, raw);
    if(mode[0] == 'a') raw_seek(newconn, 0, 3, 0);
    return newconn;
}

SEXP attribute_hidden do_rawconnection(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP sfile, sraw, sopen, ans, connclass;
    const char *desc, *open;
    int ncon;
    Rconnection con = NULL;

    checkArity(op, args);
    sfile = CAR(args);
    if(!isString(sfile) || length(sfile) != 1)
	error(_("invalid '%s' argument"), "description");
    desc = translateChar(STRING_ELT(sfile, 0));
    sraw = CADR(args);
    sopen = CADDR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    if(strchr(open, 't'))
	error(_("invalid '%s' argument"), "open");
    ncon = NextConnection();
    if(TYPEOF(sraw) != RAWSXP)
	error(_("invalid '%s' argument"), "raw");
    con = Connections[ncon] = newraw(desc, sraw, open);

    /* already opened */

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("rawConnection"));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);
    return ans;
}

SEXP attribute_hidden do_rawconvalue(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con=NULL;
    Rrawconn thisconn;
    SEXP ans;

    checkArity(op, args);
    if(!inherits(CAR(args), "rawConnection"))
	error(_("'con' is not a rawConnection"));
    con = getConnection(asInteger(CAR(args)));
    if(!con->canwrite)
	error(_("'con' is not an output rawConnection"));
    thisconn = CXXRSCAST(Rrawconn, con->connprivate);
    ans = allocVector(RAWSXP, thisconn->nbytes); /* later, use TRUELENGTH? */
    memcpy(RAW(ans), RAW(thisconn->data), thisconn->nbytes);
    return ans;
}

/* ------------------- text connections --------------------- */

typedef struct textconn {
    char *data;  /* all the data */
    int cur, nchars; /* current pos and number of chars */
    char save; /* pushback */
} *Rtextconn;

typedef struct outtextconn {
    int len;  /* number of lines */
    SEXP namesymbol;
    SEXP data;
    char *lastline;
    int lastlinelength; /* buffer size */
} *Routtextconn;

/* read a R character vector into a buffer */
static void text_init(Rconnection con, SEXP text, int type)
{
    int i, nlines = length(text), nchars = 0;
    Rtextconn thisconn = CXXRSCAST(Rtextconn, con->connprivate);

    for(i = 0; i < nlines; i++)
	nchars += strlen(type == 1 ? translateChar(STRING_ELT(text, i))
			 : ((type == 3) ?translateCharUTF8(STRING_ELT(text, i))
			    : CHAR(STRING_ELT(text, i))) ) + 1;    
    thisconn->data = static_cast<char *>( malloc(nchars+1));
    if(!thisconn->data) {
	free(thisconn); free(con->description); free(con->connclass); free(con);
	error(_("cannot allocate memory for text connection"));
    }
    *(thisconn->data) = '\0';
    for(i = 0; i < nlines; i++) {
	strcat(thisconn->data,
	       type == 1 ? translateChar(STRING_ELT(text, i))
	       : ((type == 3) ?translateCharUTF8(STRING_ELT(text, i))
		  : CHAR(STRING_ELT(text, i))) );
	strcat(thisconn->data, "\n");
    }
    thisconn->nchars = nchars;
    thisconn->cur = thisconn->save = 0;
}

static Rboolean text_open(Rconnection con)
{
    con->save = -1000;
    return TRUE;
}

static void text_close(Rconnection con)
{
}

static void text_destroy(Rconnection con)
{
    Rtextconn thisconn = CXXRSCAST(Rtextconn, con->connprivate);

    free(thisconn->data);
    /* thisconn->cur = thisconn->nchars = 0; */
    free(thisconn);
}

static int text_fgetc(Rconnection con)
{
    Rtextconn thisconn = CXXRSCAST(Rtextconn, con->connprivate);
    if(thisconn->save) {
	int c;
	c = thisconn->save;
	thisconn->save = 0;
	return c;
    }
    if(thisconn->cur >= thisconn->nchars) return R_EOF;
    else return int( (thisconn->data[thisconn->cur++]));
}

static double text_seek(Rconnection con, double where, int origin, int rw)
{
    if(where >= 0) error(_("seek is not relevant for text connection"));
    return 0; /* if just asking, always at the beginning */
}

static Rconnection newtext(const char *description, SEXP text, int type)
{
    Rconnection newconn;
    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of text connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("textConnection") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of text connection failed"));
    }
    strcpy(newconn->connclass, "textConnection");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of text connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, "r");
    newconn->isopen = TRUE;
    newconn->canwrite = FALSE;
    newconn->open = &text_open;
    newconn->close = &text_close;
    newconn->destroy = &text_destroy;
    newconn->fgetc = &text_fgetc;
    newconn->seek = &text_seek;
    newconn->connprivate = CXXRNOCAST(void*) malloc(sizeof(struct textconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of text connection failed"));
    }
    text_init(newconn, text, type);
    return newconn;
}


static SEXP mkCharLocal(const char *s)
{
    cetype_t ienc = CE_NATIVE;
    if(known_to_be_latin1) ienc = CE_LATIN1;
    if(known_to_be_utf8) ienc = CE_UTF8;
    return mkCharCE(s, ienc);
}

static void outtext_close(Rconnection con)
{
    Routtextconn thisconn = CXXRSCAST(Routtextconn, con->connprivate);
    int idx = ConnIndex(con);
    SEXP tmp, env = VECTOR_ELT(OutTextData, idx);

    if(thisconn->namesymbol &&
       findVarInFrame3(env, thisconn->namesymbol, FALSE) != R_UnboundValue)
	R_unLockBinding(thisconn->namesymbol, env);
    if(strlen(thisconn->lastline) > 0) {
	PROTECT(tmp = lengthgets(thisconn->data, ++thisconn->len));
	SET_STRING_ELT(tmp, thisconn->len - 1, mkCharLocal(thisconn->lastline));
	if(thisconn->namesymbol) defineVar(thisconn->namesymbol, tmp, env);
	SET_NAMED(tmp, 2);
	thisconn->data = tmp;
	UNPROTECT(1);
    }
}

static void outtext_destroy(Rconnection con)
{
    Routtextconn thisconn = CXXRSCAST(Routtextconn, con->connprivate);
    int idx = ConnIndex(con);
    /* OutTextData is preserved, and that implies that the environment
       we are writing it and hence the character vector is protected.
       However, this could be quite expensive.
    */
    SET_VECTOR_ELT(OutTextData, idx, R_NilValue);
    if(!thisconn->namesymbol) R_ReleaseObject(thisconn->data);
    free(thisconn->lastline); free(thisconn);
}

#define LAST_LINE_LEN 256

static int text_vfprintf(Rconnection con, const char *format, va_list ap)
{
    Routtextconn thisconn = CXXRSCAST(Routtextconn, con->connprivate);
    char buf[BUFSIZE], *b = buf, *p, *q;
    const void *vmax = vmaxget();
    int res = 0, usedRalloc = FALSE, buffree,
	already = strlen(thisconn->lastline);
    SEXP tmp;

#ifdef HAVE_VA_COPY
    va_list aq;
    va_copy(aq, ap);
    if(already >= BUFSIZE) {
	/* This will fail so just call vsnprintf to get the length of
	   the new piece */
	res = vsnprintf(buf, 0, format, aq);
	if(res > 0) res += already;
	buffree = 0;
    } else {
	strcpy(b, thisconn->lastline);
	p = b + already;
	buffree = BUFSIZE - already;
	res = vsnprintf(p, buffree, format, aq);
    }
    va_end(aq);
    if(res >= buffree) { /* res is the desired output length */
	usedRalloc = TRUE;
	b = R_alloc(res + already + 1, sizeof(char));
	strcpy(b, thisconn->lastline);
	p = b + already;
	vsprintf(p, format, ap);
    } else if(res < 0) { /* just a failure indication */
#define NBUFSIZE (already + 100*BUFSIZE)
	usedRalloc = TRUE;
	b = R_alloc(NBUFSIZE, sizeof(char));
	strncpy(b, thisconn->lastline, NBUFSIZE);
	*(b + NBUFSIZE - 1) = '\0';
	p = b + already;
	res = vsnprintf(p, NBUFSIZE - already, format, ap);
	if (res < 0) {
	    *(b + NBUFSIZE - 1) = '\0';
	    warning(_("printing of extremely long output is truncated"));
	}
    }
#else /* no va_copy: very rare so don't try too hard */
    if(already >= BUFSIZE) {
	res = -1;
    } else {
	strcpy(b, thisconn->lastline);
	p = b + already;
	buffree = BUFSIZE - already;
	res = vsnprintf(p, buffree, format, ap);
	b[BUFSIZE-1] = '\0';
    }
    if (res >= buffree || res < 0)
	warning(_("printing of extremely long output is truncated"));
#endif

    /* copy buf line-by-line to object */
    for(p = b; ; p = q+1) {
	q = Rf_strchr(p, '\n');
	if(q) {
	    int idx = ConnIndex(con);
	    SEXP env = VECTOR_ELT(OutTextData, idx);
	    *q = '\0';
	    PROTECT(tmp = lengthgets(thisconn->data, ++thisconn->len));
	    SET_STRING_ELT(tmp, thisconn->len - 1, mkCharLocal(p));
	    if(thisconn->namesymbol) {
		if(findVarInFrame3(env, thisconn->namesymbol, FALSE)
		   != R_UnboundValue) R_unLockBinding(thisconn->namesymbol, env);
		defineVar(thisconn->namesymbol, tmp, env);
		R_LockBinding(thisconn->namesymbol, env);
	    } else {
		R_ReleaseObject(thisconn->data);
		R_PreserveObject(tmp);
	    }
	    thisconn->data = tmp;
	    SET_NAMED(tmp, 2);
	    UNPROTECT(1);
	} else {
	    /* retain the last line */
	    if(CXXRCONSTRUCT(int, strlen(p)) >= thisconn->lastlinelength) {
		int newlen = strlen(p) + 1;
		thisconn->lastline = CXXRCONSTRUCT(static_cast<char*>, realloc(thisconn->lastline, newlen));
		thisconn->lastlinelength = newlen;
	    }
	    strcpy(thisconn->lastline, p);
	    con->incomplete = CXXRCONSTRUCT(Rboolean, strlen(thisconn->lastline) > 0);
	    break;
	}
    }
    if(usedRalloc) vmaxset(vmax);
    return res;
}

static void outtext_init(Rconnection con, SEXP stext, const char *mode, int idx)
{
    Routtextconn thisconn = CXXRSCAST(Routtextconn, con->connprivate);
    SEXP val;

    if(stext == R_NilValue) {
	thisconn->namesymbol = NULL;
	    /* create variable pointed to by con->description */
	val = allocVector(STRSXP, 0);
	R_PreserveObject(val);
    } else {
	thisconn->namesymbol = install(con->description);
	if(strcmp(mode, "w") == 0) {
	    /* create variable pointed to by con->description */
	    PROTECT(val = allocVector(STRSXP, 0));
	    defineVar(thisconn->namesymbol, val, VECTOR_ELT(OutTextData, idx));
	    /* Not clear if this is needed, but be conservative */
	    SET_NAMED(val, 2);
	    UNPROTECT(1);
	} else {
	    /* take over existing variable */
	    val = findVar1(thisconn->namesymbol, VECTOR_ELT(OutTextData, idx),
			   STRSXP, FALSE);
	    if(val == R_UnboundValue) {
		warning(_("text connection: appending to a non-existent char vector"));
		PROTECT(val = allocVector(STRSXP, 0));
		defineVar(thisconn->namesymbol, val, VECTOR_ELT(OutTextData, idx));
		SET_NAMED(val, 2);
		UNPROTECT(1);
	    }
	    R_LockBinding(thisconn->namesymbol, VECTOR_ELT(OutTextData, idx));
	}
    }
    thisconn->len = LENGTH(val);
    thisconn->data = val;
    thisconn->lastline[0] = '\0';
    thisconn->lastlinelength = LAST_LINE_LEN;
}


static Rconnection newouttext(const char *description, SEXP stext,
			      const char *mode, int idx)
{
    Rconnection newconn;
    void *tmp;

    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of text connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("textConnection") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of text connection failed"));
    }
    strcpy(newconn->connclass, "textConnection");
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of text connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);
    newconn->isopen = TRUE;
    newconn->canread = FALSE;
    newconn->open = &text_open;
    newconn->close = &outtext_close;
    newconn->destroy = &outtext_destroy;
    newconn->vfprintf = &text_vfprintf;
    newconn->seek = &text_seek;
    newconn->connprivate = CXXRNOCAST(void*) malloc(sizeof(struct outtextconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of text connection failed"));
    }
    tmp = malloc(LAST_LINE_LEN);
    static_cast<Routtextconn>(newconn->connprivate)->lastline = static_cast<char*>(tmp);
    if(!tmp) {
	free(newconn->connprivate);
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of text connection failed"));
    }
    outtext_init(newconn, stext, mode, idx);
    return newconn;
}

SEXP attribute_hidden do_textconnection(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP sfile, stext, sopen, ans, connclass, venv;
    const char *desc, *open;
    int ncon, type;
    Rconnection con = NULL;

    checkArity(op, args);
    sfile = CAR(args);
    if(!isString(sfile) || length(sfile) != 1)
	error(_("invalid '%s' argument"), "description");
    desc = translateChar(STRING_ELT(sfile, 0));
    stext = CADR(args);
    sopen = CADDR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    venv = CADDDR(args);
    if (isNull(venv))
	error(_("use of NULL environment is defunct"));
    if (!isEnvironment(venv))
	error(_("invalid '%s' argument"), "environment");
    type = asInteger(CAD4R(args));
    if (type == NA_INTEGER)
	error(_("invalid '%s' argument"), "encoding");
    ncon = NextConnection();
    if(!strlen(open) || strncmp(open, "r", 1) == 0) {
	if(!isString(stext))
	    error(_("invalid '%s' argument"), "text");
	con = Connections[ncon] = newtext(desc, stext, type);
    } else if (strncmp(open, "w", 1) == 0 || strncmp(open, "a", 1) == 0) {
	if (OutTextData == NULL) {
	    OutTextData = allocVector(VECSXP, NCONNECTIONS);
	    R_PreserveObject(OutTextData);
	}
	SET_VECTOR_ELT(OutTextData, ncon, venv);
	if(stext == R_NilValue)
	    con = Connections[ncon] = newouttext("NULL", stext, open, ncon);
	else if(isString(stext) && length(stext) == 1)
	    con = Connections[ncon] =
		newouttext(translateChar(STRING_ELT(stext, 0)), stext,
			   open, ncon);
	else
	    error(_("invalid '%s' argument"), "text");
    }
    else
	error(_("unsupported mode"));
    /* already opened */

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("textConnection"));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);
    return ans;
}

SEXP attribute_hidden do_textconvalue(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con=NULL;
    Routtextconn thisconn;

    checkArity(op, args);
    if(!inherits(CAR(args), "textConnection"))
	error(_("'con' is not a textConnection"));
    con = getConnection(asInteger(CAR(args)));
    if(!con->canwrite)
	error(_("'con' is not an output textConnection"));
    thisconn = CXXRSCAST(Routtextconn, con->connprivate);
    return thisconn->data;
}



/* ------------------- socket connections  --------------------- */


/* socketConnection(host, port, server, blocking, open, encoding) */
SEXP attribute_hidden do_sockconn(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP scmd, sopen, ans, connclass, enc;
    const char *host, *open;
    int ncon, port, server, blocking;
    Rconnection con = NULL;

    checkArity(op, args);
#ifdef HAVE_SOCKETS
    scmd = CAR(args);
    if(!isString(scmd) || length(scmd) != 1)
	error(_("invalid '%s' argument"), "host");
    host = translateChar(STRING_ELT(scmd, 0));
    args = CDR(args);
    port = asInteger(CAR(args));
    if(port == NA_INTEGER || port < 0)
	error(_("invalid '%s' argument"), "port");
    args = CDR(args);
    server = asLogical(CAR(args));
    if(server == NA_LOGICAL)
	error(_("invalid '%s' argument"), "server");
    args = CDR(args);
    blocking = asLogical(CAR(args));
    if(blocking == NA_LOGICAL)
	error(_("invalid '%s' argument"), "blocking");
    args = CDR(args);
    sopen = CAR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    args = CDR(args);
    enc = CAR(args);
    if(!isString(enc) || length(enc) != 1 ||
       strlen(CHAR(STRING_ELT(enc, 0))) > 100) /* ASCII */
	error(_("invalid '%s' argument"), "encoding");

    ncon = NextConnection();
    con = R_newsock(host, port, server, open);
    Connections[ncon] = con;
    con->blocking = CXXRCONSTRUCT(Rboolean, blocking);
    strncpy(con->encname, CHAR(STRING_ELT(enc, 0)), 100); /* ASCII */

    /* open it if desired */
    if(strlen(open)) {
	Rboolean success = con->open(con);
	if(!success) {
	    con_destroy(ncon);
	    error(_("cannot open the connection"));
	}
    }

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("sockconn"));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);
#else
    error(_("sockets are not available on this system"));
#endif
    return ans;
}

/* ------------------- unz connections  --------------------- */

/* see dounzip.c for the details */
SEXP attribute_hidden do_unz(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP sfile, sopen, ans, connclass, enc;
    const char *file, *open;
    int ncon;
    Rconnection con = NULL;

    checkArity(op, args);
    sfile = CAR(args);
    if(!isString(sfile) || length(sfile) < 1)
	error(_("invalid '%s' argument"), "description");
    if(length(sfile) > 1)
	warning(_("only first element of 'description' argument used"));
    file = translateChar(STRING_ELT(sfile, 0));
    sopen = CADR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    enc = CADDR(args);
    if(!isString(enc) || length(enc) != 1 ||
       strlen(CHAR(STRING_ELT(enc, 0))) > 100) /* ASCII */
	error(_("invalid '%s' argument"), "encoding");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    ncon = NextConnection();
    con = Connections[ncon] = R_newunz(file, strlen(open) ? open : CXXRCONSTRUCT(const_cast<char*>, "r"));
    strncpy(con->encname, CHAR(STRING_ELT(enc, 0)), 100); /* ASCII */

    /* open it if desired */
    if(strlen(open)) {
	Rboolean success = con->open(con);
	if(!success) {
	    con_destroy(ncon);
	    error(_("cannot open the connection"));
	}
    }

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("unz"));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);

    return ans;
}

/* -------------- open, close, seek, truncate, flush ------------------ */

SEXP attribute_hidden do_open(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int i, block;
    Rconnection con=NULL;
    SEXP sopen;
    const char *open;
    Rboolean success;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    i = asInteger(CAR(args));
    con = getConnection(i);
    if(i < 3) error(_("cannot open standard connections"));
    if(con->isopen) {
	warning(_("connection is already open"));
	return R_NilValue;
    }
    sopen = CADR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    block = asLogical(CADDR(args));
    if(block == NA_LOGICAL)
	error(_("invalid '%s' argument"), "blocking");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    if(strlen(open) > 0) strcpy(con->mode, open);
    con->blocking = CXXRCONSTRUCT(Rboolean, block);
    success = con->open(con);
    if(!success) {
	/* con_destroy(i); user might have a reference */
	error(_("cannot open the connection"));
    }
    return R_NilValue;
}

SEXP attribute_hidden do_isopen(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con;
    int rw, res;

    checkArity(op, args);
    con = getConnection(asInteger(CAR(args)));
    rw = asInteger(CADR(args));
    res = con->isopen != FALSE;
    switch(rw) {
    case 0: break;
    case 1: res = res & con->canread; break;
    case 2: res = res & con->canwrite; break;
    default: error(_("unknown 'rw' value"));
    }
    return ScalarLogical(res);
}

SEXP attribute_hidden do_isincomplete(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    con = getConnection(asInteger(CAR(args)));
    return ScalarLogical(con->incomplete != FALSE);
}

SEXP attribute_hidden do_isseekable(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    con = getConnection(asInteger(CAR(args)));
    return ScalarLogical(con->canseek != FALSE);
}

static void con_close1(Rconnection con)
{
    if(con->isopen) con->close(con);
    if(con->isGzcon) {
	Rgzconn priv = CXXRSCAST(Rgzconn, con->connprivate);
	con_close1(priv->con);
	R_ReleaseObject(CXXRCONSTRUCT(static_cast<SEXP>, priv->con->ex_ptr));
    }
    /* close inconv and outconv if open */
    if(con->inconv) Riconv_close(con->inconv);
    if(con->outconv) Riconv_close(con->outconv);
    con->destroy(con);
    free(con->connclass);
    free(con->description);
    /* clear the pushBack */
    if(con->nPushBack > 0) {
	int j;

	for(j = 0; j < con->nPushBack; j++)
	    free(con->PushBack[j]);
	free(con->PushBack);
    }
}


static void con_destroy(int i)
{
    Rconnection con=NULL;

    con = getConnection(i);
    con_close1(con);
    free(Connections[i]);
    Connections[i] = NULL;
}


SEXP attribute_hidden do_close(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int i, j;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    i = asInteger(CAR(args));
    if(i < 3) error(_("cannot close standard connections"));
    for(j = 0; j < R_SinkNumber; j++)
	if(i == SinkCons[j])
	    error(_("cannot close output sink connection"));
    if(i == R_ErrorCon)
	error(_("cannot close messages sink connection"));
    con_destroy(i);
    return R_NilValue;
}

/* seek(con, where = numeric(), origin = "start", rw = "") */
SEXP attribute_hidden do_seek(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int origin, rw;
    Rconnection con = NULL;
    double where;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    con = getConnection(asInteger(CAR(args)));
    if(!con->isopen) error(_("connection is not open"));
    where = asReal(CADR(args));
    origin = asInteger(CADDR(args));
    rw = asInteger(CADDDR(args));
    if(!ISNAN(where) && con->nPushBack > 0) {
	/* clear pushback */
	int j;
	for(j = 0; j < con->nPushBack; j++) free(con->PushBack[j]);
	free(con->PushBack);
	con->nPushBack = 0;
    }
    return ScalarReal(con->seek(con, where, origin, rw));
}

/* truncate(con) */
SEXP attribute_hidden do_truncate(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con = NULL;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    con = getConnection(asInteger(CAR(args)));
    con->truncate(con);
    return R_NilValue;
}

SEXP attribute_hidden do_flush(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con = NULL;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    con = getConnection(asInteger(CAR(args)));
    if(con->canwrite) con->fflush(con);
    return R_NilValue;
}

/* ------------------- read, write  text --------------------- */

int Rconn_fgetc(Rconnection con)
{
    char *curLine;
    int c;

    if (con->save2 != -1000) {
	c = con->save2;
	con->save2 = -1000;
	return c;
    }
    if(con->nPushBack <= 0) {
	/* map CR or CRLF to LF */
	if (con->save != -1000) {
	    c = con->save;
	    con->save = -1000;
	    return c;
	}
	c = con->fgetc(con);
	if (c == '\r') {
	    c = con->fgetc(con);
	    if (c != '\n') {
		con->save = (c != '\r') ? c : '\n';
		return('\n');
	    }
	}
	return c;
    }
    curLine = con->PushBack[con->nPushBack-1];
    c = static_cast<unsigned char>( curLine[con->posPushBack++]);
    if(con->posPushBack >= CXXRCONSTRUCT(int, strlen(curLine))) {
	/* last character on a line, so pop the line */
	free(curLine);
	con->nPushBack--;
	con->posPushBack = 0;
	if(con->nPushBack == 0) free(con->PushBack);
    }
    return c;
}

int Rconn_ungetc(int c, Rconnection con)
{
    con->save2 = c;
    return c;
}

/* read one line (without trailing newline) from con and store it in buf */
/* return number of characters read, -1 on EOF */
int Rconn_getline(Rconnection con, char *buf, int bufsize)
{
    int c, nbuf = -1;

    while((c = Rconn_fgetc(con)) != R_EOF) {
	if(nbuf+1 >= bufsize) error(_("Line longer than buffer size"));
	if(c != '\n'){
	    buf[++nbuf] = c;
	} else {
	    buf[++nbuf] = '\0';
	    break;
	}
    }
    /* Make sure it is null-terminated and count is correct, even if
     *  file did not end with newline.
     */
    if(nbuf >= 0 && buf[nbuf]) {
	if(nbuf+1 >= bufsize) error(_("Line longer than buffer size"));
	buf[++nbuf] = '\0';
    }
    return(nbuf);
}


int Rconn_printf(Rconnection con, const char *format, ...)
{
    int res;
    va_list(ap);

    va_start(ap, format);
    /* Parentheses added for FC4 with gcc4 and -D_FORTIFY_SOURCE=2 */
    res = (con->vfprintf)(con, format, ap);
    va_end(ap);
    return res;
}

/* readLines(con = stdin(), n = 1, ok = TRUE, warn = TRUE) */
#define BUF_SIZE 1000
SEXP attribute_hidden do_readLines(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans = R_NilValue, ans2;
    int i, n, nn, nnn, ok, warn, nread, c, nbuf, buf_size = BUF_SIZE;
    cetype_t oenc = CE_NATIVE;
    Rconnection con = NULL;
    Rboolean wasopen;
    char *buf;
    const char *encoding;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    con = getConnection(asInteger(CAR(args)));
    n = asInteger(CADR(args));
    if(n == NA_INTEGER)
	error(_("invalid '%s' argument"), "n");
    ok = asLogical(CADDR(args));
    if(ok == NA_LOGICAL)
	error(_("invalid '%s' argument"), "ok");
    warn = asLogical(CADDDR(args));
    if(warn == NA_LOGICAL)
	error(_("invalid '%s' argument"), "warn");
    if(!isString(CAD4R(args)) || LENGTH(CAD4R(args)) != 1)
	error(_("invalid '%s' value"), "encoding");
    encoding = CHAR(STRING_ELT(CAD4R(args), 0)); /* ASCII */
    wasopen = con->isopen;
    try {
	if(!wasopen) {
	    char mode[5];
	    con->UTF8out = TRUE;  /* a request */
	    strcpy(mode, con->mode);
	    strcpy(con->mode, "rt");
	    if(!con->open(con)) error(_("cannot open the connection"));
	    strcpy(con->mode, mode);
	    if(!con->canread) { /* recheck */
		con->close(con);
		error(_("cannot read from this connection"));
	    }
	} else { 
	    if(!con->canread) error(_("cannot read from this connection"));
	    /* for a non-blocking connection, more input may
	       have become available, so re-position */
	    if(con->canseek && !con->blocking)
		con->seek(con, con->seek(con, -1, 1, 1), 1, 1);
	}
	con->incomplete = FALSE;
	if(con->UTF8out || streql(encoding, "UTF-8")) oenc = CE_UTF8;
	else if(streql(encoding, "latin1")) oenc = CE_LATIN1;

	buf = static_cast<char *>( malloc(buf_size));
	if(!buf)
	    error(_("cannot allocate buffer in readLines"));
	nn = (n < 0) ? 1000 : n; /* initially allocate space for 1000 lines */
	nnn = (n < 0) ? INT_MAX : n;
	PROTECT(ans = allocVector(STRSXP, nn));
	for(nread = 0; nread < nnn; nread++) {
	    if(nread >= nn) {
		ans2 = allocVector(STRSXP, 2*nn);
		for(i = 0; i < nn; i++)
		    SET_STRING_ELT(ans2, i, STRING_ELT(ans, i));
		nn *= 2;
		UNPROTECT(1); /* old ans */
		PROTECT(ans = ans2);
	    }
	    nbuf = 0;
	    while((c = Rconn_fgetc(con)) != R_EOF) {
		if(nbuf == buf_size) {
		    buf_size *= 2;
		    buf = static_cast<char *>( realloc(buf, buf_size));
		    if(!buf)
			error(_("cannot allocate buffer in readLines"));
		}
		if(c != '\n') buf[nbuf++] = c; else break;
	    }
	    buf[nbuf] = '\0';
	    SET_STRING_ELT(ans, nread, mkCharCE(buf, oenc));
	    if(c == R_EOF) goto no_more_lines;
	}
	if(!wasopen) con->close(con);
	UNPROTECT(1);
	free(buf);
	return ans;
    no_more_lines:
	if(!wasopen) con->close(con);
	if(nbuf > 0) { /* incomplete last line */
	    if(con->text && !con->blocking) {
		/* push back the rest */
		con_pushback(con, CXXRFALSE, buf);
		con->incomplete = TRUE;
	    } else {
		nread++;
		if(warn)
		    warning(_("incomplete final line found on '%s'"),
			    con->description);
	    }
	}
    } catch (...) {
	if (!wasopen && con->isopen)
	    con->close(con);
	throw;
    }
    free(buf);
    if(nread < nnn && !ok)
	error(_("too few lines read in readLines"));
    PROTECT(ans2 = allocVector(STRSXP, nread));
    for(i = 0; i < nread; i++)
	SET_STRING_ELT(ans2, i, STRING_ELT(ans, i));
    UNPROTECT(2);
    return ans2;
}

/* writeLines(text, con = stdout(), sep = "\n", useBytes) */
SEXP attribute_hidden do_writelines(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int i, con_num, useBytes;
    Rboolean wasopen;
    Rconnection con=NULL;
    const char *ssep;
    SEXP text, sep;

    checkArity(op, args);
    text = CAR(args);
    if(!isString(text)) error(_("invalid '%s' argument"), "text");
    if(!inherits(CADR(args), "connection"))
	error(_("'con' is not a connection"));
    con_num = asInteger(CADR(args));
    con = getConnection(con_num);
    sep = CADDR(args);
    if(!isString(sep)) error(_("invalid '%s' argument"), "sep");
    useBytes = asLogical(CADDDR(args));
    if(useBytes == NA_LOGICAL)
	error(_("invalid '%s' argument"), "useBytes");

    wasopen = con->isopen;
    if(!wasopen) {
	char mode[5];
	/* Documented behaviour */
	strcpy(mode, con->mode);
	strcpy(con->mode, "wt");
	if(!con->open(con)) error(_("cannot open the connection"));
	strcpy(con->mode, mode);
	if(!con->canwrite) /* unlikely, but be safe */
	    con->close(con);
    }
    if(!con->canwrite) error(_("cannot write to this connection"));
    /* NB: translateChar0() is the same as CHAR() for IS_BYTES strings */
    try {
	if(useBytes)
	    ssep = CHAR(STRING_ELT(sep, 0));
	else
	    ssep = translateChar0(STRING_ELT(sep, 0));

	/* New for 2.7.0: split the output if sink was split.
	   It would be slightly simpler just to call Rvprintf if the
	   connection was stdout(), but this way is more efficent */
	if(con_num == R_OutputCon) {
	    int j = 0;
	    Rconnection con0;
	    do {
		con0 = getConnection(con_num);
		for(i = 0; i < length(text); i++)
		    Rconn_printf(con0, "%s%s",
				 useBytes ? CHAR(STRING_ELT(text, i)) :
				 translateChar0(STRING_ELT(text, i)), ssep);
		con0->fflush(con0);
		con_num = getActiveSink(j++);
	    } while (con_num > 0);
	} else {
	    for(i = 0; i < length(text); i++)
		Rconn_printf(con, "%s%s",
			     useBytes ? CHAR(STRING_ELT(text, i)) :
			     translateChar0(STRING_ELT(text, i)), ssep);
	}
    } catch (...) {
	if (!wasopen && con->isopen)
	    con->close(con);
	throw;
    }

    if(!wasopen) con->close(con);
    return R_NilValue;
}

/* ------------------- read, write  binary --------------------- */

static void swapb(void *result, int size)
{
    int i;
    char *p = CXXRCONSTRUCT(static_cast<char*>, result), tmp;

    if (size == 1) return;
    for (i = 0; i < size/2; i++) {
	tmp = p[i];
	p[i] = p[size - i - 1];
	p[size - i - 1] = tmp;
    }
}

static SEXP readOneString(Rconnection con)
{
    char buf[10001], *p;
    int pos, m;

    for(pos = 0; pos < 10000; pos++) {
	p = buf + pos;
	m = con->read(p, sizeof(char), 1, con);
	if(!m) {
	    if(pos > 0)
		warning(_("incomplete string at end of file has been discarded"));
	    return R_NilValue;
	}
	if(*p == '\0') break;
    }
    if(pos == 10000)
	warning(_("null terminator not found: breaking string at 10000 bytes"));
    return mkChar(buf);
}

static int
rawRead(char *p, int size, int n, Rbyte *bytes, int nbytes, int *np)
{
    int avail, m;

    avail = (nbytes - *np)/size;
    m = n;
    if (m > avail) m = avail;
    if (m > 0) {
	memcpy(p, bytes + *(np), m*size);
	*np += m*size;
    }
    return m;
}

static SEXP rawOneString(Rbyte *bytes, int nbytes, int *np)
{
    Rbyte *p;
    int i;
    char *buf;
    SEXP res;

    /* just look for null terminator */
    for(i = *np, p = bytes+(*np); i < nbytes; p++, i++)
	if(*p == '\0') break;
    if(i < nbytes) { /* has terminator */
	p = bytes+(*np);
	*np = i+1;
	return mkChar(reinterpret_cast<char *>(p));
    }
    /* so no terminator */
    buf = CXXRCONSTRUCT(static_cast<char*>, R_chk_calloc(nbytes - (*np) + 1, 1));
    memcpy(buf, bytes+(*np), nbytes-(*np));
    res = mkChar(buf);
    Free(buf);
    *np = nbytes;
    return res;
}

/* readBin(con, what, n, swap) */
SEXP attribute_hidden do_readbin(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans = R_NilValue, swhat;
    int i, size, signd, swap, n, m = 0, sizedef= 4, mode = 1,
	nbytes = 0, np = 0;
    const char *what;
    void *p = NULL;
    Rboolean wasopen = TRUE, isRaw = FALSE;
    Rconnection con = NULL;
    Rbyte *bytes = NULL;

    checkArity(op, args);

    if(TYPEOF(CAR(args)) == RAWSXP) {
	isRaw = TRUE;
	bytes = RAW(CAR(args));
	nbytes = LENGTH(CAR(args));
    } else {
	con = getConnection(asInteger(CAR(args)));
	if(con->text) error(_("can only read from a binary connection"));
    }

    args = CDR(args);
    swhat = CAR(args); args = CDR(args);
    if(!isString(swhat) || length(swhat) != 1)
	error(_("invalid '%s' argument"), "what");
    what = CHAR(STRING_ELT(swhat, 0)); /* ASCII */
    n = asInteger(CAR(args)); args = CDR(args);
    if(n == NA_INTEGER || n < 0) error(_("invalid '%s' argument"), "n");
    size = asInteger(CAR(args)); args = CDR(args);
    signd = asLogical(CAR(args)); args = CDR(args);
    if(signd == NA_LOGICAL)
	error(_("invalid '%s' argument"), "signed");
    swap = asLogical(CAR(args));
    if(swap == NA_LOGICAL)
	error(_("invalid '%s' argument"), "swap");
    if(!isRaw) {
	wasopen = con->isopen;
	if(!wasopen) {
	    /* Documented behaviour */
	    char mode[5];
	    strcpy(mode, con->mode);
	    strcpy(con->mode, "rb");
	    if(!con->open(con)) error(_("cannot open the connection"));
	    strcpy(con->mode, mode);
	    if(!con->canread)
		con->close(con);
	}
	if(!con->canread) error(_("cannot read from this connection"));
    }

    try {
	if(!strcmp(what, "character")) {
	    SEXP onechar;
	    PROTECT(ans = allocVector(STRSXP, n));
	    for(i = 0, m = 0; i < n; i++) {
		onechar = isRaw ? rawOneString(bytes, nbytes, &np)
		    : readOneString(con);
		if(onechar != R_NilValue) {
		    SET_STRING_ELT(ans, i, onechar);
		    m++;
		} else break;
	    }
	} else if(!strcmp(what, "complex")) {
	    if(size == NA_INTEGER) size = sizeof(Rcomplex);
	    if(size != sizeof(Rcomplex))
		error(_("size changing is not supported for complex vectors"));
	    PROTECT(ans = allocVector(CPLXSXP, n));
	    p = CXXRNOCAST(void *) COMPLEX(ans);
	    m = isRaw ? rawRead(CXXRCONSTRUCT(static_cast<char*>, p), size, n, bytes, nbytes, &np)
		: con->read(p, size, n, con);
	    if(swap)
		for(i = 0; i < m; i++) {
		    swapb(&(COMPLEX(ans)[i].r), sizeof(double));
		    swapb(&(COMPLEX(ans)[i].i), sizeof(double));
		}
	} else {
	    if (!strcmp(what, "integer") || !strcmp(what, "int")) {
		sizedef = sizeof(int); mode = 1;
		if(size == NA_INTEGER) size = sizedef;
		switch (size) {
		case sizeof(signed char):
		case sizeof(short):
		case sizeof(int):
#if SIZEOF_LONG == 8
		case sizeof(long):
#elif SIZEOF_LONG_LONG == 8
		case sizeof(_lli_t):
#endif
		    break;
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		PROTECT(ans = allocVector(INTSXP, n));
		p = CXXRNOCAST(void *) INTEGER(ans);
	    } else if (!strcmp(what, "logical")) {
		sizedef = sizeof(int); mode = 1;
		if(size == NA_INTEGER) size = sizedef;
		switch (size) {
		case sizeof(signed char):
		case sizeof(short):
		case sizeof(int):
#if SIZEOF_LONG == 8
		case sizeof(long):
#elif SIZEOF_LONG_LONG == 8
		case sizeof(_lli_t):
#endif
		    break;
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		PROTECT(ans = allocVector(LGLSXP, n));
		p = CXXRNOCAST(void *) LOGICAL(ans);
	    } else if (!strcmp(what, "raw")) {
		sizedef = 1; mode = 1;
		if(size == NA_INTEGER) size = sizedef;
		switch (size) {
		case 1:
		    break;
		default:
		    error(_("raw is always of size 1"));
		}
		PROTECT(ans = allocVector(RAWSXP, n));
		p = CXXRNOCAST(void *) RAW(ans);
	    } else if (!strcmp(what, "numeric") || !strcmp(what, "double")) {
		sizedef = sizeof(double); mode = 2;
		if(size == NA_INTEGER) size = sizedef;
		switch (size) {
		case sizeof(double):
		case sizeof(float):
#if SIZEOF_LONG_DOUBLE > SIZEOF_DOUBLE
		case sizeof(long double):
#endif
		    break;
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		PROTECT(ans = allocVector(REALSXP, n));
		p = CXXRNOCAST(void *) REAL(ans);
	    } else
		error(_("invalid '%s' argument"), "what");

	    if(size == sizedef) {
		m = isRaw ? rawRead(CXXRCONSTRUCT(static_cast<char*>, p), size, n, bytes, nbytes, &np)
		    : con->read(p, size, n, con);
		if(swap && size > 1)
		    for(i = 0; i < m; i++) swapb(static_cast<char *>(p)+i*size, size);
	    } else {
		char buf[size];
		int s;
		if(mode == 1) {
		    for(i = 0, m = 0; i < n; i++) {
			s = isRaw ? rawRead(buf, size, 1, bytes, nbytes, &np)
			    : con->read(buf, size, 1, con);
			if(s) m++; else break;
			if(swap && size > 1) swapb(buf, size);
			switch(size) {
			case sizeof(signed char):
			    if(signd)
				INTEGER(ans)[i] = int(*(reinterpret_cast<signed char *>(buf)));
			    else
				INTEGER(ans)[i] = int(*(reinterpret_cast<unsigned char *>(buf)));
			    break;
			case sizeof(short):
			    if(signd)
				INTEGER(ans)[i] = int(*(reinterpret_cast<short *>(buf)));
			    else
				INTEGER(ans)[i] = int(*(reinterpret_cast<unsigned short *>(buf)));
			    break;
#if SIZEOF_LONG == 8
			case sizeof(long):
			    INTEGER(ans)[i] = (int)*((long *)buf);
			    break;
#elif SIZEOF_LONG_LONG == 8
			case sizeof(_lli_t):
			    INTEGER(ans)[i] = int(*(reinterpret_cast<_lli_t *>(buf)));
			    break;
#endif
			default:
			    error(_("size %d is unknown on this machine"), size);
			}
		    }
		} else if (mode == 2) {
		    for(i = 0, m = 0; i < n; i++) {
			s = isRaw ? rawRead(buf, size, 1, bytes, nbytes, &np)
			    : con->read(buf, size, 1, con);
			if(s) m++; else break;
			if(swap && size > 1) swapb(buf, size);
			switch(size) {
			case sizeof(float):
			    REAL(ans)[i] = double(*(reinterpret_cast<float *>(buf)));
			    break;
#if SIZEOF_LONG_DOUBLE > SIZEOF_DOUBLE
			case sizeof(long double):
			    REAL(ans)[i] = double(*(reinterpret_cast<long double *>(buf)));
			    break;
#endif
			default:
			    error(
				  _("size %d is unknown on this machine"),
				  size);
			}
		    }
		}
	    }
	}
    } catch (...) {
	if (!wasopen && con->isopen)
	    con->close(con);
	throw;
    }
    if(!wasopen) con->close(con);
    if(m < n) {
	PROTECT(ans = lengthgets(ans, m));
	UNPROTECT(1);
    }
    UNPROTECT(1);
    return ans;
}

/* writeBin(object, con, size, swap, useBytes) */
SEXP attribute_hidden do_writebin(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP object, ans = R_NilValue;
    int i, j, size, swap, len, n = 0, useBytes;
    const char *s;
    char *buf;
    Rboolean wasopen = TRUE, isRaw = FALSE;
    Rconnection con = NULL;

    checkArity(op, args);
    object = CAR(args);
    if(!isVectorAtomic(object))
	error(_("'x' is not an atomic vector type"));

    if(TYPEOF(CADR(args)) == RAWSXP) {
	isRaw = TRUE;
    } else {
	con = getConnection(asInteger(CADR(args)));
	if(con->text) error(_("can only write to a binary connection"));
	wasopen = con->isopen;
	if(!con->canwrite) error(_("cannot write to this connection"));
    }

    size = asInteger(CADDR(args));
    swap = asLogical(CADDDR(args));
    if(swap == NA_LOGICAL)
	error(_("invalid '%s' argument"), "swap");
    useBytes = asLogical(CAD4R(args));
    if(useBytes == NA_LOGICAL)
	error(_("invalid '%s' argument"), "useBytes");
    len = LENGTH(object);
    if(len == 0) {
	if(isRaw) return allocVector(RAWSXP, 0); else return R_NilValue;
    }
    /* RAW vectors are limited to 2^31 - 1 bytes */
    if(static_cast<double>(len) *size > INT_MAX) {
	if(isRaw)
	    error(_("only 2^31-1 bytes can be written to a raw vector"));
	else
	    error(_("only 2^31-1 bytes can be written in a single readBin() call"));
    }

    if(!wasopen) {
	/* Documented behaviour */
	char mode[5];
	strcpy(mode, con->mode);
	strcpy(con->mode, "wb");
	if(!con->open(con)) error(_("cannot open the connection"));
	strcpy(con->mode, mode);
	if(!con->canwrite) {
	    con->close(con);
	    error(_("cannot write to this connection"));
	}
    }


    try {
	if(TYPEOF(object) == STRSXP) {
	    if(isRaw) {
		Rbyte *bytes;
		int np, outlen;
		if(useBytes)
		    for(i = 0, outlen = 0; i < len; i++)
			outlen += strlen(CHAR(STRING_ELT(object, i))) + 1;
		else
		    for(i = 0, outlen = 0; i < len; i++)
			outlen += strlen(translateChar0(STRING_ELT(object, i))) + 1;
		PROTECT(ans = allocVector(RAWSXP, outlen));
		bytes = RAW(ans);
		/* translateChar0() is the same as CHAR for IS_BYTES strings */
		for(i = 0, np = 0; i < len; i++) {
		    if(useBytes)
			s = CHAR(STRING_ELT(object, i));
		    else
			s = translateChar0(STRING_ELT(object, i));
		    memcpy(bytes+np, s, strlen(s) + 1);
		    np +=  strlen(s) + 1;
		}
	    } else {
		/* translateChar0() is the same as CHAR for IS_BYTES strings */
		for(i = 0; i < len; i++) {
		    if(useBytes)
			s = CHAR(STRING_ELT(object, i));
		    else
			s = translateChar0(STRING_ELT(object, i));
		    n = con->write(s, sizeof(char), strlen(s) + 1, con);
		    if(!n) {
			warning(_("problem writing to connection"));
			break;
		    }
		}
	    }
	} else {
	    switch(TYPEOF(object)) {
	    case LGLSXP:
	    case INTSXP:
		if(size == NA_INTEGER) size = sizeof(int);
		switch (size) {
		case sizeof(signed char):
		case sizeof(short):
		case sizeof(int):
#if SIZEOF_LONG == 8
		case sizeof(long):
#elif SIZEOF_LONG_LONG == 8
		case sizeof(_lli_t):
#endif
		    break;
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		break;
	    case REALSXP:
		if(size == NA_INTEGER) size = sizeof(double);
		switch (size) {
		case sizeof(double):
		case sizeof(float):
#if SIZEOF_LONG_DOUBLE > SIZEOF_DOUBLE
		case sizeof(long double):
#endif
		    break;
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		break;
	    case CPLXSXP:
		if(size == NA_INTEGER) size = sizeof(Rcomplex);
		if(size != sizeof(Rcomplex))
		    error(_("size changing is not supported for complex vectors"));
		break;
	    case RAWSXP:
		if(size == NA_INTEGER) size = 1;
		if(size != 1)
		    error(_("size changing is not supported for raw vectors"));
		break;
	    default:
		UNIMPLEMENTED_TYPE("writeBin", object);
	    }
	    buf = CXXRCONSTRUCT(static_cast<char*>, R_chk_calloc(len, size));
	    switch(TYPEOF(object)) {
	    case LGLSXP:
	    case INTSXP:
		switch (size) {
		case sizeof(int):
		    memcpy(buf, INTEGER(object), size * len);
		    break;
#if SIZEOF_LONG == 8
		case sizeof(long):
		    {
			long l1;
			for (i = 0, j = 0; i < len; i++, j += size) {
			    l1 = long( INTEGER(object)[i]);
			    memcpy(buf + j, &l1, size);
			}
			break;
		    }
#elif SIZEOF_LONG_LONG == 8
		case sizeof(_lli_t):
		    {
			_lli_t ll1;
			for (i = 0, j = 0; i < len; i++, j += size) {
			    ll1 = _lli_t( INTEGER(object)[i]);
			    memcpy(buf + j, &ll1, size);
			}
			break;
		    }
#endif
		case 2:
		    {
			short s1;
			for (i = 0, j = 0; i < len; i++, j += size) {
			    s1 = short( INTEGER(object)[i]);
			    memcpy(buf + j, &s1, size);
			}
			break;
		    }
		case 1:
		    for (i = 0; i < len; i++)
			buf[i] = static_cast<signed char>( INTEGER(object)[i]);
		    break;
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		break;
	    case REALSXP:
		switch (size) {
		case sizeof(double):
		    memcpy(buf, REAL(object), size * len);
		    break;
		case sizeof(float):
		    {
			float f1;
			for (i = 0, j = 0; i < len; i++, j += size) {
			    f1 = float( REAL(object)[i]);
			    memcpy(buf+j, &f1, size);
			}
			break;
		    }
#if SIZEOF_LONG_DOUBLE > SIZEOF_DOUBLE
		case sizeof(long double):
		    {
			/* some systems have problems with memcpy from
			   the address of an automatic long double,
			   e.g. ix86/x86_64 Linux with gcc4 */
			static long double ld1;
			for (i = 0, j = 0; i < len; i++, j += size) {
			    ld1 = static_cast<long double>( REAL(object)[i]);
			    memcpy(buf+j, &ld1, size);
			}
			break;
		    }
#endif
		default:
		    error(_("size %d is unknown on this machine"), size);
		}
		break;
	    case CPLXSXP:
		memcpy(buf, COMPLEX(object), size * len);
		break;
	    case RAWSXP:
		memcpy(buf, RAW(object), len); /* size = 1 */
		break;
	    default:  // -Wswitch
		break;
	    }

	    if(swap && size > 1) {
		if (TYPEOF(object) == CPLXSXP)
		    for(i = 0; i < len; i++) {
			int sz = size/2;
			swapb(buf+sz*2*i, sz);
			swapb(buf+sz*(2*i+1), sz);
		    }
		else
		    for(i = 0; i < len; i++) swapb(buf+size*i, size);
	    }

	    /* write it now */
	    if(isRaw) { /* We checked size*len < 2^31-1 above */
		PROTECT(ans = allocVector(RAWSXP, size*len));
		memcpy(RAW(ans), buf, size*len);
	    } else {
		n = con->write(buf, size, len, con);
		if(n < len) warning(_("problem writing to connection"));
	    }
	    Free(buf);
	}
    } catch (...) {
	if (!wasopen && con->isopen)
	    con->close(con);
	throw;
    }
    if(!wasopen) con->close(con);
    if(isRaw) {
	R_Visible = TRUE;
	UNPROTECT(1);
    } else R_Visible = FALSE;
    return ans;
}

/* FIXME: could do any MBCS locale, but would need pushback */
static SEXP
readFixedString(Rconnection con, int len, int useBytes)
{
    SEXP ans;
    char *buf;
    int  m;
    const void *vmax = vmaxget();

    if(utf8locale && !useBytes) {
	int i, clen;
	char *p, *q;

	p = buf = static_cast<char *>( R_alloc(MB_CUR_MAX*len+1, sizeof(char)));
	memset(buf, 0, MB_CUR_MAX*len+1);
	for(i = 0; i < len; i++) {
	    q = p;
	    m = con->read(p, sizeof(char), 1, con);
	    if(!m) { if(i == 0) return R_NilValue; else break;}
	    clen = utf8clen(*p++);
	    if(clen > 1) {
		m = con->read(p, sizeof(char), clen - 1, con);
		if(m < clen - 1) error(_("invalid UTF-8 input in readChar()"));
		p += clen - 1;
		/* NB: this only checks validity of multi-byte characters */
		if(int(mbrtowc(NULL, q, clen, NULL)) < 0)
		    error(_("invalid UTF-8 input in readChar()"));
	    }
	}
    } else {
	buf = static_cast<char *>( R_alloc(len+1, sizeof(char)));
	memset(buf, 0, len+1);
	m = con->read(buf, sizeof(char), len, con);
	if(len && !m) return R_NilValue;
    }
    /* String may contain nuls which we now (R >= 2.8.0) assume to be
       padding and ignore silently */
    ans = mkChar(buf);
    vmaxset(vmax);
    return ans;
}

static SEXP
rawFixedString(Rbyte *bytes, int len, int nbytes, int *np, int useBytes)
{
    char *buf;
    SEXP res;
    const void *vmax = vmaxget();

    if(*np + len > nbytes) {
	len = nbytes - *np;
	if (!len) return(R_NilValue);
    }

    if(utf8locale && !useBytes) {
	int i, clen, iread = *np;
	char *p;
	Rbyte *q;

	p = buf = static_cast<char *>( R_alloc(MB_CUR_MAX*len+1, sizeof(char)));
	for(i = 0; i < len; i++, p+=clen, iread += clen) {
	    if (iread >= nbytes) break;
	    q = bytes + iread;
	    clen = utf8clen(*q);
	    if (iread + clen > nbytes)
		error(_("invalid UTF-8 input in readChar()"));
	    memcpy(p, q, clen);
	}
	clen = iread - (*np);
	*np = iread;
	*p = '\0';
	res = mkCharLenCE(buf, clen, CE_NATIVE);
    } else {
	/* no terminator */
	buf = CXXRCONSTRUCT(static_cast<char*>, R_chk_calloc(len + 1, 1));
	memcpy(buf, bytes + (*np), len);
	*np += len;
	res = mkCharLenCE(buf, len, CE_NATIVE);
	Free(buf);
    }
    vmaxset(vmax);
    return res;
}


/* readChar(con, nchars) */
SEXP attribute_hidden do_readchar(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans = R_NilValue, onechar, nchars;
    int i, len, n, m = 0, nbytes = 0, np = 0, useBytes;
    Rboolean wasopen = TRUE, isRaw = FALSE;
    Rconnection con = NULL;
    Rbyte *bytes = NULL;

    checkArity(op, args);

    if(TYPEOF(CAR(args)) == RAWSXP) {
	isRaw = TRUE;
	bytes = RAW(CAR(args));
	nbytes = LENGTH(CAR(args));
    } else {
	con = getConnection(asInteger(CAR(args)));
	if(!con->canread)
	    error(_("cannot read from this connection"));
    }
    nchars = CADR(args);
    n = LENGTH(nchars);
    if(n == 0) return allocVector(STRSXP, 0);
    useBytes = asLogical(CADDR(args));
    if(useBytes == NA_LOGICAL)
	error(_("invalid '%s' argument"), "useBytes");

    if (!isRaw) {
	wasopen = con->isopen;
	if(!wasopen) {
	    /* Documented behaviour */
	    char mode[5];
	    strcpy(mode, con->mode);
	    strcpy(con->mode, "rb");
	    if(!con->open(con)) error(_("cannot open the connection"));
	    strcpy(con->mode, mode);
	    if(!con->canread)
		con->close(con);
	}
	if(!con->canread) error(_("cannot read from this connection"));
    }
    try {
	if (mbcslocale && !utf8locale && !useBytes)
	    warning(_("can only read in bytes in a non-UTF-8 MBCS locale" ));
	PROTECT(ans = allocVector(STRSXP, n));
	for(i = 0, m = 0; i < n; i++) {
	    len = INTEGER(nchars)[i];
	    if(len == NA_INTEGER || len < 0)
		error(_("invalid '%s' argument"), "nchar");
	    onechar = isRaw ? rawFixedString(bytes, len, nbytes, &np, useBytes)
		: readFixedString(con, len, useBytes);
	    if(onechar != R_NilValue) {
		SET_STRING_ELT(ans, i, onechar);
		m++;
	    } else break;
	}
    } catch (...) {
	if (!wasopen && con->isopen)
	    con->close(con);
	throw;
    }
    if(!wasopen) con->close(con);
    if(m < n) {
	PROTECT(ans = lengthgets(ans, m));
	UNPROTECT(1);
    }
    UNPROTECT(1);
    return ans;
}

/* writeChar(object, con, nchars, sep, useBytes) */
SEXP attribute_hidden do_writechar(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP object, nchars, sep, ans = R_NilValue, si;
    int i, len, lenb, lenc, n, nwrite=0, slen, tlen, useBytes;
    char *buf;
    const char *s, *ssep = "";
    Rboolean wasopen = TRUE, usesep, isRaw = FALSE;
    Rconnection con = NULL;
    mbstate_t mb_st;

    checkArity(op, args);
    object = CAR(args);
    if(TYPEOF(object) != STRSXP)
	error(_("invalid '%s' argument"), "object");
    if(TYPEOF(CADR(args)) == RAWSXP) {
	isRaw = TRUE;
    } else {
	con = getConnection(asInteger(CADR(args)));
	if(!con->canwrite)
	    error(_("cannot write to this connection"));
	wasopen = con->isopen;
    }

    nchars = CADDR(args);
    sep = CADDDR(args);
    useBytes = asLogical(CAD4R(args));
    if(useBytes == NA_LOGICAL)
	error(_("invalid '%s' argument"), "useBytes");

    if(isNull(sep)) {
	usesep = FALSE;
	slen = 0;
    } else {
	usesep = TRUE;
	if (!isString(sep) || length(sep) != 1)
	    error(_("invalid '%s' argument"), "sep");
	if(useBytes)
	    ssep = CHAR(STRING_ELT(sep, 0));
	else
	    ssep = translateChar(STRING_ELT(sep, 0));
	slen = strlen(ssep) + 1;
    }
    n = LENGTH(nchars);
    if(LENGTH(object) < n)
	error(_("'object' is too short"));
    if(n == 0) {
	if(isRaw) return allocVector(RAWSXP, 0); else return R_NilValue;
    }

    len = 0;
    if (!isRaw) {
	for(i = 0; i < n; i++) {
	    /* This is not currently needed, just future-proofing in case
	       the logic gets changed */
	    if(useBytes)
		tlen = strlen(CHAR(STRING_ELT(object, i)));
	    else
		tlen = strlen(translateChar(STRING_ELT(object, i)));
	    if (tlen > len) len = tlen;
	    tlen = INTEGER(nchars)[i];
	    if(tlen == NA_INTEGER || tlen < 0)
		error(_("invalid '%s' argument"), "nchar");
	    if (tlen > len) len = tlen;
	}
	buf = static_cast<char *>( R_alloc(len + slen, sizeof(char)));
    } else {
	for (i = 0; i < n; i++)
	    len += INTEGER(nchars)[i] + slen;
	PROTECT(ans = allocVector(RAWSXP, len));
	buf = reinterpret_cast<char*>(RAW(ans));
    }

    if(!wasopen) {
	/* Documented behaviour */
	char mode[5];
	strcpy(mode, con->mode);
	strcpy(con->mode, "wb");
	if(!con->open(con)) error(_("cannot open the connection"));
	strcpy(con->mode, mode);
	if(!con->canwrite) {
	    con->close(con);
	    error(_("cannot write to this connection"));
	}
    }


    try {
	for(i = 0; i < n; i++) {
	    len = INTEGER(nchars)[i];
	    si = STRING_ELT(object, i);
	    if(CXXRCONSTRUCT(int, strlen(CHAR(si))) < LENGTH(si)) {
		if(len > LENGTH(si)) {
		    warning(_("writeChar: more bytes requested than are in the string - will zero-pad"));
		}
		memset(buf, '\0', len + slen);
		memcpy(buf, CHAR(si), len);
		if (usesep) {
		    strcat(buf, ssep);
		    len += slen;
		}
		if (!isRaw) {
		    nwrite = con->write(buf, sizeof(char), len, con);
		    if(!nwrite) {
			warning(_("problem writing to connection"));
			break;
		    }
		} else
		    buf += len;
	    } else {
		if(useBytes)
		    s = CHAR(si);
		else
		    s = translateChar(si);
		lenb = lenc = strlen(s);
		if(mbcslocale) lenc = mbstowcs(NULL, s, 0);
		/* As from 1.8.1, zero-pad if too many chars are requested. */
		if(len > lenc) {
		    warning(_("writeChar: more characters requested than are in the string - will zero-pad"));
		    lenb += (len - lenc);
		}
		if(len < lenc) {
		    if(mbcslocale) {
			/* find out how many bytes we need to write */
			int i, used;
			const char *p = s;
			mbs_init(&mb_st);
			for(i = 0, lenb = 0; i < len; i++) {
			    used = Mbrtowc(NULL, p, MB_CUR_MAX, &mb_st);
			    p += used;
			    lenb += used;
			}
		    } else
			lenb = len;
		}
		memset(buf, '\0', lenb + slen);
		strncpy(buf, s, lenb);
		if (usesep) {
		    strcat(buf, ssep);
		    lenb += slen;
		}
		if (!isRaw) {
		    nwrite = con->write(buf, sizeof(char), lenb, con);
		    if(!nwrite) {
			warning(_("problem writing to connection"));
			break;
		    }
		} else
		    buf += lenb;
	    }
	}
    } catch (...) {
	if (!wasopen && con->isopen)
	    con->close(con);
	throw;
    }
    if(!wasopen) con->close(con);
    if(isRaw) {
	R_Visible = TRUE;
	UNPROTECT(1);
    } else {
	ans = R_NilValue;
	R_Visible = FALSE;
    }
    return ans;
}

/* ------------------- push back text  --------------------- */


/* used in readLines and scan */
attribute_hidden
void con_pushback(Rconnection con, Rboolean newLine, char *line)
{
    int nexists = con->nPushBack;
    char **q;

    if(nexists > 0) {
	q = con->PushBack =
	    static_cast<char **>( realloc(con->PushBack, (nexists+1)*sizeof(char *)));
    } else {
	q = con->PushBack = static_cast<char **>( malloc(sizeof(char *)));
    }
    if(!q) error(_("could not allocate space for pushBack"));
    q += nexists;
    *q = static_cast<char *>( malloc(strlen(line) + 1 + newLine));
    if(!(*q)) error(_("could not allocate space for pushBack"));
    strcpy(*q, line);
    if(newLine) strcat(*q, "\n");
    q++;
    con->posPushBack = 0;
    con->nPushBack++;
}


SEXP attribute_hidden do_pushback(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int i, n, nexists, newLine;
    Rconnection con = NULL;
    SEXP stext;
    const char *p;
    char **q;

    checkArity(op, args);

    stext = CAR(args);
    if(!isString(stext))
	error(_("invalid '%s' argument"), "data");
    con = getConnection(asInteger(CADR(args)));
    newLine = asLogical(CADDR(args));
    if(newLine == NA_LOGICAL)
	error(_("invalid '%s' argument"), "newLine");
    if(!con->canread && !con->isopen)
	error(_("can only push back on open readable connections"));
    if(!con->text)
	error(_("can only push back on text-mode connections"));
    nexists = con->nPushBack;
    if((n = length(stext)) > 0) {
	if(nexists > 0) {
	    q = con->PushBack =
		static_cast<char **>( realloc(con->PushBack, (n+nexists)*sizeof(char *)));
	} else {
	    q = con->PushBack = static_cast<char **>( malloc(n*sizeof(char *)));
	}
	if(!q) error(_("could not allocate space for pushBack"));
	q += nexists;
	for(i = 0; i < n; i++) {
	    p = translateChar(STRING_ELT(stext, n - i - 1));
	    *q = static_cast<char *>( malloc(strlen(p) + 1 + newLine));
	    if(!(*q)) error(_("could not allocate space for pushBack"));
	    strcpy(*q, p);
	    if(newLine) strcat(*q, "\n");
	    q++;
	}
	con->posPushBack = 0;
	con->nPushBack += n;
    }
    return R_NilValue;
}

SEXP attribute_hidden do_pushbacklength(SEXP call, SEXP op, SEXP args, SEXP env)
{
    Rconnection con = NULL;

    con = getConnection(asInteger(CAR(args)));
    return ScalarInteger(con->nPushBack);
}

SEXP attribute_hidden do_clearpushback(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int j;
    Rconnection con = NULL;

    con = getConnection(asInteger(CAR(args)));

    if(con->nPushBack > 0) {
	for(j = 0; j < con->nPushBack; j++) free(con->PushBack[j]);
	free(con->PushBack);
	con->nPushBack = 0;
    }
    return R_NilValue;
}

/* ------------------- sink functions  --------------------- */

/* Switch output to connection number icon, or pop stack if icon < 0
 */

static Rboolean
switch_or_tee_stdout(int icon, int closeOnExit, int tee)
{
    int toclose;

    if(icon == R_OutputCon) return FALSE;

    if(icon >= 0 && R_SinkNumber >= NSINKS - 1)
	error(_("sink stack is full"));

    if(icon == 0)
	error(_("cannot switch output to stdin"));
    else if(icon == 1 || icon == 2) {
	R_OutputCon = SinkCons[++R_SinkNumber] = icon;
	R_SinkSplit[R_SinkNumber] = tee;
	SinkConsClose[R_SinkNumber] = 0;
    } else if(icon >= 3) {
	Rconnection con = getConnection(icon); /* checks validity */
	toclose = 2*closeOnExit;
	if(!con->isopen) {
	    char mode[5];
	    strcpy(mode, con->mode);
	    strcpy(con->mode, "wt");
	    if(!con->open(con)) error(_("cannot open the connection"));
	    strcpy(con->mode, mode);
	    if(!con->canwrite) {
		con->close(con);
		error(_("cannot write to this connection"));
	    }
	    toclose = 1;
	} else if(!con->canwrite)
	    error(_("cannot write to this connection"));
	R_OutputCon = SinkCons[++R_SinkNumber] = icon;
	SinkConsClose[R_SinkNumber] = toclose;
	R_SinkSplit[R_SinkNumber] = tee;
	R_PreserveObject(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
   } else { /* removing a sink */
	if (R_SinkNumber <= 0) {
	    warning(_("no sink to remove"));
	    return FALSE;
	} else {
	    R_OutputCon = SinkCons[--R_SinkNumber];
	    if((icon = SinkCons[R_SinkNumber + 1]) >= 3) {
		Rconnection con = getConnection(icon);
		R_ReleaseObject(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
		if(SinkConsClose[R_SinkNumber + 1] == 1) /* close it */
		    con->close(con);
		else if (SinkConsClose[R_SinkNumber + 1] == 2) /* destroy it */
		    con_destroy(icon);
	    }
	}
    }
    return TRUE;
}

/* This is only used by cat() */
Rboolean attribute_hidden switch_stdout(int icon, int closeOnExit)
{
  return switch_or_tee_stdout(icon, closeOnExit, 0);
}

SEXP attribute_hidden do_sink(SEXP call, SEXP op, SEXP args, SEXP rho)
{
  int icon, closeOnExit, errcon, tee;

    checkArity(op, args);
    icon = asInteger(CAR(args));
    closeOnExit = asLogical(CADR(args));
    if(closeOnExit == NA_LOGICAL)
	error(_("invalid '%s' argument"), "closeOnExit");
    errcon = asLogical(CADDR(args));
    if(errcon == NA_LOGICAL) error(_("invalid '%s' argument"), "type");
    tee = asLogical(CADDDR(args));
    if(tee == NA_LOGICAL) error(_("invalid '%s' argument"), "split");

    if(!errcon) {
	/* allow space for cat() to use sink() */
	if(icon >= 0 && R_SinkNumber >= NSINKS - 2)
	    error(_("sink stack is full"));
	switch_or_tee_stdout(icon, closeOnExit, tee);
    } else {
	if(icon < 0) {
	    R_ReleaseObject(CXXRCONSTRUCT(static_cast<SEXP>, getConnection(R_ErrorCon)->ex_ptr));
	    R_ErrorCon = 2;
	} else {
	    getConnection(icon); /* check validity */
	    R_ErrorCon = icon;
	    R_PreserveObject(CXXRCONSTRUCT(static_cast<SEXP>, getConnection(icon)->ex_ptr));
	}
    }

    return R_NilValue;
}

SEXP attribute_hidden do_sinknumber(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int errcon;
    checkArity(op, args);

    errcon = asLogical(CAR(args));
    if(errcon == NA_LOGICAL)
	error(_("invalid '%s' argument"), "type");
    return ScalarInteger(errcon ? R_SinkNumber : R_ErrorCon);
}

#ifdef Win32
#include <R_ext/RStartup.h>
extern UImode CharacterMode;
void WinCheckUTF8(void)
{
    if(CharacterMode == RGui) WinUTF8out = (SinkCons[R_SinkNumber] == 1);
    else WinUTF8out = FALSE;
}
#endif

/* ------------------- admin functions  --------------------- */

void attribute_hidden InitConnections()
{
    int i;
    Connections[0] = newterminal("stdin", "r");
    Connections[0]->fgetc = stdin_fgetc;
    Connections[1] = newterminal("stdout", "w");
    Connections[1]->vfprintf = stdout_vfprintf;
    Connections[1]->fflush = stdout_fflush;
    Connections[2] = newterminal("stderr", "w");
    Connections[2]->vfprintf = stderr_vfprintf;
    Connections[2]->fflush = stderr_fflush;
    for(i = 3; i < NCONNECTIONS; i++) Connections[i] = NULL;
    R_OutputCon = 1;
    R_SinkNumber = 0;
    SinkCons[0] = 1; R_ErrorCon = 2;
}

SEXP attribute_hidden
do_getallconnections(SEXP call, SEXP op, SEXP args, SEXP env)
{
    int i, j=0, n=0;
    SEXP ans;
    checkArity(op, args);
    for(i = 0; i < NCONNECTIONS; i++)
	if(Connections[i]) n++;
    PROTECT(ans = allocVector(INTSXP, n));
    for(i = 0; i < NCONNECTIONS; i++)
	if(Connections[i])
	    INTEGER(ans)[j++] = i;
    UNPROTECT(1);
    return ans;
}

SEXP attribute_hidden
do_getconnection(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, connclass;
    int what;
    Rconnection con;

    checkArity(op, args);
    what = asInteger(CAR(args));
    if (what == NA_INTEGER || what < 0 || what >= NCONNECTIONS ||
	!Connections[what]) error(_("there is no connection %d"), what);

    con = Connections[what];
    PROTECT(ans = ScalarInteger(what));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar(con->connclass));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    if (what > 2)
	setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    UNPROTECT(2);
    return ans;
}

SEXP attribute_hidden do_sumconnection(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, names, tmp;
    Rconnection Rcon;

    checkArity(op, args);
    Rcon = getConnection(asInteger(CAR(args)));
    PROTECT(ans = allocVector(VECSXP, 7));
    PROTECT(names = allocVector(STRSXP, 7));
    SET_STRING_ELT(names, 0, mkChar("description"));
    PROTECT(tmp = allocVector(STRSXP, 1));
    if(Rcon->enc == CE_UTF8)
	SET_STRING_ELT(tmp, 0, mkCharCE(Rcon->description, CE_UTF8));
    else
	SET_STRING_ELT(tmp, 0, mkChar(Rcon->description));
    SET_VECTOR_ELT(ans, 0, tmp);
    SET_STRING_ELT(names, 1, mkChar("class"));
    SET_VECTOR_ELT(ans, 1, mkString(Rcon->connclass));
    SET_STRING_ELT(names, 2, mkChar("mode"));
    SET_VECTOR_ELT(ans, 2, mkString(Rcon->mode));
    SET_STRING_ELT(names, 3, mkChar("text"));
    SET_VECTOR_ELT(ans, 3, mkString(Rcon->text? "text":"binary"));
    SET_STRING_ELT(names, 4, mkChar("opened"));
    SET_VECTOR_ELT(ans, 4, mkString(Rcon->isopen? "opened":"closed"));
    SET_STRING_ELT(names, 5, mkChar("can read"));
    SET_VECTOR_ELT(ans, 5, mkString(Rcon->canread? "yes":"no"));
    SET_STRING_ELT(names, 6, mkChar("can write"));
    SET_VECTOR_ELT(ans, 6, mkString(Rcon->canwrite? "yes":"no"));
    setAttrib(ans, R_NamesSymbol, names);
    UNPROTECT(3);
    return ans;
}


#if defined(USE_WININET_ASYNC) && !defined(USE_WININET)
# define USE_WININET 2
#endif


/* op = 0: url(description, open, blocking, encoding)
   op = 1: file(description, open, blocking, encoding)
*/
SEXP attribute_hidden do_url(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP scmd, sopen, ans, connclass, enc;
    CXXRCONST char *class2 = "url";
    const char *url, *open;
    int ncon, block, raw = 0;
    cetype_t ienc = CE_NATIVE;
    Rconnection con = NULL;
#ifdef HAVE_INTERNET
    UrlScheme type = HTTPsh;	/* -Wall */
#endif

    checkArity(op, args);
    scmd = CAR(args);
    if(!isString(scmd) || length(scmd) < 1)
	error(_("invalid '%s' argument"), "description");
    if(length(scmd) > 1)
	warning(_("only first element of 'description' argument used"));
    url = CHAR(STRING_ELT(scmd, 0)); /* ASCII */
#ifdef Win32
    if(PRIMVAL(op) && !strIsASCII(CHAR(STRING_ELT(scmd, 0))) ) {
	ienc = CE_UTF8;
	url = translateCharUTF8(STRING_ELT(scmd, 0));
    } else {
	ienc = getCharCE(STRING_ELT(scmd, 0));
	if(ienc == CE_UTF8)
	    url = CHAR(STRING_ELT(scmd, 0));
	else
	    url = translateChar(STRING_ELT(scmd, 0));
    }
#else
	url = translateChar(STRING_ELT(scmd, 0));
#endif
#ifdef HAVE_INTERNET
    if (strncmp(url, "http://", 7) == 0) type = HTTPsh;
    else if (strncmp(url, "ftp://", 6) == 0) type = FTPsh;
    else if (strncmp(url, "https://", 8) == 0) type = HTTPSsh;
#endif

    sopen = CADR(args);
    if(!isString(sopen) || length(sopen) != 1)
	error(_("invalid '%s' argument"), "open");
    open = CHAR(STRING_ELT(sopen, 0)); /* ASCII */
    block = asLogical(CADDR(args));
    if(block == NA_LOGICAL)
	error(_("invalid '%s' argument"), "block");
    enc = CADDDR(args);
    if(!isString(enc) || length(enc) != 1 ||
       strlen(CHAR(STRING_ELT(enc, 0))) > 100) /* ASCII */
	error(_("invalid '%s' argument"), "encoding");
    if(PRIMVAL(op)) {
	raw = asLogical(CAD4R(args));
	if(raw == NA_LOGICAL)
	    error(_("invalid '%s' argument"), "raw");
    }

    ncon = NextConnection();
    if(strncmp(url, "file://", 7) == 0) {
	int nh = 7;
#ifdef Win32
	/* on Windows we have file:///d:/path/to
	   whereas on Unix it is file:///path/to */
	if (strlen(url) > 9 && url[7] == '/' && url[9] == ':') nh = 8;
#endif
	con = newfile(url + nh, ienc, strlen(open) ? open : "r", raw);
	class2 = "file";
#ifdef HAVE_INTERNET
    } else if (strncmp(url, "http://", 7) == 0 ||
	       strncmp(url, "https://", 8) == 0 ||
	       strncmp(url, "ftp://", 6) == 0) {
       con = R_newurl(url, strlen(open) ? open : "r");
       (static_cast<Rurlconn>(con->connprivate))->type = type;
#endif
    } else {
	if(PRIMVAL(op)) { /* call to file() */
	    if(strlen(url) == 0) {
		if(!strlen(open)) open ="w+";
		if(strcmp(open, "w+") != 0 && strcmp(open, "w+b") != 0) {
		    open ="w+";
		    warning(_("file(\"\") only supports open = \"w+\" and open = \"w+b\": using the former"));
		}
	    }
	    if(strcmp(url, "clipboard") == 0 ||
#ifdef Win32
	       strncmp(url, "clipboard-", 10) == 0
#else
	       strcmp(url, "X11_primary") == 0
	       || strcmp(url, "X11_secondary") == 0
	       || strcmp(url, "X11_clipboard") == 0
#endif
		)
		con = newclp(url, strlen(open) ? open : CXXRCONSTRUCT(const_cast<char*>, "r"));
	    else {
		if (!raw &&
		    (!strlen(open) || streql(open, "r") || streql(open, "rt"))) {
		    /* check if this is a compressed file */
		    FILE *fp = fopen(R_ExpandFileName(url), "rb");
		    char buf[7];
		    int res, ztype = -1, subtype = 0, compress = 0;
		    if (fp) {
			memset(buf, 0, 7);
			res = fread(buf, 5, 1, fp);
			fclose(fp);
			if(res == 1) {
			    if(buf[0] == '\x1f' && buf[1] == '\x8b') ztype = 0;
			    if(!strncmp(buf, "BZh", 3)) ztype = 1;
			    if((buf[0] == '\xFD') && !strncmp(buf+1, "7zXZ", 4))
				ztype = 2;
			    if((buf[0] == '\xFF') && !strncmp(buf+1, "LZMA", 4))
			    { ztype = 2; subtype = 1;}
			    if(!memcmp(buf, "]\0\0\200\0", 5))
			    { ztype = 2; subtype = 1;}
			}
		    }
		    switch(ztype) {
		    case -1:
			con = newfile(url, ienc, strlen(open) ? open : "r", raw);
			break;
		    case 0:
			con = newgzfile(url, strlen(open) ? open : "rt", compress);
			break;
		    case 1:
			con = newbzfile(url, strlen(open) ? open : "rt", compress);
			break;
		    case 2:
			con = newxzfile(url, strlen(open) ? open : "rt", subtype, compress);
			break;
		    }
		} else
		    con = newfile(url, ienc, strlen(open) ? open : "r", raw);
	    }
	    class2 = "file";
	} else {
	    error(_("unsupported URL scheme"));
	}
    }

    Connections[ncon] = con;
    con->blocking = CXXRCONSTRUCT(Rboolean, block);
    strncpy(con->encname, CHAR(STRING_ELT(enc, 0)), 100); /* ASCII */

    /* only text-mode connections are affected, but we can't tell that
       until the connection is opened, and why set an encoding on a
       connection intended to be used in binary mode? */
    if (con->encname[0] && !streql(con->encname, "native.enc"))
	con->canseek = CXXRFALSE;
    /* open it if desired */
    if(strlen(open)) {
	Rboolean success = con->open(con);
	if(!success) {
	    con_destroy(ncon);
	    error(_("cannot open the connection"));
	}
    }

    PROTECT(ans = ScalarInteger(ncon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar(class2));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    con->ex_ptr = R_MakeExternalPtr(con->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, con->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);

    return ans;
}

/* This function allows C code to call the write method of a
   connection.  It is mainly intended as a means for C code to do a
   buffered write to sockets, but could be the start of a more
   extensive C-level connection API.  LT */
size_t R_WriteConnection(Rconnection con, CXXRCONST void *buf, size_t n)
{
    if(!con->isopen) error(_("connection is not open"));
    if(!con->canwrite) error(_("cannot write to this connection"));

    return con->write(buf, 1, n, con);
}

/* ------------------- (de)compression functions  --------------------- */

/* Code for gzcon connections is modelled on gzio.c from zlib 1.2.3 */

static int gz_magic[2] = {0x1f, 0x8b}; /* gzip magic header */

/* gzip flag byte */
#define ASCII_FLAG   0x01 /* bit 0 set: file probably ascii text */
#define HEAD_CRC     0x02 /* bit 1 set: header CRC present */
#define EXTRA_FIELD  0x04 /* bit 2 set: extra field present */
#define ORIG_NAME    0x08 /* bit 3 set: original file name present */
#define COMMENT      0x10 /* bit 4 set: file comment present */
#define RESERVED     0xE0 /* bits 5..7: reserved */

#define get_byte() (icon->read(&ccc, 1, 1, icon), ccc)
#define Z_BUFSIZE 16384


static Rboolean gzcon_open(Rconnection con)
{
    Rgzconn priv = CXXRSCAST(Rgzconn, con->connprivate);
    Rconnection icon = priv->con;

    if(!icon->isopen && !icon->open(icon)) return FALSE;
    con->isopen = TRUE;
    con->canwrite = icon->canwrite;
    con->canread = CXXRCONSTRUCT(Rboolean, !con->canwrite);
    con->save = -1000;

    priv->s.zalloc = CXXRNOCAST(alloc_func)0;
    priv->s.zfree = CXXRNOCAST(free_func)0;
    priv->s.opaque = CXXRNOCAST(voidpf)0;
    priv->s.next_in = Z_NULL;
    priv->s.next_out = Z_NULL;
    priv->s.avail_in = priv->s.avail_out = 0;
    priv->inbuf = priv->outbuf = Z_NULL;
    priv->z_err = Z_OK;
    priv->z_eof = 0;
    priv->crc = crc32(0L, Z_NULL, 0);

    if(con->canread) {
	/* read header */
	char c, ccc, method, flags, dummy[6];
	unsigned char head[2];
	uInt len;

	icon->read(head, 1, 2, icon);
	if(head[0] != gz_magic[0] || head[1] != gz_magic[1]) {
	    if(!priv->allow) {
		warning(_("file stream does not have gzip magic number"));
		return FALSE;
	    }
	    priv->nsaved = 2;
	    priv->saved[0] = head[0];
	    priv->saved[1] = head[1];
	    return TRUE;
	}
	icon->read(&method, 1, 1, icon);
	icon->read(&flags, 1, 1, icon);
	if (method != Z_DEFLATED || (flags & RESERVED) != 0) {
	    warning(_("file stream does not have valid gzip header"));
	    return FALSE;
	}
	icon->read(dummy, 1, 6, icon);
	if ((flags & EXTRA_FIELD) != 0) { /* skip the extra field */
	    len  =  uInt(get_byte());
	    len += (uInt(get_byte()))<<8;
	    /* len is garbage if EOF but the loop below will quit anyway */
	    while (len-- != 0 && get_byte() != EOF) ;
	}
	if ((flags & ORIG_NAME) != 0) { /* skip the original file name */
	    while ((c = get_byte()) != 0 && c != EOF) ;
	}
	if ((flags & COMMENT) != 0) {   /* skip the .gz file comment */
	    while ((c = get_byte()) != 0 && c != EOF) ;
	}
	if ((flags & HEAD_CRC) != 0) {  /* skip the header crc */
	    for (len = 0; len < 2; len++) (void)get_byte();
	}
	priv->s.next_in  = priv->inbuf = static_cast<Byte*>(malloc(Z_BUFSIZE));
	inflateInit2(&(priv->s), -MAX_WBITS);
    } else {
	/* write a header */
	char head[11];
	sprintf(head, "%c%c%c%c%c%c%c%c%c%c", gz_magic[0], gz_magic[1],
		Z_DEFLATED, 0 /*flags*/, 0,0,0,0 /*time*/, 0 /*xflags*/,
		0 /*OS_CODE*/);
	icon->write(head, 1, 10, icon);
	deflateInit2(&(priv->s), priv->cp, Z_DEFLATED, -MAX_WBITS,
		     8, Z_DEFAULT_STRATEGY);
	priv->s.next_out = priv->outbuf = static_cast<Byte*>(malloc(Z_BUFSIZE));
	priv->s.avail_out = Z_BUFSIZE;
    }

    return TRUE;
}

static void putLong(Rconnection con, uLong x)
{
    int n;
    unsigned char buf[4];

    for (n = 0; n < 4; n++) {
	buf[n] = (x & 0xff);
	x >>= 8;
    }
    con->write(&buf, 4, 1, con);
}


static void gzcon_close(Rconnection con)
{
    Rgzconn priv = CXXRSCAST(Rgzconn, con->connprivate);
    Rconnection icon = priv->con;

    if(icon->canwrite) {
	uInt len;
	int done = 0;
	priv->s.avail_in = 0; /* should be zero already anyway */
	for (;;) {
	    len = Z_BUFSIZE - priv->s.avail_out;

	    if (len != 0) {
		if (icon->write(priv->outbuf, 1, len, icon) != len) {
		    priv->z_err = Z_ERRNO;
		    error(_("writing error whilst flushing 'gzcon' connection"));
		}
		priv->s.next_out = priv->outbuf;
		priv->s.avail_out = Z_BUFSIZE;
	    }
	    if (done) break;
	    priv->z_err = deflate(&(priv->s), Z_FINISH);

	    /* deflate has finished flushing only when it hasn't used up
	     * all the available space in the output buffer:
	     */
	    done = (priv->s.avail_out != 0 || priv->z_err == Z_STREAM_END);

	    if (priv->z_err != Z_OK && priv->z_err != Z_STREAM_END) break;
	}
	deflateEnd(&(priv->s));
	/* NB: these must be little-endian */
	putLong(icon, priv->crc);
	putLong(icon, uLong((priv->s.total_in & 0xffffffff)));
    } else inflateEnd(&(priv->s));
    if(priv->inbuf) {free(priv->inbuf); priv->inbuf = Z_NULL;}
    if(priv->outbuf) {free(priv->outbuf); priv->outbuf = Z_NULL;}
    if(icon->isopen) icon->close(icon);
    con->isopen = FALSE;
}

static int gzcon_byte(Rgzconn priv)
{
    Rconnection icon = priv->con;

    if (priv->z_eof) return EOF;
    if (priv->s.avail_in == 0) {
	priv->s.avail_in = icon->read(priv->inbuf, 1, Z_BUFSIZE, icon);
	if (priv->s.avail_in == 0) {
	    priv->z_eof = 1;
	    return EOF;
	}
	priv->s.next_in = priv->inbuf;
    }
    priv->s.avail_in--;
    return *(priv->s.next_in)++;
}


static size_t gzcon_read(void *ptr, size_t size, size_t nitems,
			 Rconnection con)
{
    Rgzconn priv = CXXRSCAST(Rgzconn, con->connprivate);
    Rconnection icon = priv->con;
    Bytef *start = static_cast<Bytef*>(ptr);
    uLong crc;
    int n;

    if (priv->z_err == Z_STREAM_END) return 0;  /* EOF */

    /* wrapped connection only needs to handle INT_MAX */
    if (double( size) * double( nitems) > INT_MAX)
	error(_("too large a block specified"));
    if (priv->nsaved >= 0) { /* non-compressed mode */
	size_t len = size*nitems;
	int i, nsaved = priv->nsaved;
	if (len == 0) return 0;
	if (len >= 2) {
	    for(i = 0; i < priv->nsaved; i++)
		(static_cast<char *>(ptr))[i] = priv->saved[i];
	    priv->nsaved = 0;
	    return (nsaved + icon->read(static_cast<char *>(ptr)+nsaved, 1, len - nsaved,
					icon))/size;
	}
	if (len == 1) { /* size must be one */
	    if (nsaved > 0) {
		(static_cast<char *>(ptr))[0] = priv->saved[0];
		priv->saved[0] = priv->saved[1];
		priv->nsaved--;
		return 1;
	    } else
		return icon->read(ptr, 1, 1, icon);
	}
    }

    priv->s.next_out = static_cast<Bytef*>(ptr);
    priv->s.avail_out = size*nitems;

    while (priv->s.avail_out != 0) {
	if (priv->s.avail_in == 0 && !priv->z_eof) {
	    priv->s.avail_in = icon->read(priv->inbuf, 1, Z_BUFSIZE, icon);
	    if (priv->s.avail_in == 0) priv->z_eof = 1;
	    priv->s.next_in = priv->inbuf;
	}
	priv->z_err = inflate(&(priv->s), Z_NO_FLUSH);

	if (priv->z_err == Z_STREAM_END) {
	    /* Check CRC */
	    priv->crc = crc32(priv->crc, start,
			      uInt(priv->s.next_out - start));
	    start = priv->s.next_out;
	    crc = 0;
	    for (n = 0; n < 4; n++) {
		crc >>= 8;
		crc += (uLong(gzcon_byte(priv)) << 24);
	    }
	    if (crc != priv->crc) {
		priv->z_err = Z_DATA_ERROR;
		REprintf(_("crc error %x %x\n"), crc, priv->crc);
	    }
	    /* finally, get (and ignore) length */
	    for (n = 0; n < 4; n++) gzcon_byte(priv);
	}
	if (priv->z_err != Z_OK || priv->z_eof) break;
    }
    priv->crc = crc32(priv->crc, start, uInt(priv->s.next_out - start));
    return size_t(size*nitems - priv->s.avail_out)/size;
}

static size_t gzcon_write(const void *ptr, size_t size, size_t nitems,
			  Rconnection con)
{
    Rgzconn priv = CXXRSCAST(Rgzconn, con->connprivate);
    Rconnection icon = priv->con;

    if (double( size) * double( nitems) > INT_MAX)
	error(_("too large a block specified"));
    priv->s.next_in = static_cast<Bytef*>(CXXRCONSTRUCT(const_cast<void*>, ptr));
    priv->s.avail_in = size*nitems;

    while (priv->s.avail_in != 0) {
	if (priv->s.avail_out == 0) {
	    priv->s.next_out = priv->outbuf;
	    if (icon->write(priv->outbuf, 1, Z_BUFSIZE, icon) != Z_BUFSIZE) {
		priv->z_err = Z_ERRNO;
		warning(_("write error on 'gzcon' connection"));
		break;
	    }
	    priv->s.avail_out = Z_BUFSIZE;
	}
	priv->z_err = deflate(&(priv->s), Z_NO_FLUSH);
	if (priv->z_err != Z_OK) break;
    }
    priv->crc = crc32(priv->crc, static_cast<const Bytef *>(ptr), size*nitems);
    return size_t(size*nitems - priv->s.avail_in)/size;
}

static int gzcon_fgetc(Rconnection con)
{
    unsigned char c;
    int n = gzcon_read(&c, 1, 1, con);
    return (n == 1) ? c : R_EOF;
}


/* gzcon(con, level) */
SEXP attribute_hidden do_gzcon(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP ans, connclass;
    int icon, level, allow;
    Rconnection incon=NULL, newconn=NULL;
    char *m, description[1000];
    CXXRCONST char* mode = NULL;

    checkArity(op, args);
    if(!inherits(CAR(args), "connection"))
	error(_("'con' is not a connection"));
    incon = getConnection(icon = asInteger(CAR(args)));
    level = asInteger(CADR(args));
    if(level == NA_INTEGER || level < 0 || level > 9)
	error(_("'level' must be one of 0 ... 9"));
    allow = asLogical(CADDR(args));
    if(allow == NA_INTEGER)
	error(_("'allowNonCompression' must be TRUE or FALSE"));

    if(incon->isGzcon) {
	warning(_("this is already a gzcon connection"));
	return CAR(args);
    }
    m = incon->mode;
    if(strcmp(m, "r") == 0 || strncmp(m, "rb", 2) == 0) mode = "rb";
    else if (strcmp(m, "w") == 0 || strncmp(m, "wb", 2) == 0) mode = "wb";
    else error(_("can only use read- or write- binary connections"));
    if(strcmp(incon->connclass, "file") == 0 &&
       (strcmp(m, "r") == 0 || strcmp(m, "w") == 0))
	warning(_("using a text-mode 'file' connection may not work correctly"));

    else if(strcmp(incon->connclass, "textConnection") == 0 && strcmp(m, "w") == 0)
	error(_("cannot create a gzcon connection from a writable textConnection; maybe use rawConnection"));

    newconn = static_cast<Rconnection>( malloc(sizeof(struct Rconn)));
    if(!newconn) error(_("allocation of 'gzcon' connection failed"));
    newconn->connclass = static_cast<char *>( malloc(strlen("gzcon") + 1));
    if(!newconn->connclass) {
	free(newconn);
	error(_("allocation of 'gzcon' connection failed"));
    }
    strcpy(newconn->connclass, "gzcon");
    sprintf(description, "gzcon(%s)", incon->description);
    newconn->description = static_cast<char *>( malloc(strlen(description) + 1));
    if(!newconn->description) {
	free(newconn->connclass); free(newconn);
	error(_("allocation of 'gzcon' connection failed"));
    }
    init_con(newconn, description, CE_NATIVE, mode);
    newconn->text = FALSE;
    newconn->isGzcon = TRUE;
    newconn->open = &gzcon_open;
    newconn->close = &gzcon_close;
    newconn->vfprintf = &dummy_vfprintf;
    newconn->fgetc = &gzcon_fgetc;
    newconn->read = &gzcon_read;
    newconn->write = &gzcon_write;
    newconn->connprivate = CXXRNOCAST(void *) malloc(sizeof(struct gzconn));
    if(!newconn->connprivate) {
	free(newconn->description); free(newconn->connclass); free(newconn);
	error(_("allocation of 'gzcon' connection failed"));
    }
    (static_cast<Rgzconn>((newconn->connprivate)))->con = incon;
    (static_cast<Rgzconn>((newconn->connprivate)))->cp = level;
    (static_cast<Rgzconn>((newconn->connprivate)))->nsaved = -1;
    (static_cast<Rgzconn>((newconn->connprivate)))->allow = CXXRCONSTRUCT(Rboolean, allow);

    /* as there might not be an R-level reference to the wrapped
       connection */
    R_PreserveObject(CXXRCONSTRUCT(static_cast<SEXP>, incon->ex_ptr));

    Connections[icon] = newconn;
    strncpy(newconn->encname, incon->encname, 100);
    if(incon->isopen) newconn->open(newconn);
    /* show we do encoding here */

    PROTECT(ans = ScalarInteger(icon));
    PROTECT(connclass = allocVector(STRSXP, 2));
    SET_STRING_ELT(connclass, 0, mkChar("gzcon"));
    SET_STRING_ELT(connclass, 1, mkChar("connection"));
    classgets(ans, connclass);
    newconn->ex_ptr = R_MakeExternalPtr(CXXRNOCAST(void *)newconn->id, install("connection"),
				    R_NilValue);
    setAttrib(ans, R_ConnIdSymbol, CXXRCONSTRUCT(static_cast<SEXP>, newconn->ex_ptr));
    R_RegisterCFinalizerEx(CXXRCONSTRUCT(static_cast<SEXP>, newconn->ex_ptr), conFinalizer, FALSE);
    UNPROTECT(2);

    return ans;
}


/* code for in-memory (de)compression
   of data stored in a scalar string. Uses a 4-byte header of length,
   in XDR order. */

#ifndef WORDS_BIGENDIAN
static unsigned int uiSwap (unsigned int x)
{
  return((x << 24) | ((x & 0xff00) << 8) | ((x & 0xff0000) >> 8) | (x >> 24));
}
#else
static inline unsigned int uiSwap (unsigned int x)
{
    return x;
}
#endif

attribute_hidden
SEXP R_compress1(SEXP in)
{
    uLong inlen, outlen;
    int res;
    Bytef *buf;
    SEXP ans;

    if(TYPEOF(in) != RAWSXP)
	error("R_compress1 requires a raw vector");
    inlen = LENGTH(in);
    outlen = CXXRCONSTRUCT(uLong, 1.001*inlen) + 20;
    buf = static_cast<Bytef *>( CXXR_alloc(outlen + 4, sizeof(Bytef)));
    /* we want this to be system-independent */
    *(reinterpret_cast<unsigned int *>(buf)) = CXXRNOCAST(unsigned int) uiSwap(inlen);
    res = compress(buf + 4, &outlen, static_cast<Bytef *>(RAW(in)), inlen);
    if(res != Z_OK) error("internal error %d in R_compress1", res);
    ans = allocVector(RAWSXP, outlen + 4);
    memcpy(RAW(ans), buf, outlen + 4);
    return ans;
}

attribute_hidden
SEXP R_decompress1(SEXP in)
{
    uLong inlen, outlen;
    int res;
    Bytef *buf;
    unsigned char *p = RAW(in);
    SEXP ans;

    if(TYPEOF(in) != RAWSXP)
	error("R_decompress1 requires a raw vector");
    inlen = LENGTH(in);
    outlen = uLong( uiSwap(*(reinterpret_cast<unsigned int *>( p))));
    buf = static_cast<Bytef *>( CXXR_alloc(outlen, sizeof(Bytef)));
    res = uncompress(buf, &outlen, static_cast<Bytef *>(p + 4), inlen - 4);
    if(res != Z_OK) error("internal error %d in R_decompress1", res);
    ans = allocVector(RAWSXP, outlen);
    memcpy(RAW(ans), buf, outlen);
    return ans;
}

attribute_hidden
SEXP R_compress2(SEXP in)
{
    unsigned int inlen, outlen;
    int res;
    char *buf;
    SEXP ans;

    if(TYPEOF(in) != RAWSXP)
	error("R_compress2 requires a raw vector");
    inlen = LENGTH(in);
    outlen = 1.01*inlen + 600;
    buf = R_alloc(outlen + 5, sizeof(char));
    /* we want this to be system-independent */
    *(reinterpret_cast<unsigned int *>(buf)) = CXXRNOCAST(unsigned int) uiSwap(inlen);
    buf[4] = '2';
    res = BZ2_bzBuffToBuffCompress(buf + 5, &outlen,
				   reinterpret_cast<char *>(RAW(in)), inlen,
				   9, 0, 0);
    if(res != BZ_OK) error("internal error %d in R_compress2", res);
    /* printf("compressed %d to %d\n", inlen, outlen); */
    if (res != BZ_OK || outlen > inlen) {
	outlen = inlen;
	buf[4] = '0';
	memcpy(buf+5, CXXRNOCAST(char *)RAW(in), inlen);
    }
    ans = allocVector(RAWSXP, outlen + 5);
    memcpy(RAW(ans), buf, outlen + 5);
    return ans;
}

attribute_hidden
SEXP R_decompress2(SEXP in)
{
    unsigned int inlen, outlen;
    int res;
    char *buf, *p = reinterpret_cast<char *>( RAW(in)), type;
    SEXP ans;

    if(TYPEOF(in) != RAWSXP)
	error("R_decompress2 requires a raw vector");
    inlen = LENGTH(in);
    outlen = CXXRNOCAST(uLong) uiSwap(static_cast<unsigned int>(*p));
    buf = R_alloc(outlen, sizeof(char));
    type = p[4];
    if (type == '2') {
	res = BZ2_bzBuffToBuffDecompress(buf, &outlen, p + 5, inlen - 5, 0, 0);
	if(res != BZ_OK) error("internal error %d in R_decompress2", res);
    } else if (type == '1') {
	uLong outl;
	res = uncompress(reinterpret_cast<unsigned char *>( buf), &outl,
			 reinterpret_cast<Bytef *>((p + 5)), inlen - 5);
	if(res != Z_OK) error("internal error %d in R_decompress1");
    } else if (type == '0') {
	buf = p + 5;
    } else error("unknown type in R_decompress2");
    ans = allocVector(RAWSXP, outlen);
    memcpy(RAW(ans), buf, outlen);
    return ans;
}


SEXP attribute_hidden do_sockselect(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    Rboolean immediate = FALSE;
    int nsock, i;
    SEXP insock, write, val, insockfd;
    double timeout;

    checkArity(op, args);

    insock = CAR(args);
    if (TYPEOF(insock) != VECSXP || LENGTH(insock) == 0)
	error(_("not a list of sockets"));
    nsock = LENGTH(insock);

    write = CADR(args);
    if (TYPEOF(write) != LGLSXP || LENGTH(write) != nsock)
	error(_("bad write indicators"));

    timeout = asReal(CADDR(args));

    PROTECT(insockfd = allocVector(INTSXP, nsock));
    PROTECT(val = allocVector(LGLSXP, nsock));

    for (i = 0; i < nsock; i++) {
	Rconnection conn = getConnection(asInteger(VECTOR_ELT(insock, i)));
	Rsockconn scp = CXXRSCAST(Rsockconn, conn->connprivate);
	if (strcmp(conn->connclass, "sockconn") != 0)
	    error(_("not a socket connection"));
	INTEGER(insockfd)[i] = scp->fd;
	if (! LOGICAL(write)[i] && scp->pstart < scp->pend) {
	    LOGICAL(val)[i] = TRUE;
	    immediate = TRUE;
	}
	else LOGICAL(val)[i] = FALSE;
    }

    if (! immediate)
	Rsockselect(nsock, INTEGER(insockfd), LOGICAL(val), LOGICAL(write),
		    timeout);

    UNPROTECT(2);
    return val;
}

static lzma_filter filters[LZMA_FILTERS_MAX + 1];

static void init_filters(void)
{
    static size_t preset_number = 6; /* 9 | LZMA_PRESET_EXTREME; */
    static lzma_options_lzma opt_lzma;
    static Rboolean set = FALSE;
    if(set) return;
    if(lzma_lzma_preset(&opt_lzma, preset_number))
	error("problem setting presets");
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt_lzma;
    filters[1].id = LZMA_VLI_UNKNOWN;
    set = TRUE;
    /*
      printf("encoding memory usage %lu\n", lzma_raw_encoder_memusage(filters));
      printf("decoding memory usage %lu\n", lzma_raw_decoder_memusage(filters));
    */
}

attribute_hidden
SEXP R_compress3(SEXP in)
{
    unsigned int inlen, outlen;
    unsigned char *buf;
    SEXP ans;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret;

    if(TYPEOF(in) != RAWSXP)
	error("R_compress3 requires a raw vector");
    inlen = LENGTH(in);
    outlen = inlen + 5;  /* don't allow it to expand */
    buf = reinterpret_cast<unsigned char *>( R_alloc(outlen + 5, sizeof(unsigned char)));
    /* we want this to be system-independent */
    *(reinterpret_cast<unsigned int *>(buf)) = CXXRNOCAST(unsigned int) uiSwap(inlen);
    buf[4] = 'Z';

    init_filters();
    ret = lzma_raw_encoder(&strm, filters);
    if (ret != LZMA_OK) error("internal error %d in R_compress3", ret);
    strm.next_in = RAW(in);
    strm.avail_in = inlen;
    strm.next_out = buf + 5;
    strm.avail_out = outlen;
    while(!ret) ret = lzma_code(&strm, LZMA_FINISH);
    if (ret != LZMA_STREAM_END || (strm.avail_in > 0)) {
	warning("internal error %d in R_compress3", ret);
	outlen = inlen;
	buf[4] = '0';
	memcpy(buf+5, reinterpret_cast<char *>(RAW(in)), inlen);
    } else outlen = strm.total_out;
    lzma_end(&strm);

    /* printf("compressed %d to %d\n", inlen, outlen); */
    ans = allocVector(RAWSXP, outlen + 5);
    memcpy(RAW(ans), buf, outlen + 5);
    return ans;
}

attribute_hidden
SEXP R_decompress3(SEXP in)
{
    unsigned int inlen, outlen;
    unsigned char *buf, *p = RAW(in), type = p[4];
    SEXP ans;

    if(TYPEOF(in) != RAWSXP)
	error("R_decompress3 requires a raw vector");
    inlen = LENGTH(in);
    outlen = CXXRNOCAST(uLong) uiSwap(*(reinterpret_cast<unsigned int *>( p)));
    buf = reinterpret_cast<unsigned char *>( R_alloc(outlen, sizeof(unsigned char)));

    if (type == 'Z') {
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_ret ret;
	init_filters();
	ret = lzma_raw_decoder(&strm, filters);
	if (ret != LZMA_OK) error("internal error %d in R_decompress3", ret);
	strm.next_in = p + 5;
	strm.avail_in = inlen - 5;
	strm.next_out = buf;
	strm.avail_out = outlen;
	ret = lzma_code(&strm, LZMA_RUN);
	if (ret != LZMA_OK && (strm.avail_in > 0))
	    error("internal error %d in R_decompress3 %d",
		  ret, strm.avail_in);
	lzma_end(&strm);
    } else if (type == '2') {
	int res;
	res = BZ2_bzBuffToBuffDecompress(reinterpret_cast<char *>(buf), &outlen,
					 reinterpret_cast<char *>((p + 5)), inlen - 5, 0, 0);
	if(res != BZ_OK) error("internal error %d in R_decompress2", res);
    } else if (type == '1') {
	uLong outl; int res;
	res = uncompress(buf, &outl, static_cast<Bytef *>((p + 5)), inlen - 5);
	if(res != Z_OK) error("internal error %d in R_decompress1");
    } else if (type == '0') {
	buf = p + 5;
    } else error("unknown type in R_decompress3");

    ans = allocVector(RAWSXP, outlen);
    memcpy(RAW(ans), buf, outlen);
    return ans;
}

SEXP attribute_hidden
do_memCompress(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, from;
    int type, res;

    checkArity(op, args);
    ans = from = CAR(args);
    if(TYPEOF(from) != RAWSXP) error("'from' must be raw or character");
    type = asInteger(CADR(args));
    switch(type) {
    case 1: break; /* none */
    case 2: /*gzip */
    {
	Bytef *buf;
	/* could use outlen = compressBound(inlen) */
	uLong inlen = LENGTH(from), outlen = 1.001*inlen + 20;
	buf = reinterpret_cast<Bytef *>( R_alloc(outlen, sizeof(Bytef)));
	res = compress(buf, &outlen, static_cast<Bytef *>(RAW(from)), inlen);
	if(res != Z_OK) error("internal error %d in memCompress", res);
	ans = allocVector(RAWSXP, outlen);
	memcpy(RAW(ans), buf, outlen);
	break;
    }
    case 3: /* bzip */
    {
	char *buf;
	unsigned int inlen = LENGTH(from), outlen = 1.01*inlen + 600;
	buf = R_alloc(outlen, sizeof(char));
	res = BZ2_bzBuffToBuffCompress(buf, &outlen, reinterpret_cast<char *>(RAW(from)),
				       inlen, 9, 0, 0);
	if(res != BZ_OK) error("internal error %d in memCompress", res);
	ans = allocVector(RAWSXP, outlen);
	memcpy(RAW(ans), buf, outlen);
	break;
    }
    case 4: /* xv */
    {
	unsigned char *buf;
	unsigned int inlen = LENGTH(from), outlen;
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_ret ret;
	lzma_filter filters[LZMA_FILTERS_MAX + 1];
	size_t preset_number = 9 | LZMA_PRESET_EXTREME;
	lzma_options_lzma opt_lzma;

	if(lzma_lzma_preset(&opt_lzma, preset_number))
	    error("problem setting presets");
	filters[0].id = LZMA_FILTER_LZMA2;
	filters[0].options = &opt_lzma;
	filters[1].id = LZMA_VLI_UNKNOWN;

	ret = lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC32);
	if (ret != LZMA_OK) error("internal error %d in memCompress", ret);

	outlen = 1.01 * inlen + 600; /* FIXME, copied from bzip2 */
	buf = reinterpret_cast<unsigned char *>( R_alloc(outlen, sizeof(unsigned char)));
	strm.next_in = RAW(from);
	strm.avail_in = inlen;
	strm.next_out = buf;
	strm.avail_out = outlen;
	while(!ret) ret = lzma_code(&strm, LZMA_FINISH);
	if (ret != LZMA_STREAM_END || (strm.avail_in > 0))
	    error("internal error %d in memCompress", ret);
	/* If LZMZ_BUF_ERROR, could realloc and continue */
	outlen = strm.total_out;
	lzma_end(&strm);
	ans = allocVector(RAWSXP, outlen);
	memcpy(RAW(ans), buf, outlen);
	break;
    }
    default:
	break;
    }

    return ans;
}

SEXP attribute_hidden
do_memDecompress(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, from;
    int type, subtype = 0;

    checkArity(op, args);
    ans = from = CAR(args);
    if(TYPEOF(from) != RAWSXP) error("'from' must be raw or character");
    type = asInteger(CADR(args));
    if (type == 5) {/* type = 5 is "unknown" */
	char *p = reinterpret_cast<char *>( RAW(from));
	if (strncmp(p, "BZh", 3) == 0) type = 3; /* bzip2 always uses a header */
	else if(p[0] == '\x1f' && p[1] == '\x8b') type = 2; /* gzip files */
	else if((p[0] == '\xFD') && !strncmp(p+1, "7zXZ", 4)) type = 4;
	else if((p[0] == '\xFF') && !strncmp(p+1, "LZMA", 4)) {
	    type = 4; subtype = 1;
	} else if(!memcmp(p, "]\0\0\200\0", 5)) {
	    type = 4; subtype = 1;
	} else {
	    warning(_("unknown compression, assuming none"));
	    type = 1;
	}
    }

    switch(type) {
    case 1: break; /* none */
    case 2: /* gzip */
    {
	uLong inlen = LENGTH(from), outlen = 3*inlen;
	int res;
	Bytef *buf, *p = static_cast<Bytef *>(RAW(from));
	/* we check for a file header */
	if (p[0] == 0x1f && p[1] == 0x8b) { p += 2; inlen -= 2; }
	while(1) {
	    buf = reinterpret_cast<Bytef *>( R_alloc(outlen, sizeof(Bytef)));
	    res = uncompress(buf, &outlen, p, inlen);
	    if(res == Z_BUF_ERROR) { outlen *= 2; continue; }
	    if(res == Z_OK) break;
	    error("internal error %d in memDecompress(%d)", res, type);
	}
	ans = allocVector(RAWSXP, outlen);
	memcpy(RAW(ans), buf, outlen);
	break;
    }
    case 3: /* bzip2 */
    {
	unsigned int inlen = LENGTH(from), outlen = 3*inlen;
	int res;
	char *buf, *p = reinterpret_cast<char *>( RAW(from));
	while(1) {
	    buf = R_alloc(outlen, sizeof(char));
	    res = BZ2_bzBuffToBuffDecompress(buf, &outlen, p, inlen, 0, 0);
	    if(res == BZ_OUTBUFF_FULL) { outlen *= 2; continue; }
	    if(res == BZ_OK) break;
	    error("internal error %d in memDecompress(%d)", res, type);
	}
	ans = allocVector(RAWSXP, outlen);
	memcpy(RAW(ans), buf, outlen);
	break;
    }
    case 4: /* xz */
    {
	unsigned char *buf;
	unsigned int inlen = LENGTH(from), outlen = 3*inlen;
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_ret ret;
	while(1) {
	    /* Initialize lzma_stream in each iteration. */
	    /* probably at most 80Mb is required, but 512Mb seems OK as a limit */
	    if (subtype == 1)
		ret = lzma_alone_decoder(&strm, 536870912);
	    else
		ret = lzma_stream_decoder(&strm, 536870912, LZMA_CONCATENATED);
	    if (ret != LZMA_OK)
		error(_("cannot initialize lzma decoder, error %d"), ret);

	    buf = reinterpret_cast<unsigned char *>( R_alloc(outlen, sizeof(unsigned char)));
	    strm.avail_in = inlen;
	    strm.avail_out = outlen;
	    strm.next_in = CXXRNOCAST(unsigned char *) RAW(from);
	    strm.next_out = buf;

	    ret = lzma_code(&strm, LZMA_FINISH);
	    /* Did lzma_code() leave some input? */
	    if (strm.avail_in > 0) {
		/* Decompression failed, free lzma_stream. */
		lzma_end(&strm);
		/* Because it ran out of output buffer? 
		 *
		 * This used to only check if LZMA_BUF_ERROR was
		 * returned, but apparently XZ will also signal an out
		 * of buffer condition by returning LZMA_OK and
		 * leaving avail_in > 0 (i.e. not all input was
		 * consumed).
		 */
		if (ret == LZMA_BUF_ERROR || ret == LZMA_OK) {
		    outlen *= 2;
		    continue;
		} else {
		    error("internal error %d in memDecompress(%d) at %d",
			  ret, type, strm.avail_in);
		}
	    } else {
		break;
	    }
	}
	outlen = strm.total_out;
	lzma_end(&strm);
	ans = allocVector(RAWSXP, outlen);
	memcpy(RAW(ans), buf, outlen);
	break;
    }
    default:
	break;
    }
    return ans;
}
