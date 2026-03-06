/** @file mdb.c
 *	@brief Lightning memory-mapped database library
 *
 *	A Btree-based database management library modeled loosely on the
 *	BerkeleyDB API, but much simplified.
 */
/*
 * Copyright 2011-2021 Howard Chu, Symas Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 *
 * This code is derived from btree.c written by Martin Hedenfalk.
 *
 * Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#if defined(_MSC_VER)
/* Suppress warnings for MSVC */
#pragma warning(disable: 4244) /* conversion from 'type1' to 'type2', possible loss of data */
#pragma warning(disable: 4267) /* conversion from 'size_t' to 'type', possible loss of data */
#pragma warning(disable: 4018) /* signed/unsigned mismatch */
#pragma warning(disable: 4996) /* deprecated function */
#pragma warning(disable: 4102) /* unreferenced label */
#pragma warning(disable: 4146) /* unary minus operator applied to unsigned type */
#pragma warning(disable: 4333) /* right shift by too large amount */
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#if defined(MDB_VL32) || defined(__WIN64__)
#define _FILE_OFFSET_BITS	64
#endif
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#include <signal.h>
#include <wchar.h>				/* get wcscpy() */

/* We use native NT APIs to setup the memory map, so that we can
 * let the DB file grow incrementally instead of always preallocating
 * the full size. These APIs are defined in <wdm.h> and <ntifs.h>
 * but those headers are meant for driver-level development and
 * conflict with the regular user-level headers, so we explicitly
 * declare them here. We get pointers to these functions from
 * NTDLL.DLL at runtime, to avoid buildtime dependencies on any
 * NTDLL import libraries.
 */
typedef NTSTATUS (WINAPI NtCreateSectionFunc)
  (OUT PHANDLE sh, IN ACCESS_MASK acc,
  IN void * oa OPTIONAL,
  IN PLARGE_INTEGER ms OPTIONAL,
  IN ULONG pp, IN ULONG aa, IN HANDLE fh OPTIONAL);

static NtCreateSectionFunc *NtCreateSection;

typedef enum _SECTION_INHERIT {
	ViewShare = 1,
	ViewUnmap = 2
} SECTION_INHERIT;

typedef NTSTATUS (WINAPI NtMapViewOfSectionFunc)
  (IN PHANDLE sh, IN HANDLE ph,
  IN OUT PVOID *addr, IN ULONG_PTR zbits,
  IN SIZE_T cs, IN OUT PLARGE_INTEGER off OPTIONAL,
  IN OUT PSIZE_T vs, IN SECTION_INHERIT ih,
  IN ULONG at, IN ULONG pp);

static NtMapViewOfSectionFunc *NtMapViewOfSection;

typedef NTSTATUS (WINAPI NtCloseFunc)(HANDLE h);

static NtCloseFunc *NtClose;

/** getpid() returns int; MinGW defines pid_t but MinGW64 typedefs it
 *  as int64 which is wrong. MSVC doesn't define it at all, so just
 *  don't use it.
 */
#define MDB_PID_T	int
#define MDB_THR_T	DWORD
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __GNUC__
# include <sys/param.h>
#else
# define LITTLE_ENDIAN	1234
# define BIG_ENDIAN	4321
# define BYTE_ORDER	LITTLE_ENDIAN
# ifndef SSIZE_MAX
#  define SSIZE_MAX	INT_MAX
# endif
#endif
#define MDB_OFF_T	int64_t
#else
#include <sys/types.h>
#include <sys/stat.h>
#define MDB_PID_T	pid_t
#define MDB_THR_T	pthread_t
#include <sys/param.h>
#include <sys/uio.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#include <fcntl.h>
#define MDB_OFF_T	off_t
#endif

#if defined(__mips) && defined(__linux)
/* MIPS has cache coherency issues, requires explicit cache control */
#include <sys/cachectl.h>
#define CACHEFLUSH(addr, bytes, cache)	cacheflush(addr, bytes, cache)
#else
#define CACHEFLUSH(addr, bytes, cache)
#endif

#if defined(__linux) && !defined(MDB_FDATASYNC_WORKS)
/** fdatasync is broken on ext3/ext4fs on older kernels, see
 *	description in #mdb_env_open2 comments. You can safely
 *	define MDB_FDATASYNC_WORKS if this code will only be run
 *	on kernels 3.6 and newer.
 */
#define	BROKEN_FDATASYNC
#endif

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#include <io.h>
typedef SSIZE_T	ssize_t;
#else
#include <unistd.h>
#endif

#if defined(__sun) || defined(__ANDROID__)
/* Most platforms have posix_memalign, older may only have memalign */
#define HAVE_MEMALIGN	1
#include <malloc.h>
/* On Solaris, we need the POSIX sigwait function */
#if defined (__sun)
# define _POSIX_PTHREAD_SEMANTICS	1
#endif
#endif

#if !(defined(BYTE_ORDER) || defined(__BYTE_ORDER))
#include <netinet/in.h>
#include <resolv.h>	/* defines BYTE_ORDER on HPUX and Solaris */
#endif

#if defined(__FreeBSD__) && defined(__FreeBSD_version) && __FreeBSD_version >= 1100110
# define MDB_USE_POSIX_MUTEX	1
# define MDB_USE_ROBUST	1
#elif defined(__APPLE__) || defined (BSD) || defined(__FreeBSD_kernel__)
# if !(defined(MDB_USE_POSIX_MUTEX) || defined(MDB_USE_POSIX_SEM))
# define MDB_USE_SYSV_SEM	1
# endif
# if defined(__APPLE__)
# define MDB_FDATASYNC(fd)		fcntl(fd, F_FULLFSYNC)
# else
# define MDB_FDATASYNC		fsync
# endif
#elif defined(__ANDROID__)
# define MDB_FDATASYNC		fsync
#elif defined(__HAIKU__)
# define MDB_USE_POSIX_SEM	1
# define MDB_FDATASYNC		fsync
#endif

/* NetBSD does not define union semun in sys/sem.h */
#if defined(__NetBSD__) && !defined(_SEM_SEMUN_UNDEFINED)
# define _SEM_SEMUN_UNDEFINED  1
#endif

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#ifdef MDB_USE_POSIX_SEM
# define MDB_USE_HASH		1
#include <semaphore.h>
#elif defined(MDB_USE_SYSV_SEM)
#include <sys/ipc.h>
#include <sys/sem.h>
#ifdef _SEM_SEMUN_UNDEFINED
union semun {
	int val;
	struct semid_ds *buf;
	unsigned short *array;
};
#endif /* _SEM_SEMUN_UNDEFINED */
#else
#define MDB_USE_POSIX_MUTEX	1
#endif /* MDB_USE_POSIX_SEM */
#endif /* !_WIN32 */

#if defined(_WIN32) + defined(MDB_USE_POSIX_SEM) + defined(MDB_USE_SYSV_SEM) \
	+ defined(MDB_USE_POSIX_MUTEX) != 1
# error "Ambiguous shared-lock implementation"
#endif

#ifdef USE_VALGRIND
#include <valgrind/memcheck.h>
#define VGMEMP_CREATE(h,r,z)    VALGRIND_CREATE_MEMPOOL(h,r,z)
#define VGMEMP_ALLOC(h,a,s) VALGRIND_MEMPOOL_ALLOC(h,a,s)
#define VGMEMP_FREE(h,a) VALGRIND_MEMPOOL_FREE(h,a)
#define VGMEMP_DESTROY(h)	VALGRIND_DESTROY_MEMPOOL(h)
#define VGMEMP_DEFINED(a,s)	VALGRIND_MAKE_MEM_DEFINED(a,s)
#else
#define VGMEMP_CREATE(h,r,z)
#define VGMEMP_ALLOC(h,a,s)
#define VGMEMP_FREE(h,a)
#define VGMEMP_DESTROY(h)
#define VGMEMP_DEFINED(a,s)
#endif

#ifndef BYTE_ORDER
# if (defined(_LITTLE_ENDIAN) || defined(_BIG_ENDIAN)) && !(defined(_LITTLE_ENDIAN) && defined(_BIG_ENDIAN))
/* Solaris just defines one or the other */
#  define LITTLE_ENDIAN	1234
#  define BIG_ENDIAN	4321
#  ifdef _LITTLE_ENDIAN
#   define BYTE_ORDER  LITTLE_ENDIAN
#  else
#   define BYTE_ORDER  BIG_ENDIAN
#  endif
# else
#  define BYTE_ORDER   __BYTE_ORDER
# endif
#endif

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN	__LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN	__BIG_ENDIAN
#endif

#if defined(__i386) || defined(__x86_64) || defined(_M_IX86)
#define MISALIGNED_OK	1
#endif

#include "dlmdb.h"
#include "midl.h"

#if (BYTE_ORDER == LITTLE_ENDIAN) == (BYTE_ORDER == BIG_ENDIAN)
# error "Unknown or unsupported endianness (BYTE_ORDER)"
#elif (-6 & 5) || CHAR_BIT!=8 || UINT_MAX!=0xffffffff || MDB_SIZE_MAX%UINT_MAX
# error "Two's complement, reasonably sized integer types, please"
#endif

#if (((__clang_major__ << 8) | __clang_minor__) >= 0x0302) || (((__GNUC__ << 8) | __GNUC_MINOR__) >= 0x0403)
/** Mark infrequently used env functions as cold. This puts them in a separate
 *  section, and optimizes them for size */
#define ESECT __attribute__ ((cold))
#else
/* On older compilers, use a separate section */
# ifdef __GNUC__
#  ifdef __APPLE__
#   define      ESECT   __attribute__ ((section("__TEXT,text_env")))
#  else
#   define      ESECT   __attribute__ ((section("text_env")))
#  endif
# else
#  define ESECT
# endif
#endif

#ifdef _WIN32
#define CALL_CONV WINAPI
#else
#define CALL_CONV
#endif

#ifdef __GNUC__
#define MDB_ALIGNED(n) __attribute__((aligned(n)))
#else
#define MDB_ALIGNED(n)
#endif

/** @defgroup internal	LMDB Internals
 *	@{
 */
/** @defgroup compat	Compatibility Macros
 *	A bunch of macros to minimize the amount of platform-specific ifdefs
 *	needed throughout the rest of the code. When the features this library
 *	needs are similar enough to POSIX to be hidden in a one-or-two line
 *	replacement, this macro approach is used.
 *	@{
 */

	/** Features under development */
#ifndef MDB_DEVEL
#define MDB_DEVEL 0
#endif

	/** Wrapper around __func__, which is a C99 feature */
#if __STDC_VERSION__ >= 199901L
# define mdb_func_	__func__
#elif __GNUC__ >= 2 || _MSC_VER >= 1300
# define mdb_func_	__FUNCTION__
#else
/* If a debug message says <mdb_unknown>(), update the #if statements above */
# define mdb_func_	"<mdb_unknown>"
#endif

/* Internal error codes, not exposed outside liblmdb */
#define	MDB_NO_ROOT		(MDB_LAST_ERRCODE + 10)
#ifdef _WIN32
#define MDB_OWNERDEAD	((int) WAIT_ABANDONED)
#elif defined MDB_USE_SYSV_SEM
#define MDB_OWNERDEAD	(MDB_LAST_ERRCODE + 11)
#elif defined(MDB_USE_POSIX_MUTEX) && defined(EOWNERDEAD)
#define MDB_OWNERDEAD	EOWNERDEAD	/**< #LOCK_MUTEX0() result if dead owner */
#endif

#ifdef __GLIBC__
#define	GLIBC_VER	((__GLIBC__ << 16 )| __GLIBC_MINOR__)
#endif
/** Some platforms define the EOWNERDEAD error code
 * even though they don't support Robust Mutexes.
 * Compile with -DMDB_USE_ROBUST=0, or use some other
 * mechanism like -DMDB_USE_SYSV_SEM instead of
 * -DMDB_USE_POSIX_MUTEX. (SysV semaphores are
 * also Robust, but some systems don't support them
 * either.)
 */
#ifndef MDB_USE_ROBUST
/* Android currently lacks Robust Mutex support. So does glibc < 2.4. */
# if defined(MDB_USE_POSIX_MUTEX) && (defined(__ANDROID__) || \
	(defined(__GLIBC__) && GLIBC_VER < 0x020004))
#  define MDB_USE_ROBUST	0
# else
#  define MDB_USE_ROBUST	1
# endif
#endif /* !MDB_USE_ROBUST */

#if defined(MDB_USE_POSIX_MUTEX) && (MDB_USE_ROBUST)
/* glibc < 2.12 only provided _np API */
#  if (defined(__GLIBC__) && GLIBC_VER < 0x02000c) || \
	(defined(PTHREAD_MUTEX_ROBUST_NP) && !defined(PTHREAD_MUTEX_ROBUST))
#   define PTHREAD_MUTEX_ROBUST	PTHREAD_MUTEX_ROBUST_NP
#   define pthread_mutexattr_setrobust(attr, flag)	pthread_mutexattr_setrobust_np(attr, flag)
#   define pthread_mutex_consistent(mutex)	pthread_mutex_consistent_np(mutex)
#  endif
#endif /* MDB_USE_POSIX_MUTEX && MDB_USE_ROBUST */

#if defined(MDB_OWNERDEAD) && (MDB_USE_ROBUST)
#define MDB_ROBUST_SUPPORTED	1
#endif

#ifdef _WIN32
#define MDB_USE_HASH	1
#define MDB_PIDLOCK	0
#define THREAD_RET	DWORD
#define pthread_t	HANDLE
#define pthread_mutex_t	HANDLE
#define pthread_cond_t	HANDLE
typedef HANDLE mdb_mutex_t, mdb_mutexref_t;
#define pthread_key_t	DWORD
#define pthread_self()	GetCurrentThreadId()
#define pthread_key_create(x,y)	\
	((*(x) = TlsAlloc()) == TLS_OUT_OF_INDEXES ? ErrCode() : 0)
#define pthread_key_delete(x)	TlsFree(x)
#define pthread_getspecific(x)	TlsGetValue(x)
#define pthread_setspecific(x,y)	(TlsSetValue(x,y) ? 0 : ErrCode())
#define pthread_mutex_unlock(x)	ReleaseMutex(*x)
#define pthread_mutex_lock(x)	WaitForSingleObject(*x, INFINITE)
#define pthread_cond_signal(x)	SetEvent(*x)
#define pthread_cond_wait(cond,mutex)	do{SignalObjectAndWait(*mutex, *cond, INFINITE, FALSE); WaitForSingleObject(*mutex, INFINITE);}while(0)
#define THREAD_CREATE(thr,start,arg) \
	(((thr) = CreateThread(NULL, 0, start, arg, 0, NULL)) ? 0 : ErrCode())
#define THREAD_FINISH(thr) \
	(WaitForSingleObject(thr, INFINITE) ? ErrCode() : 0)
#define LOCK_MUTEX0(mutex)		WaitForSingleObject(mutex, INFINITE)
#define UNLOCK_MUTEX(mutex)		ReleaseMutex(mutex)
#define mdb_mutex_consistent(mutex)	0
#define getpid()	GetCurrentProcessId()
#define	MDB_FDATASYNC(fd)	(!FlushFileBuffers(fd))
#define	MDB_MSYNC(addr,len,flags)	(!FlushViewOfFile(addr,len))
#define	ErrCode()	GetLastError()
#define GET_PAGESIZE(x) {SYSTEM_INFO si; GetSystemInfo(&si); (x) = si.dwPageSize;}
#define	close(fd)	(CloseHandle(fd) ? 0 : -1)
#define	munmap(ptr,len)	UnmapViewOfFile(ptr)
#ifdef PROCESS_QUERY_LIMITED_INFORMATION
#define MDB_PROCESS_QUERY_LIMITED_INFORMATION PROCESS_QUERY_LIMITED_INFORMATION
#else
#define MDB_PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#else
#define THREAD_RET	void *
#define THREAD_CREATE(thr,start,arg)	pthread_create(&thr,NULL,start,arg)
#define THREAD_FINISH(thr)	pthread_join(thr,NULL)

	/** For MDB_LOCK_FORMAT: True if readers take a pid lock in the lockfile */
#define MDB_PIDLOCK			1

#ifdef MDB_USE_POSIX_SEM

typedef sem_t *mdb_mutex_t, *mdb_mutexref_t;
#define LOCK_MUTEX0(mutex)		mdb_sem_wait(mutex)
#define UNLOCK_MUTEX(mutex)		sem_post(mutex)

static int
mdb_sem_wait(sem_t *sem)
{
   int rc;
   while ((rc = sem_wait(sem)) && (rc = errno) == EINTR) ;
   return rc;
}

#elif defined MDB_USE_SYSV_SEM

typedef struct mdb_mutex {
	int semid;
	int semnum;
	int *locked;
	pthread_mutex_t pmutex;
} mdb_mutex_t[1], *mdb_mutexref_t;

#define LOCK_MUTEX0(mutex)		mdb_sem_wait(mutex)
#define UNLOCK_MUTEX(mutex)		mdb_sem_post(mutex)

static int
mdb_sem_wait(mdb_mutexref_t sem)
{
	if (sem->semid == -1)
		return pthread_mutex_lock(&sem->pmutex);

	int rc, *locked = sem->locked;
	struct sembuf sb = { 0, -1, SEM_UNDO };
	sb.sem_num = sem->semnum;
	do {
		if (!semop(sem->semid, &sb, 1)) {
			rc = *locked ? MDB_OWNERDEAD : MDB_SUCCESS;
			*locked = 1;
			break;
		}
	} while ((rc = errno) == EINTR);
	return rc;
}

static int
mdb_sem_post(mdb_mutexref_t sem)
{
	if (sem->semid == -1)
		return pthread_mutex_unlock(&sem->pmutex);

	{
		struct sembuf sb = { 0, 1, SEM_UNDO };
		int rc;
		sb.sem_num = sem->semnum;
		*sem->locked = 0;
		do {
			rc = semop(sem->semid, &sb, 1);
		} while (rc && errno == EINTR);
		return rc;
	}
}

#define mdb_mutex_consistent(mutex)	0

#else	/* MDB_USE_POSIX_MUTEX: */
	/** Shared mutex/semaphore as the original is stored.
	 *
	 *	Not for copies.  Instead it can be assigned to an #mdb_mutexref_t.
	 *	When mdb_mutexref_t is a pointer and mdb_mutex_t is not, then it
	 *	is array[size 1] so it can be assigned to the pointer.
	 */
typedef pthread_mutex_t mdb_mutex_t[1];
	/** Reference to an #mdb_mutex_t */
typedef pthread_mutex_t *mdb_mutexref_t;
	/** Lock the reader or writer mutex.
	 *	Returns 0 or a code to give #mdb_mutex_failed(), as in #LOCK_MUTEX().
	 */
#define LOCK_MUTEX0(mutex)	pthread_mutex_lock(mutex)
	/** Unlock the reader or writer mutex.
	 */
#define UNLOCK_MUTEX(mutex)	pthread_mutex_unlock(mutex)
	/** Mark mutex-protected data as repaired, after death of previous owner.
	 */
#define mdb_mutex_consistent(mutex)	pthread_mutex_consistent(mutex)
#endif	/* MDB_USE_POSIX_SEM || MDB_USE_SYSV_SEM */

	/** Get the error code for the last failed system function.
	 */
#define	ErrCode()	errno

	/** An abstraction for a file handle.
	 *	On POSIX systems file handles are small integers. On Windows
	 *	they're opaque pointers.
	 */
#define	HANDLE	int

	/**	A value for an invalid file handle.
	 *	Mainly used to initialize file variables and signify that they are
	 *	unused.
	 */
#define INVALID_HANDLE_VALUE	(-1)

	/** Get the size of a memory page for the system.
	 *	This is the basic size that the platform's memory manager uses, and is
	 *	fundamental to the use of memory-mapped files.
	 */
#define	GET_PAGESIZE(x)	((x) = sysconf(_SC_PAGE_SIZE))
#endif

#ifndef _WIN32
# ifdef MAP_ANONYMOUS
#  define MDB_MAP_ANON	MAP_ANONYMOUS
# elif defined(MAP_ANON)
#  define MDB_MAP_ANON	MAP_ANON
# endif
#endif

#define	Z	MDB_FMT_Z	/**< printf/scanf format modifier for size_t */
#define	Yu	MDB_PRIy(u)	/**< printf format for #mdb_size_t */
#define	Yd	MDB_PRIy(d)	/**< printf format for 'signed #mdb_size_t' */

#ifdef MDB_USE_SYSV_SEM
#define MNAME_LEN	(sizeof(int))
#else
#define MNAME_LEN	(sizeof(pthread_mutex_t))
#endif

/** Initial part of #MDB_env.me_mutexname[].
 *	Changes to this code must be reflected in #MDB_LOCK_FORMAT.
 */
#ifdef _WIN32
#define MUTEXNAME_PREFIX		"Global\\MDB"
#elif defined MDB_USE_POSIX_SEM
#define MUTEXNAME_PREFIX		"/MDB"
#endif

/** @} */

#ifdef MDB_ROBUST_SUPPORTED
	/** Lock mutex, handle any error, set rc = result.
	 *	Return 0 on success, nonzero (not rc) on error.
	 */
#define LOCK_MUTEX(rc, env, mutex) \
	(((rc) = LOCK_MUTEX0(mutex)) && \
	 ((rc) = mdb_mutex_failed(env, mutex, rc)))
static int mdb_mutex_failed(MDB_env *env, mdb_mutexref_t mutex, int rc);
#else
#define LOCK_MUTEX(rc, env, mutex) ((rc) = LOCK_MUTEX0(mutex))
#define mdb_mutex_failed(env, mutex, rc) (rc)
#endif

#ifndef _WIN32
/**	A flag for opening a file and requesting synchronous data writes.
 *	This is only used when writing a meta page. It's not strictly needed;
 *	we could just do a normal write and then immediately perform a flush.
 *	But if this flag is available it saves us an extra system call.
 *
 *	@note If O_DSYNC is undefined but exists in /usr/include,
 * preferably set some compiler flag to get the definition.
 */
#ifndef MDB_DSYNC
# ifdef O_DSYNC
# define MDB_DSYNC	O_DSYNC
# else
# define MDB_DSYNC	O_SYNC
# endif
#endif
#endif

/** Function for flushing the data of a file. Define this to fsync
 *	if fdatasync() is not supported.
 */
#ifndef MDB_FDATASYNC
# define MDB_FDATASYNC	fdatasync
#endif

#ifndef MDB_MSYNC
# define MDB_MSYNC(addr,len,flags)	msync(addr,len,flags)
#endif

#ifndef MS_SYNC
#define	MS_SYNC	1
#endif

#ifndef MS_ASYNC
#define	MS_ASYNC	0
#endif

	/** A page number in the database.
	 *	Note that 64 bit page numbers are overkill, since pages themselves
	 *	already represent 12-13 bits of addressable memory, and the OS will
	 *	always limit applications to a maximum of 63 bits of address space.
	 *
	 *	@note In the #MDB_node structure, we only store 48 bits of this value,
	 *	which thus limits us to only 60 bits of addressable data.
	 */
typedef MDB_ID	pgno_t;

	/** A transaction ID.
	 *	See struct MDB_txn.mt_txnid for details.
	 */
typedef MDB_ID	txnid_t;

/** @defgroup debug	Debug Macros
 *	@{
 */
#ifndef MDB_DEBUG
	/**	Enable debug output.  Needs variable argument macros (a C99 feature).
	 *	Set this to 1 for copious tracing. Set to 2 to add dumps of all IDLs
	 *	read from and written to the database (used for free space management).
	 */
#define MDB_DEBUG 0
#endif

#define MDB_DBG_INFO	1
#define MDB_DBG_TRACE	2

#if MDB_DEBUG
static int mdb_debug = MDB_DBG_TRACE;
static txnid_t mdb_debug_start;

	/**	Print a debug message with printf formatting.
	 *	Requires double parenthesis around 2 or more args.
	 */
# define DPRINTF(args) ((void) ((mdb_debug & MDB_DBG_INFO) && DPRINTF0 args))
# define DPRINTF0(fmt, ...) \
	fprintf(stderr, "%s:%d " fmt "\n", mdb_func_, __LINE__, __VA_ARGS__)
	/** Trace info for replaying */
# define MDB_TRACE(args)	((void) ((mdb_debug & MDB_DBG_TRACE) && DPRINTF1 args))
# define DPRINTF1(fmt, ...) \
	fprintf(stderr, ">%d:%s: " fmt "\n", getpid(), mdb_func_, __VA_ARGS__)
#else
# define DPRINTF(args)	((void) 0)
# define MDB_TRACE(args)	((void) 0)
#endif
	/**	Print a debug string.
	 *	The string is printed literally, with no format processing.
	 */
#define DPUTS(arg)	DPRINTF(("%s", arg))
	/** Debugging output value of a cursor DBI: Negative in a sub-cursor. */
#define DDBI(mc) \
	(((mc)->mc_flags & C_SUB) ? -(int)(mc)->mc_dbi : (int)(mc)->mc_dbi)
/** @} */

	/**	@brief The maximum size of a database page.
	 *
	 *	It is 32k or 64k, since value-PAGEBASE must fit in
	 *	#MDB_page.%mp_upper.
	 *
	 *	LMDB will use database pages < OS pages if needed.
	 *	That causes more I/O in write transactions: The OS must
	 *	know (read) the whole page before writing a partial page.
	 *
	 *	Note that we don't currently support Huge pages. On Linux,
	 *	regular data files cannot use Huge pages, and in general
	 *	Huge pages aren't actually pageable. We rely on the OS
	 *	demand-pager to read our data and page it out when memory
	 *	pressure from other processes is high. So until OSs have
	 *	actual paging support for Huge pages, they're not viable.
	 */
#define MAX_PAGESIZE	 (PAGEBASE ? 0x10000 : 0x8000)

	/** The minimum number of keys required in a database page.
	 *	Setting this to a larger value will place a smaller bound on the
	 *	maximum size of a data item. Data items larger than this size will
	 *	be pushed into overflow pages instead of being stored directly in
	 *	the B-tree node. This value used to default to 4. With a page size
	 *	of 4096 bytes that meant that any item larger than 1024 bytes would
	 *	go into an overflow page. That also meant that on average 2-3KB of
	 *	each overflow page was wasted space. The value cannot be lower than
	 *	2 because then there would no longer be a tree structure. With this
	 *	value, items larger than 2KB will go into overflow pages, and on
	 *	average only 1KB will be wasted.
	 */
#define MDB_MINKEYS	 2

	/**	A stamp that identifies a file as an LMDB file.
	 *	There's nothing special about this value other than that it is easily
	 *	recognizable, and it will reflect any byte order mismatches.
	 */
#define MDB_MAGIC	 0xBEEFC0DE

	/**	The version number for a database's datafile format. */
#define MDB_DATA_VERSION	 ((MDB_DEVEL) ? 999 : 1)
	/**	The version number for a database's lockfile format. */
#define MDB_LOCK_VERSION	 ((MDB_DEVEL) ? 999 : 2)
	/** Number of bits representing #MDB_LOCK_VERSION in #MDB_LOCK_FORMAT.
	 *	The remaining bits must leave room for #MDB_lock_desc.
	 */
#define MDB_LOCK_VERSION_BITS 12

	/**	@brief The max size of a key we can write, or 0 for computed max.
	 *
	 *	This macro should normally be left alone or set to 0.
	 *	Note that a database with big keys or dupsort data cannot be
	 *	reliably modified by a liblmdb which uses a smaller max.
	 *	The default is 511 for backwards compat, or 0 when #MDB_DEVEL.
	 *
	 *	Other values are allowed, for backwards compat.  However:
	 *	A value bigger than the computed max can break if you do not
	 *	know what you are doing, and liblmdb <= 0.9.10 can break when
	 *	modifying a DB with keys/dupsort data bigger than its max.
	 *
	 *	Data items in an #MDB_DUPSORT database are also limited to
	 *	this size, since they're actually keys of a sub-DB.  Keys and
	 *	#MDB_DUPSORT data items must fit on a node in a regular page.
	 */
#ifndef MDB_MAXKEYSIZE
#define MDB_MAXKEYSIZE	 ((MDB_DEVEL) ? 0 : 511)
#endif

	/**	The maximum size of a key we can write to the environment. */
#if MDB_MAXKEYSIZE
#define ENV_MAXKEY(env)	(MDB_MAXKEYSIZE)
#else
#define ENV_MAXKEY(env)	((env)->me_maxkey)
#endif

	/** Scratch space reserved per cursor for decompressed keys. */
#if MDB_MAXKEYSIZE
#define MDB_KEYBUF_MAX	(MDB_MAXKEYSIZE)
#else
#define MDB_KEYBUF_MAX	511
#endif

	/**	@brief The maximum size of a data item.
	 *
	 *	We only store a 32 bit value for node sizes.
	 */
#define MAXDATASIZE	0xffffffffUL

#if MDB_DEBUG
	/**	Key size which fits in a #DKBUF.
	 *	@ingroup debug
	 */
#define DKBUF_MAXKEYSIZE ((MDB_MAXKEYSIZE) > 0 ? (MDB_MAXKEYSIZE) : 511)
	/**	A key buffer.
	 *	@ingroup debug
	 *	This is used for printing a hex dump of a key's contents.
	 */
#define DKBUF	char kbuf[DKBUF_MAXKEYSIZE*2+1]
	/**	A data value buffer.
	 *	@ingroup debug
	 *	This is used for printing a hex dump of a #MDB_DUPSORT value's contents.
	 */
#define DDBUF	char dbuf[DKBUF_MAXKEYSIZE*2+1+2]
	/**	Display a key in hex.
	 *	@ingroup debug
	 *	Invoke a function to display a key in hex.
	 */
#define	DKEY(x)	mdb_dkey(x, kbuf)
#else
#define	DKBUF
#define	DDBUF
#define DKEY(x)	0
#endif

	/** An invalid page number.
	 *	Mainly used to denote an empty tree.
	 */
#define P_INVALID	 (~(pgno_t)0)

	/** Test if the flags \b f are set in a flag word \b w. */
#define F_ISSET(w, f)	 (((w) & (f)) == (f))

	/** Round \b n up to an even number. */
#define EVEN(n)		(((n) + 1U) & -2) /* sign-extending -2 to match n+1U */

	/** Least significant 1-bit of \b n.  n must be of an unsigned type. */
#define LOW_BIT(n)		((n) & (-(n)))

	/** (log2(\b p2) % \b n), for p2 = power of 2 and 0 < n < 8. */
#define LOG2_MOD(p2, n)	(7 - 86 / ((p2) % ((1U<<(n))-1) + 11))
	/* Explanation: Let p2 = 2**(n*y + x), x<n and M = (1U<<n)-1. Now p2 =
	 * (M+1)**y * 2**x = 2**x (mod M). Finally "/" "happens" to return 7-x.
	 */

	/** Should be alignment of \b type. Ensure it is a power of 2. */
#if defined(_MSC_VER)
#define ALIGNOF2(type) __alignof(type)
#else
#define ALIGNOF2(type) \
	LOW_BIT(offsetof(struct { char ch_; type align_; }, align_))
#endif

	/**	Used for offsets within a single page.
	 *	Since memory pages are typically 4 or 8KB in size, 12-13 bits,
	 *	this is plenty.
	 */
typedef uint16_t	 indx_t;

typedef unsigned long long	mdb_hash_t;

	/**	Default size of memory map.
	 *	This is certainly too small for any actual applications. Apps should always set
	 *	the size explicitly using #mdb_env_set_mapsize().
	 */
#define DEFAULT_MAPSIZE	1048576

/**	@defgroup readers	Reader Lock Table
 *	Readers don't acquire any locks for their data access. Instead, they
 *	simply record their transaction ID in the reader table. The reader
 *	mutex is needed just to find an empty slot in the reader table. The
 *	slot's address is saved in thread-specific data so that subsequent read
 *	transactions started by the same thread need no further locking to proceed.
 *
 *	If #MDB_NOTLS is set, the slot address is not saved in thread-specific data.
 *
 *	No reader table is used if the database is on a read-only filesystem, or
 *	if #MDB_NOLOCK is set.
 *
 *	Since the database uses multi-version concurrency control, readers don't
 *	actually need any locking. This table is used to keep track of which
 *	readers are using data from which old transactions, so that we'll know
 *	when a particular old transaction is no longer in use. Old transactions
 *	that have discarded any data pages can then have those pages reclaimed
 *	for use by a later write transaction.
 *
 *	The lock table is constructed such that reader slots are aligned with the
 *	processor's cache line size. Any slot is only ever used by one thread.
 *	This alignment guarantees that there will be no contention or cache
 *	thrashing as threads update their own slot info, and also eliminates
 *	any need for locking when accessing a slot.
 *
 *	A writer thread will scan every slot in the table to determine the oldest
 *	outstanding reader transaction. Any freed pages older than this will be
 *	reclaimed by the writer. The writer doesn't use any locks when scanning
 *	this table. This means that there's no guarantee that the writer will
 *	see the most up-to-date reader info, but that's not required for correct
 *	operation - all we need is to know the upper bound on the oldest reader,
 *	we don't care at all about the newest reader. So the only consequence of
 *	reading stale information here is that old pages might hang around a
 *	while longer before being reclaimed. That's actually good anyway, because
 *	the longer we delay reclaiming old pages, the more likely it is that a
 *	string of contiguous pages can be found after coalescing old pages from
 *	many old transactions together.
 *	@{
 */
	/**	Number of slots in the reader table.
	 *	This value was chosen somewhat arbitrarily. 126 readers plus a
	 *	couple mutexes fit exactly into 8KB on my development machine.
	 *	Applications should set the table size using #mdb_env_set_maxreaders().
	 */
#define DEFAULT_READERS	126

	/**	The size of a CPU cache line in bytes. We want our lock structures
	 *	aligned to this size to avoid false cache line sharing in the
	 *	lock table.
	 *	This value works for most CPUs. For Itanium this should be 128.
	 */
#ifndef CACHELINE
#define CACHELINE	64
#endif

	/**	The information we store in a single slot of the reader table.
	 *	In addition to a transaction ID, we also record the process and
	 *	thread ID that owns a slot, so that we can detect stale information,
	 *	e.g. threads or processes that went away without cleaning up.
	 *	@note We currently don't check for stale records. We simply re-init
	 *	the table when we know that we're the only process opening the
	 *	lock file.
	 */
typedef struct MDB_rxbody {
	/**	Current Transaction ID when this transaction began, or (txnid_t)-1.
	 *	Multiple readers that start at the same time will probably have the
	 *	same ID here. Again, it's not important to exclude them from
	 *	anything; all we need to know is which version of the DB they
	 *	started from so we can avoid overwriting any data used in that
	 *	particular version.
	 */
	volatile txnid_t		mrb_txnid;
	/** The process ID of the process owning this reader txn. */
	volatile MDB_PID_T	mrb_pid;
	/** The thread ID of the thread owning this txn. */
	volatile MDB_THR_T	mrb_tid;
} MDB_rxbody;

	/** The actual reader record, with cacheline padding. */
typedef struct MDB_reader {
	union {
		MDB_rxbody mrx;
		/** shorthand for mrb_txnid */
#define	mr_txnid	mru.mrx.mrb_txnid
#define	mr_pid	mru.mrx.mrb_pid
#define	mr_tid	mru.mrx.mrb_tid
		/** cache line alignment */
		char pad[(sizeof(MDB_rxbody)+CACHELINE-1) & ~(CACHELINE-1)];
	} mru;
} MDB_reader;

	/** The header for the reader table.
	 *	The table resides in a memory-mapped file. (This is a different file
	 *	than is used for the main database.)
	 *
	 *	For POSIX the actual mutexes reside in the shared memory of this
	 *	mapped file. On Windows, mutexes are named objects allocated by the
	 *	kernel; we store the mutex names in this mapped file so that other
	 *	processes can grab them. This same approach is also used on
	 *	MacOSX/Darwin (using named semaphores) since MacOSX doesn't support
	 *	process-shared POSIX mutexes. For these cases where a named object
	 *	is used, the object name is derived from a 64 bit FNV hash of the
	 *	environment pathname. As such, naming collisions are extremely
	 *	unlikely. If a collision occurs, the results are unpredictable.
	 */
typedef struct MDB_txbody {
		/** Stamp identifying this as an LMDB file. It must be set
		 *	to #MDB_MAGIC. */
	uint32_t	mtb_magic;
		/** Format of this lock file. Must be set to #MDB_LOCK_FORMAT. */
	uint32_t	mtb_format;
		/**	The ID of the last transaction committed to the database.
		 *	This is recorded here only for convenience; the value can always
		 *	be determined by reading the main database meta pages.
		 */
	volatile txnid_t		mtb_txnid;
		/** The number of slots that have been used in the reader table.
		 *	This always records the maximum count, it is not decremented
		 *	when readers release their slots.
		 */
	volatile unsigned	mtb_numreaders;
#if defined(_WIN32) || defined(MDB_USE_POSIX_SEM)
		/** Binary form of names of the reader/writer locks */
	mdb_hash_t			mtb_mutexid;
#elif defined(MDB_USE_SYSV_SEM)
	int 	mtb_semid;
	int		mtb_rlocked;
#else
		/** Mutex protecting access to this table.
		 *	This is the reader table lock used with LOCK_MUTEX().
		 */
	mdb_mutex_t	mtb_rmutex;
#endif
} MDB_txbody;

	/** The actual reader table definition. */
typedef struct MDB_txninfo {
	union {
		MDB_txbody mtb;
#define mti_magic	mt1.mtb.mtb_magic
#define mti_format	mt1.mtb.mtb_format
#define mti_rmutex	mt1.mtb.mtb_rmutex
#define mti_txnid	mt1.mtb.mtb_txnid
#define mti_numreaders	mt1.mtb.mtb_numreaders
#define mti_mutexid	mt1.mtb.mtb_mutexid
#ifdef MDB_USE_SYSV_SEM
#define	mti_semid	mt1.mtb.mtb_semid
#define	mti_rlocked	mt1.mtb.mtb_rlocked
#endif
		char pad[(sizeof(MDB_txbody)+CACHELINE-1) & ~(CACHELINE-1)];
	} mt1;
#if !(defined(_WIN32) || defined(MDB_USE_POSIX_SEM))
	union {
#ifdef MDB_USE_SYSV_SEM
		int mt2_wlocked;
#define mti_wlocked	mt2.mt2_wlocked
#else
		mdb_mutex_t	mt2_wmutex;
#define mti_wmutex	mt2.mt2_wmutex
#endif
		char pad[(MNAME_LEN+CACHELINE-1) & ~(CACHELINE-1)];
	} mt2;
#endif
	MDB_reader	mti_readers[1];
} MDB_txninfo;

	/** Lockfile format signature: version, features and field layout */
#define MDB_LOCK_FORMAT \
	((uint32_t)         \
	 (((MDB_LOCK_VERSION) % (1U << MDB_LOCK_VERSION_BITS)) \
	  + MDB_lock_desc     * (1U << MDB_LOCK_VERSION_BITS)))

	/** Lock type and layout. Values 0-119. _WIN32 implies #MDB_PIDLOCK.
	 *	Some low values are reserved for future tweaks.
	 */
#ifdef _WIN32
# define MDB_LOCK_TYPE	(0 + ALIGNOF2(mdb_hash_t)/8 % 2)
#elif defined MDB_USE_POSIX_SEM
# define MDB_LOCK_TYPE	(4 + ALIGNOF2(mdb_hash_t)/8 % 2)
#elif defined MDB_USE_SYSV_SEM
# define MDB_LOCK_TYPE	(8)
#elif defined MDB_USE_POSIX_MUTEX
/* We do not know the inside of a POSIX mutex and how to check if mutexes
 * used by two executables are compatible. Just check alignment and size.
 */
# define MDB_LOCK_TYPE	(10 + \
		LOG2_MOD(ALIGNOF2(pthread_mutex_t), 5) + \
		sizeof(pthread_mutex_t) / 4U % 22 * 5)
#endif

enum {
	/** Magic number for lockfile layout and features.
	 *
	 *  This *attempts* to stop liblmdb variants compiled with conflicting
	 *	options from using the lockfile at the same time and thus breaking
	 *	it.  It describes locking types, and sizes and sometimes alignment
	 *	of the various lockfile items.
	 *
	 *	The detected ranges are mostly guesswork, or based simply on how
	 *	big they could be without using more bits.  So we can tweak them
	 *	in good conscience when updating #MDB_LOCK_VERSION.
	 */
	MDB_lock_desc =
	/* Default CACHELINE=64 vs. other values (have seen mention of 32-256) */
	(CACHELINE==64 ? 0 : 1 + LOG2_MOD(CACHELINE >> (CACHELINE>64), 5))
	+ 6  * (sizeof(MDB_PID_T)/4 % 3)    /* legacy(2) to word(4/8)? */
	+ 18 * (sizeof(pthread_t)/4 % 5)    /* can be struct{id, active data} */
	+ 90 * (sizeof(MDB_txbody) / CACHELINE % 3)
	+ 270 * (MDB_LOCK_TYPE % 120)
	/* The above is < 270*120 < 2**15 */
	+ ((sizeof(txnid_t) == 8) << 15)    /* 32bit/64bit */
	+ ((sizeof(MDB_reader) > CACHELINE) << 16)
	/* Not really needed - implied by MDB_LOCK_TYPE != (_WIN32 locking) */
	+ (((MDB_PIDLOCK) != 0)   << 17)
	/* 18 bits total: Must be <= (32 - MDB_LOCK_VERSION_BITS). */
};
/** @} */

/** Common header for all page types. The page type depends on #mp_flags.
 *
 * #P_BRANCH and #P_LEAF pages have unsorted '#MDB_node's at the end, with
 * sorted #mp_ptrs[] entries referring to them. Exception: #P_LEAF2 pages
 * omit mp_ptrs and pack sorted #MDB_DUPFIXED values after the page header.
 *
 * #P_OVERFLOW records occupy one or more contiguous pages where only the
 * first has a page header. They hold the real data of #F_BIGDATA nodes.
 *
 * #P_SUBP sub-pages are small leaf "pages" with duplicate data.
 * A node with flag #F_DUPDATA but not #F_SUBDATA contains a sub-page.
 * (Duplicate data can also go in sub-databases, which use normal pages.)
 *
 * #P_META pages contain #MDB_meta, the start point of an LMDB snapshot.
 *
 * Each non-metapage up to #MDB_meta.%mm_last_pg is reachable exactly once
 * in the snapshot: Either used by a database or listed in a freeDB record.
 */
typedef struct MDB_page {
#define	mp_pgno	mp_p.p_pgno
#define	mp_next	mp_p.p_next
	union {
		pgno_t		p_pgno;	/**< page number */
		struct MDB_page *p_next; /**< for in-memory list of freed pages */
	} mp_p;
	uint16_t	mp_pad;			/**< key size if this is a LEAF2 page */
/**	@defgroup mdb_page	Page Flags
 *	@ingroup internal
 *	Flags for the page headers.
 *	@{
 */
#define	P_BRANCH	 0x01		/**< branch page */
#define	P_LEAF		 0x02		/**< leaf page */
#define	P_OVERFLOW	 0x04		/**< overflow page */
#define	P_META		 0x08		/**< meta page */
#define	P_DIRTY		 0x10		/**< dirty page, also set for #P_SUBP pages */
#define	P_LEAF2		 0x20		/**< for #MDB_DUPFIXED records */
#define	P_SUBP		 0x40		/**< for #MDB_DUPSORT sub-pages */
#define	P_COUNTED	 0x80		/**< branch page stores subtree counts */
#define	P_LOOSE		 0x4000		/**< page was dirtied then freed, can be reused */
#define	P_KEEP		 0x8000		/**< leave this page alone during spill */
#define MDB_COUNT_ALLOWED_FLAGS \
	(MDB_COUNT_LOWER_INCL|MDB_COUNT_UPPER_INCL)
/** @} */
	uint16_t	mp_flags;		/**< @ref mdb_page */
#define mp_lower	mp_pb.pb.pb_lower
#define mp_upper	mp_pb.pb.pb_upper
#define mp_pages	mp_pb.pb_pages
	union {
		struct {
			indx_t		pb_lower;		/**< lower bound of free space */
			indx_t		pb_upper;		/**< upper bound of free space */
		} pb;
		uint32_t	pb_pages;	/**< number of overflow pages */
	} mp_pb;
	indx_t		mp_ptrs[0];		/**< dynamic size */
} MDB_page MDB_ALIGNED(1);

/** Alternate page header, for 2-byte aligned access */
typedef struct MDB_page2 {
	uint16_t	mp2_p[sizeof(pgno_t)/2];
	uint16_t	mp2_pad;
	uint16_t	mp2_flags;
	indx_t		mp2_lower;
	indx_t		mp2_upper;
	indx_t		mp2_ptrs[0];
} MDB_page2;

static pgno_t
mdb_page_pgno_get(const MDB_page *mp)
{
#ifdef MISALIGNED_OK
	return mp->mp_pgno;
#else
	pgno_t pgno;
	memcpy(&pgno, &mp->mp_pgno, sizeof(pgno));
	return pgno;
#endif
}

static void
mdb_page_pgno_set(MDB_page *mp, pgno_t pgno)
{
#ifdef MISALIGNED_OK
	mp->mp_pgno = pgno;
#else
	memcpy(&mp->mp_pgno, &pgno, sizeof(pgno));
#endif
}

#define MP_PGNO(p)	mdb_page_pgno_get((const MDB_page *)(p))
#define MP_SETPGNO(p,v)	mdb_page_pgno_set((MDB_page *)(p), (v))
#define MP_PAD(p)	(((MDB_page2 *)(void *)(p))->mp2_pad)
#define MP_FLAGS(p)	(((MDB_page2 *)(void *)(p))->mp2_flags)
#define MP_LOWER(p)	(((MDB_page2 *)(void *)(p))->mp2_lower)
#define MP_UPPER(p)	(((MDB_page2 *)(void *)(p))->mp2_upper)
#define MP_PTRS(p)	(((MDB_page2 *)(void *)(p))->mp2_ptrs)

	/** Size of the page header, excluding dynamic data at the end */
#define PAGEHDRSZ	 ((unsigned) offsetof(MDB_page, mp_ptrs))

	/** Address of first usable data byte in a page, after the header */
#define METADATA(p)	 ((void *)((char *)(p) + PAGEHDRSZ))

	/** ITS#7713, change PAGEBASE to handle 65536 byte pages */
#define	PAGEBASE	((MDB_DEVEL) ? PAGEHDRSZ : 0)

	/** Number of nodes on a page */
#define NUMKEYS(p)	 ((MP_LOWER(p) - (PAGEHDRSZ-PAGEBASE)) >> 1)

	/** The amount of space remaining in the page */
#define SIZELEFT(p)	 (indx_t)(MP_UPPER(p) - MP_LOWER(p))

	/** The percentage of space used in the page, in tenths of a percent. */
#define PAGEFILL(env, p) (1000L * ((env)->me_psize - PAGEHDRSZ - SIZELEFT(p)) / \
				((env)->me_psize - PAGEHDRSZ))
	/** The minimum page fill factor, in tenths of a percent.
	 *	Pages emptier than this are candidates for merging.
	 */
#define FILL_THRESHOLD	 250

	/** Test if a page is a leaf page */
#define IS_LEAF(p)	 F_ISSET(MP_FLAGS(p), P_LEAF)
	/** Test if a page is a LEAF2 page */
#define IS_LEAF2(p)	 F_ISSET(MP_FLAGS(p), P_LEAF2)
	/** Test if a page is a branch page */
#define IS_BRANCH(p)	 F_ISSET(MP_FLAGS(p), P_BRANCH)
	/** Test if a page stores subtree counts */
#define IS_COUNTED(p)	 F_ISSET(MP_FLAGS(p), P_COUNTED)
	/** Test if a page is an overflow page */
#define IS_OVERFLOW(p)	 F_ISSET(MP_FLAGS(p), P_OVERFLOW)
	/** Test if a page is a sub page */
#define IS_SUBP(p)	 F_ISSET(MP_FLAGS(p), P_SUBP)

	/** The number of overflow pages needed to store the given size. */
#define OVPAGES(size, psize)	((PAGEHDRSZ-1 + (size)) / (psize) + 1)

	/** Link in #MDB_txn.%mt_loose_pgs list.
	 *  Kept outside the page header, which is needed when reusing the page.
	 */
#define NEXT_LOOSE_PAGE(p)		(*(MDB_page **)((p) + 2))

	/** Header for a single key/data pair within a page.
	 * Used in pages of type #P_BRANCH and #P_LEAF without #P_LEAF2.
	 * We guarantee 2-byte alignment for 'MDB_node's.
	 *
	 * #mn_lo and #mn_hi are used for data size on leaf nodes, and for child
	 * pgno on branch nodes.  On 64 bit platforms, #mn_flags is also used
	 * for pgno.  (Branch nodes have no flags).  Lo and hi are in host byte
	 * order in case some accesses can be optimized to 32-bit word access.
	 *
	 * Leaf node flags describe node contents.  #F_BIGDATA says the node's
	 * data part is the page number of an overflow page with actual data.
	 * #F_DUPDATA and #F_SUBDATA can be combined giving duplicate data in
	 * a sub-page/sub-database, and named databases (just #F_SUBDATA).
	 */
typedef struct MDB_node {
	/** part of data size or pgno
	 *	@{ */
#if BYTE_ORDER == LITTLE_ENDIAN
	unsigned short	mn_lo, mn_hi;
#else
	unsigned short	mn_hi, mn_lo;
#endif
	/** @} */
/** @defgroup mdb_node Node Flags
 *	@ingroup internal
 *	Flags for node headers.
 *	@{
 */
#define F_BIGDATA	 0x01			/**< data put on overflow page */
#define F_SUBDATA	 0x02			/**< data is a sub-database */
#define F_DUPDATA	 0x04			/**< data has duplicates */

/** valid flags for #mdb_node_add() */
#define	NODE_ADD_FLAGS	(F_DUPDATA|F_SUBDATA|MDB_RESERVE|MDB_APPEND)

/** @} */
	unsigned short	mn_flags;		/**< @ref mdb_node */
	unsigned short	mn_ksize;		/**< key size */
	char		mn_data[1];			/**< key and data are appended here */
} MDB_node;

	/** Size of the node header, excluding dynamic data at the end */
#define NODESIZE	 offsetof(MDB_node, mn_data)

	/** Bit position of top word in page number, for shifting mn_flags */
#define PGNO_TOPWORD ((pgno_t)-1 > 0xffffffffu ? 32 : 0)

	/** Size of a node in a branch page with a given key.
	 *	This is just the node header plus the key, there is no data.
	 */
#define INDXSIZE(k)	 (NODESIZE + ((k) == NULL ? 0 : EVEN((k)->mv_size)))

	/** Size of a node in a leaf page with a given key and data.
	 *	This is node header plus key plus data size.
	 */
#define LEAFSIZE(k, d)	 (NODESIZE + EVEN((k)->mv_size) + (d)->mv_size)

	/** Address of node \b i in page \b p */
#define NODEPTR(p, i)	 ((MDB_node *)((char *)(p) + MP_PTRS(p)[i] + PAGEBASE))

	/** Address of the key for the node */
#define NODEKEY(mp, node)	 (void *)((char *)(node)->mn_data + \
		((IS_BRANCH(mp) && IS_COUNTED(mp)) ? (ptrdiff_t)sizeof(uint64_t) : 0))

	/** Address of the data for a node */
#define NODEDATA(node)	 (void *)((char *)(node)->mn_data + EVEN((node)->mn_ksize))

	/** Get the page number pointed to by a branch node */
#define NODEPGNO(node) \
	((node)->mn_lo | ((pgno_t) (node)->mn_hi << 16) | \
	 (PGNO_TOPWORD ? ((pgno_t) (node)->mn_flags << PGNO_TOPWORD) : 0))
	/** Set the page number in a branch node */
#define SETPGNO(node,pgno)	do { \
	(node)->mn_lo = (pgno) & 0xffff; (node)->mn_hi = (pgno) >> 16; \
	if (PGNO_TOPWORD) (node)->mn_flags = (pgno) >> PGNO_TOPWORD; } while(0)

	/** Get the size of the data in a leaf node */
#define NODEDSZ(node)	 ((node)->mn_lo | ((unsigned)(node)->mn_hi << 16))
	/** Set the size of the data for a leaf node */
#define SETDSZ(node,size)	do { \
	(node)->mn_lo = (size) & 0xffff; (node)->mn_hi = (size) >> 16;} while(0)
	/** The size of a key in a node */
#define NODEKSZ(node)	 ((node)->mn_ksize)

	/** The address of a key in a LEAF2 page.
	 *	LEAF2 pages are used for #MDB_DUPFIXED sorted-duplicate sub-DBs.
	 *	There are no node headers, keys are stored contiguously.
	 */
#define LEAF2KEY(p, i, ks)	((char *)(p) + PAGEHDRSZ + ((i)*(ks)))

static void
mdb_node_set_count(MDB_page *mp, MDB_node *node, uint64_t value)
{
	if (IS_BRANCH(mp) && IS_COUNTED(mp))
		memcpy(node->mn_data, &value, sizeof(value));
}

static uint64_t
mdb_node_get_count(const MDB_page *mp, const MDB_node *node)
{
	uint64_t value = 0;
	if (IS_BRANCH(mp) && IS_COUNTED(mp))
		memcpy(&value, node->mn_data, sizeof(value));
	return value;
}

#define MDB_VARINT_MAX	9U

static size_t
mdb_varint_length(uint64_t value)
{
	size_t len = 1;
	while (value >= 0x80) {
		value >>= 7;
		len++;
	}
	return len;
}

static size_t
mdb_varint_encode(uint64_t value, unsigned char *dest)
{
	size_t len = mdb_varint_length(value);
	size_t idx = len;
	while (idx) {
		unsigned char byte = (unsigned char)(value & 0x7f);
		dest[--idx] = byte;
		value >>= 7;
	}
	for (size_t i = 0; i + 1 < len; ++i)
		dest[i] |= 0x80;
	return len;
}

static int
mdb_varint_decode(const unsigned char *src, size_t available,
	uint64_t *out, size_t *used)
{
	uint64_t value = 0;
	size_t consumed = 0;

	while (consumed < available && consumed < MDB_VARINT_MAX) {
		unsigned char byte = src[consumed++];
		value = (value << 7) | (byte & 0x7f);
		if ((byte & 0x80) == 0) {
			if (out)
				*out = value;
			if (used)
				*used = consumed;
			return MDB_SUCCESS;
		}
	}
	return MDB_CORRUPTED;
}

static size_t
mdb_leaf_shared_prefix(const MDB_val *a, const MDB_val *b)
{
	size_t limit = a->mv_size < b->mv_size ? a->mv_size : b->mv_size;
	const unsigned char *lhs = (const unsigned char *)a->mv_data;
	const unsigned char *rhs = (const unsigned char *)b->mv_data;
	size_t i = 0;
	for (; i < limit && lhs[i] == rhs[i]; ++i)
		;
	return i;
}

static size_t
mdb_leaf_encoded_size(const MDB_val *trunk, const MDB_val *key, size_t *shared_out)
{
	if (!trunk || !trunk->mv_data)
		return key->mv_size;
	size_t shared = mdb_leaf_shared_prefix(trunk, key);
	if (shared_out)
		*shared_out = shared;
	return mdb_varint_length(shared) + (key->mv_size - shared);
}

static size_t
mdb_leaf_encode_key_fast(const MDB_val *key, size_t shared,
	unsigned char *dest)
{
	if (shared > key->mv_size)
		shared = key->mv_size;
	size_t header = mdb_varint_encode(shared, dest);
	size_t suffix_len = key->mv_size - shared;
	if (suffix_len)
		memcpy(dest + header,
		    (const unsigned char *)key->mv_data + shared, suffix_len);
	return header + suffix_len;
}

static size_t
mdb_leaf_encode_key(const MDB_val *trunk, const MDB_val *key,
	unsigned char *dest, size_t *shared_out)
{
	if (!trunk || !trunk->mv_data) {
		memcpy(dest, key->mv_data, key->mv_size);
		if (shared_out)
			*shared_out = 0;
		return key->mv_size;
	}
	size_t shared = mdb_leaf_shared_prefix(trunk, key);
	size_t total = mdb_leaf_encode_key_fast(key, shared, dest);
	if (shared_out)
		*shared_out = shared;
	return total;
}

static int
mdb_prefix_decode_shared(const unsigned char *encoded, size_t encoded_len,
	uint64_t *shared_out, size_t *used_out)
{
	if (!encoded_len)
		return MDB_CORRUPTED;

	unsigned char first = encoded[0];
	if ((first & 0x80) == 0) {
		if (shared_out)
			*shared_out = first;
		if (used_out)
			*used_out = 1;
		return MDB_SUCCESS;
	}

return mdb_varint_decode(encoded, encoded_len, shared_out, used_out);
}

typedef struct MDB_prefix_header {
	size_t shared;	/**< decoded shared-prefix length */
	size_t used;	/**< bytes consumed by the prefix header */
	unsigned int valid; /**< header decode succeeded */
} MDB_prefix_header;

static int
mdb_leaf_decode_key(const MDB_val *trunk, const unsigned char *encoded,
	size_t encoded_len, MDB_val *out, void *buf, size_t buf_size,
	int allow_trunk_alias, const MDB_prefix_header *header)
{
	size_t used = 0;
	uint64_t shared = 0;
	size_t suffix_len, full_len;
	unsigned char *decoded;

	if (!trunk || !trunk->mv_data) {
		if (!allow_trunk_alias) {
			if (encoded_len > buf_size)
				return MDB_BAD_VALSIZE;
			memcpy(buf, encoded, encoded_len);
			out->mv_data = buf;
		} else {
			out->mv_data = (void *)encoded;
		}
		out->mv_size = encoded_len;
		return MDB_SUCCESS;
	}

	if (header && header->valid) {
		shared = header->shared;
		used = header->used;
	} else {
		int rc = mdb_prefix_decode_shared(encoded, encoded_len, &shared, &used);
		if (rc != MDB_SUCCESS)
			return rc;
	}

	if (shared > trunk->mv_size || used > encoded_len)
		goto raw_fallback;

	suffix_len = encoded_len - used;
	full_len = (size_t)shared + suffix_len;
	if (full_len > buf_size)
		return MDB_BAD_VALSIZE;

	decoded = (unsigned char *)buf;
	memcpy(decoded, trunk->mv_data, (size_t)shared);
	memcpy(decoded + (size_t)shared, encoded + used, suffix_len);

	/* Reject accidental parses of raw keys as prefix-encoded payloads.
	 * A valid encoding must round-trip with the canonical shared prefix.
	 */
	{
		const unsigned char *trunk_bytes =
		    (const unsigned char *)trunk->mv_data;
		size_t canonical_shared = (size_t)shared;
		size_t trunk_tail = trunk->mv_size - canonical_shared;
		size_t extra = 0;

		/* decoded starts with exactly 'shared' trunk bytes; only scan tail */
		while (extra < suffix_len && extra < trunk_tail &&
		    trunk_bytes[canonical_shared + extra] == encoded[used + extra])
			extra++;

		/* Any extra shared bytes mean the payload wasn't canonically encoded. */
		if (extra)
			goto raw_fallback;

		/* Fast path: one-byte shared prefix header is always canonical. */
		if (!(used == 1 && encoded[0] < 0x80)) {
			size_t canonical_used = mdb_varint_length(canonical_shared);
			unsigned char hdr[MDB_VARINT_MAX];
			size_t hdr_len;

			if (canonical_used != used)
				goto raw_fallback;

			hdr_len = mdb_varint_encode(canonical_shared, hdr);
			if (hdr_len != canonical_used ||
			    memcmp(encoded, hdr, hdr_len) != 0)
				goto raw_fallback;
		}
	}

	out->mv_data = decoded;
	out->mv_size = full_len;
	return MDB_SUCCESS;

raw_fallback:
	if (!trunk || !trunk->mv_data) {
		if (!allow_trunk_alias) {
			if (encoded_len > buf_size)
				return MDB_BAD_VALSIZE;
			memcpy(buf, encoded, encoded_len);
			out->mv_data = buf;
		} else {
			out->mv_data = (void *)encoded;
		}
		out->mv_size = encoded_len;
		return MDB_SUCCESS;
	}
	if (!allow_trunk_alias) {
		if (encoded_len > buf_size)
			return MDB_BAD_VALSIZE;
		memcpy(buf, encoded, encoded_len);
		out->mv_data = buf;
	} else {
		out->mv_data = (void *)encoded;
	}
	out->mv_size = encoded_len;
	return MDB_SUCCESS;
}

	/** Information about a single database in the environment. */
typedef struct MDB_db {
	uint32_t	md_pad;		/**< also ksize for LEAF2 pages */
	uint16_t	md_flags;	/**< @ref mdb_dbi_open */
	uint16_t	md_depth;	/**< depth of this tree */
	pgno_t		md_branch_pages;	/**< number of internal pages */
	pgno_t		md_leaf_pages;		/**< number of leaf pages */
	pgno_t		md_overflow_pages;	/**< number of overflow pages */
	mdb_size_t	md_entries;		/**< number of data items */
	pgno_t		md_root;		/**< the root page of this tree */
} MDB_db;

#define MDB_VALID	0x8000		/**< DB handle is valid, for me_dbflags */
#define PERSISTENT_FLAGS	(0xffff & ~(MDB_VALID))
	/** #mdb_dbi_open() flags */
#define VALID_FLAGS	(MDB_REVERSEKEY|MDB_DUPSORT|MDB_INTEGERKEY|MDB_DUPFIXED|\
	MDB_INTEGERDUP|MDB_REVERSEDUP|MDB_COUNTED|MDB_PREFIX_COMPRESSION|MDB_CREATE)

	/** Handle for the DB used to track free pages. */
#define	FREE_DBI	0
	/** Handle for the default DB. */
#define	MAIN_DBI	1
	/** Number of DBs in metapage (free and main) - also hardcoded elsewhere */
#define CORE_DBS	2

	/** Number of meta pages - also hardcoded elsewhere */
#define NUM_METAS	2

	/** Meta page content.
	 *	A meta page is the start point for accessing a database snapshot.
	 *	Pages 0-1 are meta pages. Transaction N writes meta page #(N % 2).
	 */
typedef struct MDB_meta {
		/** Stamp identifying this as an LMDB file. It must be set
		 *	to #MDB_MAGIC. */
	uint32_t	mm_magic;
		/** Version number of this file. Must be set to #MDB_DATA_VERSION. */
	uint32_t	mm_version;
#ifdef MDB_VL32
	union {		/* always zero since we don't support fixed mapping in MDB_VL32 */
		MDB_ID	mmun_ull;
		void *mmun_address;
	} mm_un;
#define	mm_address mm_un.mmun_address
#else
	void		*mm_address;		/**< address for fixed mapping */
#endif
	mdb_size_t	mm_mapsize;			/**< size of mmap region */
	MDB_db		mm_dbs[CORE_DBS];	/**< first is free space, 2nd is main db */
	/** The size of pages used in this DB */
#define	mm_psize	mm_dbs[FREE_DBI].md_pad
	/** Any persistent environment flags. @ref mdb_env */
#define	mm_flags	mm_dbs[FREE_DBI].md_flags
	/** Last used page in the datafile.
	 *	Actually the file may be shorter if the freeDB lists the final pages.
	 */
	pgno_t		mm_last_pg;
	volatile txnid_t	mm_txnid;	/**< txnid that committed this page */
} MDB_meta;

	/** Buffer for a stack-allocated meta page.
	 *	The members define size and alignment, and silence type
	 *	aliasing warnings.  They are not used directly; that could
	 *	mean incorrectly using several union members in parallel.
	 */
typedef union MDB_metabuf {
	MDB_page	mb_page;
	struct {
		char		mm_pad[PAGEHDRSZ];
		MDB_meta	mm_meta;
	} mb_metabuf;
} MDB_metabuf;

	/** Auxiliary DB info.
	 *	The information here is mostly static/read-only. There is
	 *	only a single copy of this record in the environment.
	 */
typedef struct MDB_dbx {
	MDB_val		md_name;		/**< name of the database */
	MDB_cmp_func	*md_cmp;	/**< function for comparing keys */
	MDB_cmp_func	*md_dcmp;	/**< function for comparing data items */
	MDB_rel_func	*md_rel;	/**< user relocate function */
	void		*md_relctx;		/**< user-provided context for md_rel */
} MDB_dbx;

struct MDB_prefix_rebuild_entry;
struct MDB_cursor;

typedef struct MDB_prefix_measure_cache {
	struct MDB_prefix_rebuild_entry *entries;
	unsigned char *keybuf;
	struct MDB_cursor *cursor;
	pgno_t pgno;
	unsigned int count;
	unsigned int insert;
	unsigned int node_flags;
	unsigned int is_reserve;
	unsigned int is_sub;
	size_t key_bytes;
	size_t insert_key_size;
	size_t insert_data_size;
	MDB_page *snapshot;
	size_t snapshot_bytes;
	unsigned int valid;
} MDB_prefix_measure_cache;

typedef struct MDB_prefix_stride_entry {
	pgno_t			pgno;
	uint32_t		*lengths;
	unsigned int	length_cap;
	unsigned int	count;
	size_t			max_len;
	unsigned int	max_valid;
	unsigned int	valid;
} MDB_prefix_stride_entry;

typedef struct MDB_prefix_stride_cache {
	MDB_prefix_stride_entry	*entries;
	unsigned int			count;
	unsigned int			capacity;
} MDB_prefix_stride_cache;

typedef struct MDB_prefix_scratch {
	void		*snapshot;		/**< cached page copy for prefix rebuild */
	size_t		snapshot_size;	/**< allocated bytes for snapshot */
	struct MDB_prefix_rebuild_entry *entries; /**< cached entry descriptors */
	unsigned int	entries_cap;	/**< number of cached entry slots */
	unsigned char	*keybuf;	/**< contiguous decoded key storage */
	size_t		keybuf_size;	/**< allocated bytes for decoded keys */
	unsigned char	*encodedbuf;	/**< contiguous encoded key storage */
	size_t		encodedbuf_size;/**< allocated bytes for encoded keys */
	MDB_prefix_measure_cache measure_cache; /**< cached measure descriptors */
	MDB_prefix_stride_cache stride_cache; /**< cached decoded length metadata */
} MDB_prefix_scratch;

typedef struct MDB_cursor_leaf_cache {
	MDB_val		*decoded_vals;		/**< cached decoded keys for this cursor */
	unsigned int	decoded_vals_cap;	/**< capacity for decoded_vals */
	unsigned char	*decoded_ready;		/**< bitmap of decoded key slots */
	unsigned int	decoded_ready_cap;	/**< capacity for decoded_ready */
	unsigned char	*decoded_buf;		/**< contiguous decoded key storage */
	size_t		decoded_buf_size;	/**< allocated bytes for decoded_buf */
	size_t		decoded_stride;		/**< bytes reserved per decoded key */
	uint64_t	*decoded_prefix;	/**< cached prefix counts for this leaf */
	unsigned int	decoded_prefix_cap; /**< capacity for decoded_prefix */
	unsigned int	decoded_prefix_count; /**< populated prefix entries */
	unsigned int	decoded_count;		/**< total keys in cached page */
	unsigned int	decoded_slots_ready; /**< decoded_buf assigned to decoded_vals */
	pgno_t		decoded_pgno;		/**< leaf page number currently cached */
	txnid_t		decoded_gen;		/**< txnid that generated decoded cache */
	const MDB_page	*decoded_source;	/**< exact leaf/subpage pointer backing the cache */
} MDB_cursor_leaf_cache;

	/** A database transaction.
	 *	Every operation requires a transaction handle.
	 */
struct MDB_txn {
	MDB_txn		*mt_parent;		/**< parent of a nested txn */
	/** Nested txn under this txn, set together with flag #MDB_TXN_HAS_CHILD */
	MDB_txn		*mt_child;
	pgno_t		mt_next_pgno;	/**< next unallocated page */
#ifdef MDB_VL32
	pgno_t		mt_last_pgno;	/**< last written page */
#endif
	/** The ID of this transaction. IDs are integers incrementing from 1.
	 *	Only committed write transactions increment the ID. If a transaction
	 *	aborts, the ID may be re-used by the next writer.
	 */
	txnid_t		mt_txnid;
	MDB_env		*mt_env;		/**< the DB environment */
	/** The list of pages that became unused during this transaction.
	 */
	MDB_IDL		mt_free_pgs;
	/** The list of loose pages that became unused and may be reused
	 *	in this transaction, linked through #NEXT_LOOSE_PAGE(page).
	 */
	MDB_page	*mt_loose_pgs;
	/** Number of loose pages (#mt_loose_pgs) */
	int			mt_loose_count;
	/** The sorted list of dirty pages we temporarily wrote to disk
	 *	because the dirty list was full. page numbers in here are
	 *	shifted left by 1, deleted slots have the LSB set.
	 */
	MDB_IDL		mt_spill_pgs;
	union {
		/** For write txns: Modified pages. Sorted when not MDB_WRITEMAP. */
		MDB_ID2L	dirty_list;
		/** For read txns: This thread/txn's reader table slot, or NULL. */
		MDB_reader	*reader;
	} mt_u;
	/** Array of records for each DB known in the environment. */
	MDB_dbx		*mt_dbxs;
	/** Array of MDB_db records for each known DB */
	MDB_db		*mt_dbs;
	/** Array of sequence numbers for each DB handle */
	unsigned int	*mt_dbiseqs;
/** @defgroup mt_dbflag	Transaction DB Flags
 *	@ingroup internal
 * @{
 */
#define DB_DIRTY	0x01		/**< DB was written in this txn */
#define DB_STALE	0x02		/**< Named-DB record is older than txnID */
#define DB_NEW		0x04		/**< Named-DB handle opened in this txn */
#define DB_VALID	0x08		/**< DB handle is valid, see also #MDB_VALID */
#define DB_USRVALID	0x10		/**< As #DB_VALID, but not set for #FREE_DBI */
#define DB_DUPDATA	0x20		/**< DB is #MDB_DUPSORT data */
/** @} */
	/** In write txns, array of cursors for each DB */
	MDB_cursor	**mt_cursors;
	/** Array of flags for each DB */
	unsigned char	*mt_dbflags;
	MDB_prefix_scratch mt_prefix;	/**< scratch buffers for prefix compression maintenance */
#ifdef MDB_VL32
	/** List of read-only pages (actually chunks) */
	MDB_ID3L	mt_rpages;
	/** We map chunks of 16 pages. Even though Windows uses 4KB pages, all
	 * mappings must begin on 64KB boundaries. So we round off all pgnos to
	 * a chunk boundary. We do the same on Linux for symmetry, and also to
	 * reduce the frequency of mmap/munmap calls.
	 */
#define MDB_RPAGE_CHUNK	16
#define MDB_TRPAGE_SIZE	4096	/**< size of #mt_rpages array of chunks */
#define MDB_TRPAGE_MAX	(MDB_TRPAGE_SIZE-1)	/**< maximum chunk index */
	unsigned int mt_rpcheck;	/**< threshold for reclaiming unref'd chunks */
#endif
	/**	Number of DB records in use, or 0 when the txn is finished.
	 *	This number only ever increments until the txn finishes; we
	 *	don't decrement it when individual DB handles are closed.
	 */
	MDB_dbi		mt_numdbs;

/** @defgroup mdb_txn	Transaction Flags
 *	@ingroup internal
 *	@{
 */
	/** #mdb_txn_begin() flags */
#define MDB_TXN_BEGIN_FLAGS	(MDB_NOMETASYNC|MDB_NOSYNC|MDB_RDONLY)
#define MDB_TXN_NOMETASYNC	MDB_NOMETASYNC	/**< don't sync meta for this txn on commit */
#define MDB_TXN_NOSYNC		MDB_NOSYNC	/**< don't sync this txn on commit */
#define MDB_TXN_RDONLY		MDB_RDONLY	/**< read-only transaction */
	/* internal txn flags */
#define MDB_TXN_WRITEMAP	MDB_WRITEMAP	/**< copy of #MDB_env flag in writers */
#define MDB_TXN_FINISHED	0x01		/**< txn is finished or never began */
#define MDB_TXN_ERROR		0x02		/**< txn is unusable after an error */
#define MDB_TXN_DIRTY		0x04		/**< must write, even if dirty list is empty */
#define MDB_TXN_SPILLS		0x08		/**< txn or a parent has spilled pages */
#define MDB_TXN_HAS_CHILD	0x10		/**< txn has an #MDB_txn.%mt_child */
	/** most operations on the txn are currently illegal */
#define MDB_TXN_BLOCKED		(MDB_TXN_FINISHED|MDB_TXN_ERROR|MDB_TXN_HAS_CHILD)
/** @} */
	unsigned int	mt_flags;		/**< @ref mdb_txn */
	int		mt_last_err;		/**< last error that marked txn invalid */
	/** #dirty_list room: Array size - \#dirty pages visible to this txn.
	 *	Includes ancestor txns' dirty pages not hidden by other txns'
	 *	dirty/spilled pages. Thus commit(nested txn) has room to merge
	 *	dirty_list into mt_parent after freeing hidden mt_parent pages.
	 */
	unsigned int	mt_dirty_room;
};

/** Enough space for 2^32 nodes with minimum of 2 keys per node. I.e., plenty.
 * At 4 keys per node, enough for 2^64 nodes, so there's probably no need to
 * raise this on a 64 bit machine.
 */
#define CURSOR_STACK		 32

struct MDB_xcursor;

static void
mdb_txn_mark_error(MDB_txn *txn, int err)
{
	if (!txn)
		return;
	txn->mt_last_err = err ? err : MDB_BAD_TXN;
	txn->mt_flags |= MDB_TXN_ERROR;
}

	/** Cursors are used for all DB operations.
	 *	A cursor holds a path of (page pointer, key index) from the DB
	 *	root to a position in the DB, plus other state. #MDB_DUPSORT
	 *	cursors include an xcursor to the current data item. Write txns
	 *	track their cursors and keep them up to date when data moves.
	 *	Exception: An xcursor's pointer to a #P_SUBP page can be stale.
	 *	(A node with #F_DUPDATA but no #F_SUBDATA contains a subpage).
	 */
struct MDB_cursor {
	/** Next cursor on this DB in this txn */
	MDB_cursor	*mc_next;
	/** Backup of the original cursor if this cursor is a shadow */
	MDB_cursor	*mc_backup;
	/** Context used for databases with #MDB_DUPSORT, otherwise NULL */
	struct MDB_xcursor	*mc_xcursor;
	/** The transaction that owns this cursor */
	MDB_txn		*mc_txn;
	/** The database handle this cursor operates on */
	MDB_dbi		mc_dbi;
	/** The database record for this cursor */
	MDB_db		*mc_db;
	/** The database auxiliary record for this cursor */
	MDB_dbx		*mc_dbx;
	/** The @ref mt_dbflag for this database */
	unsigned char	*mc_dbflag;
	unsigned short 	mc_snum;	/**< number of pushed pages */
	unsigned short	mc_top;		/**< index of top page, normally mc_snum-1 */
/** @defgroup mdb_cursor	Cursor Flags
 *	@ingroup internal
 *	Cursor state flags.
 *	@{
 */
#define C_INITIALIZED	0x01	/**< cursor has been initialized and is valid */
#define C_EOF	0x02			/**< No more data */
#define C_SUB	0x04			/**< Cursor is a sub-cursor */
#define C_DEL	0x08			/**< last op was a cursor_del */
#define C_UNTRACK	0x40		/**< Un-track cursor when closing */
#define C_WRITEMAP	MDB_TXN_WRITEMAP /**< Copy of txn flag */
#define C_LEAFCACHE	0x80		/**< Cursor supports leaf decode caching */
#define C_SEQEXPECT	0x100		/**< Cursor expects sequential decode reuse */
#define C_RANKVALID	0x200		/**< Cursor cache for rank traversal is valid */
/** Read-only cursor into the txn's original snapshot in the map.
 *	Set for read-only txns, and in #mdb_page_alloc() for #FREE_DBI when
 *	#MDB_DEVEL & 2. Only implements code which is necessary for this.
 */
#define C_ORIG_RDONLY	MDB_TXN_RDONLY
/** @} */
	unsigned int	mc_flags;	/**< @ref mdb_cursor */
	MDB_page	*mc_pg[CURSOR_STACK];	/**< stack of pushed pages */
	indx_t		mc_ki[CURSOR_STACK];	/**< stack of page indices */
#ifdef MDB_VL32
	MDB_page	*mc_ovpg;		/**< a referenced overflow page */
#	define MC_OVPG(mc)			((mc)->mc_ovpg)
#	define MC_SET_OVPG(mc, pg)	((mc)->mc_ovpg = (pg))
#else
#	define MC_OVPG(mc)			((MDB_page *)0)
#	define MC_SET_OVPG(mc, pg)	((void)0)
#endif
	MDB_val		mc_key;			/**< scratch buffer for decompressed keys */
	unsigned char	mc_keybuf[MDB_KEYBUF_MAX];
	pgno_t		mc_key_pgno;		/**< leaf page that mc_key currently describes */
	indx_t		mc_key_last;		/**< index last decoded into mc_key */
	pgno_t		mc_seq_pgno;		/**< leaf page of last user-visible key */
	indx_t		mc_seq_idx;		/**< leaf index of last user-visible key */
	size_t		mc_seq_shared;		/**< cached shared bytes currently materialized in mc_keybuf */
	size_t		mc_seq_cached_shared;	/**< cached shared prefix for repeated reads */
	size_t		mc_seq_cached_used;	/**< cached header byte count for repeated reads */
	indx_t		mc_seq_cached_idx;	/**< leaf index associated with cached shared prefix */
	pgno_t		mc_seq_cached_pgno;	/**< page number associated with cached shared prefix */
	unsigned int	mc_seq_cache_valid;	/**< cached shared prefix metadata valid flag */
	unsigned int	mc_seq_keybuf_valid;	/**< mc_keybuf currently holds decoded key bytes */
	const void	*mc_seq_cmp_keyptr;	/**< cached search key pointer for prefix compare skip */
	size_t		mc_seq_cmp_keysize;	/**< cached search key size for prefix compare skip */
	size_t		mc_seq_cmp_prefix;	/**< cached shared prefix between search key and trunk */
pgno_t		mc_seq_cmp_pgno;	/**< leaf page the compare cache applies to */
	MDB_cursor_leaf_cache mc_leaf_cache; /**< cursor-local decoded leaf cache */
	uint64_t	mc_rank_base[CURSOR_STACK]; /**< cached subtree prefixes for rank traversal */
	uint64_t	mc_rank_cached; /**< cached absolute rank of current cursor position */
	uint64_t	mc_rank_offset; /**< cached duplicate offset for current entry */
};

static void
mdb_cursor_rank_invalidate(MDB_cursor *mc)
{
	if (!mc)
		return;
	mc->mc_flags &= ~C_RANKVALID;
	mc->mc_rank_cached = 0;
	mc->mc_rank_offset = 0;
}

static void
mdb_cursor_seq_invalidate(MDB_cursor *mc)
{
	if (!mc)
		return;
	mc->mc_seq_pgno = P_INVALID;
	mc->mc_seq_idx = (indx_t)~0;
	mc->mc_seq_shared = 0;
	mc->mc_seq_cached_shared = 0;
	mc->mc_seq_cached_used = 0;
	mc->mc_seq_cached_idx = (indx_t)~0;
	mc->mc_seq_cached_pgno = P_INVALID;
	mc->mc_seq_cache_valid = 0;
	mc->mc_seq_keybuf_valid = 0;
	mc->mc_seq_cmp_keyptr = NULL;
	mc->mc_seq_cmp_keysize = 0;
	mc->mc_seq_cmp_prefix = 0;
	mc->mc_seq_cmp_pgno = P_INVALID;
	mdb_cursor_rank_invalidate(mc);
}

	/** Context for sorted-dup records.
	 *	We could have gone to a fully recursive design, with arbitrarily
	 *	deep nesting of sub-databases. But for now we only handle these
	 *	levels - main DB, optional sub-DB, sorted-duplicate DB.
	 */
typedef struct MDB_xcursor {
	/** A sub-cursor for traversing the Dup DB */
	MDB_cursor mx_cursor;
	/** The database record for this Dup DB */
	MDB_db	mx_db;
	/**	The auxiliary DB record for this Dup DB */
	MDB_dbx	mx_dbx;
	/** The @ref mt_dbflag for this Dup DB */
	unsigned char mx_dbflag;
	/** Total bytes reserved for inline sub-page payloads (0 for sub-DB) */
	size_t mx_inline_bytes;
	/** Cached inline capacity measurement for pending insert */
	size_t mx_inline_required;
	/** Cached measurement validity flag */
	unsigned int mx_inline_measure_ready;
} MDB_xcursor;

static void mdb_cursor_leaf_cache_init(MDB_cursor_leaf_cache *cache);
static void mdb_cursor_leaf_cache_reset(MDB_cursor_leaf_cache *cache);
static void mdb_cursor_leaf_cache_clear(MDB_cursor_leaf_cache *cache);
static int mdb_cursor_leaf_cache_clone(MDB_cursor_leaf_cache *dst,
	const MDB_cursor_leaf_cache *src, MDB_txn *txn);
static int mdb_cursor_leaf_cache_ensure_prefix(MDB_txn *txn,
	MDB_cursor_leaf_cache *cache, unsigned int count, uint64_t **out);
static int mdb_cursor_leaf_cache_prepare(MDB_cursor *mc, MDB_page *mp);
static void mdb_cursor_leaf_cache_assign_slots(MDB_cursor_leaf_cache *cache);
static int mdb_cursor_leaf_cache_materialize(MDB_cursor *mc, MDB_page *mp);
static size_t mdb_cursor_seq_cmp_refresh(MDB_cursor *mc, MDB_page *mp,
	const MDB_val *search_key);
static int mdb_cmp_memn_with_skip(const MDB_val *a, const MDB_val *b,
	size_t skip);

static int
mdb_cursor_try_seq_fastpath(MDB_cursor *mc, MDB_page *mp, MDB_node *node,
	indx_t idx, const MDB_val *trunk, MDB_val *out,
	MDB_prefix_scratch *scratch)
{
	const unsigned char *encoded = NODEKEY(mp, node);
	size_t encoded_len = node->mn_ksize;
	size_t used = 0;
	size_t shared = 0;
	size_t prev_shared = 0;
	int have_prev = 0;

	if (!encoded_len)
		return 0;

	if (mc->mc_seq_keybuf_valid &&
	    mc->mc_seq_pgno == MP_PGNO(mp) &&
	    mc->mc_seq_idx != (indx_t)~0 &&
	    mc->mc_seq_idx + 1 == idx) {
		prev_shared = mc->mc_seq_shared;
		have_prev = 1;
	}

	unsigned char first = encoded[0];
	if ((first & 0x80) == 0) {
		shared = first;
		used = 1;
	} else if (mc->mc_seq_cache_valid &&
		   mc->mc_seq_cached_pgno == MP_PGNO(mp) &&
		   mc->mc_seq_cached_idx == idx) {
		shared = mc->mc_seq_cached_shared;
		used = mc->mc_seq_cached_used;
	} else {
		uint64_t shared64 = 0;
		int vrc = mdb_varint_decode(encoded, encoded_len, &shared64, &used);
		if (vrc != MDB_SUCCESS)
			return 0;
		shared = (size_t)shared64;
	}

	if (used > encoded_len)
		return 0;

	mc->mc_seq_cached_pgno = MP_PGNO(mp);
	mc->mc_seq_cached_idx = idx;
	mc->mc_seq_cached_shared = shared;
	mc->mc_seq_cached_used = used;
	mc->mc_seq_cache_valid = 1;

	size_t trunk_len = trunk->mv_size;
	if (shared > trunk_len)
		return 0;

	size_t suffix_len = encoded_len - used;
	size_t needed = shared + suffix_len;

	if (needed > MDB_KEYBUF_MAX)
		return 0;

	unsigned char *dst = mc->mc_keybuf;
	const unsigned char *trunk_bytes = (const unsigned char *)trunk->mv_data;
	size_t reuse = 0;

	if (have_prev) {
		reuse = prev_shared < shared ? prev_shared : shared;
	}

	if (shared > reuse) {
		memcpy(dst + reuse, trunk_bytes + reuse, shared - reuse);
	}

	if (suffix_len)
		memcpy(dst + shared, encoded + used, suffix_len);

	mc->mc_key.mv_data = dst;
	mc->mc_key.mv_size = needed;
	mc->mc_key_pgno = MP_PGNO(mp);
	mc->mc_key_last = idx;
	mc->mc_seq_shared = shared;
	mc->mc_seq_keybuf_valid = 1;
	*out = mc->mc_key;
	return 1;
}

static int
mdb_cmp_memn_with_skip(const MDB_val *a, const MDB_val *b, size_t skip)
{
	const unsigned char *adata = (const unsigned char *)a->mv_data;
	const unsigned char *bdata = (const unsigned char *)b->mv_data;
	size_t alen = a->mv_size;
	size_t blen = b->mv_size;
	size_t minlen = alen < blen ? alen : blen;

	if (skip >= minlen) {
		if (alen == blen)
			return 0;
		return (alen < blen) ? -1 : 1;
	}

	size_t cmp_len = minlen - skip;
	int diff = memcmp(adata + skip, bdata + skip, cmp_len);
	if (diff)
		return diff;
	if (alen == blen)
		return 0;
	return (alen < blen) ? -1 : 1;
}

static size_t
mdb_cursor_seq_cmp_refresh(MDB_cursor *mc, MDB_page *mp, const MDB_val *search_key)
{
	if (!mc || !mp || !search_key || !search_key->mv_data) {
		mc->mc_seq_cmp_pgno = P_INVALID;
		mc->mc_seq_cmp_keyptr = NULL;
		mc->mc_seq_cmp_keysize = 0;
		mc->mc_seq_cmp_prefix = 0;
		return 0;
	}

	if (mc->mc_seq_cmp_pgno == MP_PGNO(mp) &&
	    mc->mc_seq_cmp_keyptr == search_key->mv_data &&
	    mc->mc_seq_cmp_keysize == search_key->mv_size) {
		return mc->mc_seq_cmp_prefix;
	}

	if (!(mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) ||
	    !IS_LEAF(mp) || NUMKEYS(mp) == 0) {
		mc->mc_seq_cmp_pgno = P_INVALID;
		mc->mc_seq_cmp_keyptr = NULL;
		mc->mc_seq_cmp_keysize = 0;
		mc->mc_seq_cmp_prefix = 0;
		return 0;
	}

	MDB_node *trunk = NODEPTR(mp, 0);
	const unsigned char *trunk_bytes = NODEKEY(mp, trunk);
	size_t trunk_len = trunk->mn_ksize;
	const unsigned char *key_bytes = (const unsigned char *)search_key->mv_data;
	size_t limit = trunk_len < search_key->mv_size ? trunk_len : search_key->mv_size;
	size_t prefix = 0;

	while (prefix < limit && trunk_bytes[prefix] == key_bytes[prefix])
		++prefix;

	mc->mc_seq_cmp_pgno = MP_PGNO(mp);
	mc->mc_seq_cmp_keyptr = search_key->mv_data;
	mc->mc_seq_cmp_keysize = search_key->mv_size;
	mc->mc_seq_cmp_prefix = prefix;
	return prefix;
}

static int
mdb_leaf_cmp_memn_encoded(MDB_page *mp, MDB_node *node,
	const MDB_val *trunk, const MDB_val *search_key, size_t key_trunk_prefix,
	int *cmp_res)
{
	const unsigned char *encoded = NODEKEY(mp, node);
	size_t encoded_len = node->mn_ksize;
	const unsigned char *suffix;
	size_t used = 0;
	uint64_t shared64 = 0;
	size_t shared = 0;
	size_t suffix_len;
	size_t candidate_len;
	size_t skip;
	size_t minlen;
	size_t pos;
	const unsigned char *key_bytes;
	const unsigned char *trunk_bytes;

	if (!encoded_len || !trunk || !trunk->mv_data || !search_key || !search_key->mv_data)
		return MDB_BAD_VALSIZE;

	int rc = mdb_prefix_decode_shared(encoded, encoded_len, &shared64, &used);
	if (rc != MDB_SUCCESS)
		return rc;

	if (shared64 > trunk->mv_size || used > encoded_len)
		return MDB_BAD_VALSIZE;
	shared = (size_t)shared64;

	suffix = encoded + used;
	suffix_len = encoded_len - used;
	candidate_len = shared + suffix_len;
	key_bytes = (const unsigned char *)search_key->mv_data;
	trunk_bytes = (const unsigned char *)trunk->mv_data;

	skip = key_trunk_prefix < shared ? key_trunk_prefix : shared;
	pos = skip;
	minlen = search_key->mv_size < candidate_len ? search_key->mv_size : candidate_len;

	while (pos < minlen) {
		unsigned char a = key_bytes[pos];
		unsigned char b = (pos < shared) ? trunk_bytes[pos] : suffix[pos - shared];
		if (a != b) {
			*cmp_res = (a < b) ? -1 : 1;
			return MDB_SUCCESS;
		}
		pos++;
	}

	if (search_key->mv_size == candidate_len)
		*cmp_res = 0;
	else if (search_key->mv_size < candidate_len)
		*cmp_res = -1;
	else
		*cmp_res = 1;

	return MDB_SUCCESS;
}

	/** Check if there is an inited xcursor */
#define XCURSOR_INITED(mc) \
	((mc)->mc_xcursor && ((mc)->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))

	/** Update the xcursor's sub-page pointer, if any, in \b mc.  Needed
	 *	when the node which contains the sub-page may have moved.  Called
	 *	with leaf page \b mp = mc->mc_pg[\b top].
	 */
#define XCURSOR_REFRESH(mc, top, mp) do { \
	MDB_page *xr_pg = (mp); \
	MDB_node *xr_node; \
	if (!XCURSOR_INITED(mc) || (mc)->mc_ki[top] >= NUMKEYS(xr_pg)) break; \
	xr_node = NODEPTR(xr_pg, (mc)->mc_ki[top]); \
	if ((xr_node->mn_flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA) \
		(mc)->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(xr_node); \
} while (0)

static int
mdb_cursor_read_key_at(MDB_cursor *mc, MDB_page *mp, indx_t idx, MDB_val *out)
{
	if (!out)
		return MDB_SUCCESS;
	if (IS_LEAF2(mp)) {
		out->mv_size = mp->mp_pad;
		out->mv_data = LEAF2KEY(mp, idx, out->mv_size);
		return MDB_SUCCESS;
	}
	MDB_node *node = NODEPTR(mp, idx);
	if (IS_LEAF(mp)) {
		int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;
		MDB_prefix_scratch *scratch = &mc->mc_txn->mt_prefix;
		const unsigned char *encoded = NODEKEY(mp, node);

			if (!prefix_enabled || idx == 0) {
				if (prefix_enabled && idx == 0) {
	mc->mc_key_pgno = MP_PGNO(mp);
					mc->mc_key_last = idx;
					mc->mc_key.mv_size = node->mn_ksize;
					mc->mc_key.mv_data = (void *)encoded;
				}
				mc->mc_seq_shared = 0;
				mc->mc_seq_keybuf_valid = 0;
				mc->mc_seq_cache_valid = 0;
				mc->mc_seq_cached_pgno = P_INVALID;
				mc->mc_seq_cached_idx = (indx_t)~0;
				mc->mc_seq_cached_shared = 0;
				mc->mc_seq_cached_used = 0;
				out->mv_size = node->mn_ksize;
				out->mv_data = (void *)encoded;
				return MDB_SUCCESS;
			}

		MDB_node *trunk_node = NODEPTR(mp, 0);
		MDB_val trunk = { trunk_node->mn_ksize, NODEKEY(mp, trunk_node) };

		if ((mc->mc_flags & C_SEQEXPECT) &&
		    mdb_cursor_try_seq_fastpath(mc, mp, node, idx, &trunk, out, scratch))
			return MDB_SUCCESS;

		MDB_prefix_header header = {0, 0, 0};
		size_t shared_hint = 0;
		if (idx > 0 && prefix_enabled && node->mn_ksize > 0) {
			uint64_t shared64 = 0;
			size_t used = 0;
			int shrc = mdb_prefix_decode_shared(encoded, node->mn_ksize, &shared64, &used);
			if (shrc == MDB_SUCCESS) {
				if (shared64 <= SIZE_MAX) {
					header.shared = (size_t)shared64;
					header.used = used;
					header.valid = 1;
				}
				if (shared64 > trunk.mv_size)
					shared64 = trunk.mv_size;
				shared_hint = (size_t)shared64;
			}
		}

		if ((mc->mc_txn->mt_flags & MDB_TXN_RDONLY) &&
		    (mc->mc_flags & C_LEAFCACHE) &&
		    !(mc->mc_flags & C_SEQEXPECT)) {
			int prc = mdb_cursor_leaf_cache_prepare(mc, mp);
			if (prc != MDB_SUCCESS)
				return prc;
			MDB_cursor_leaf_cache *cache = &mc->mc_leaf_cache;
			if (idx < cache->decoded_count &&
			    cache->decoded_pgno == MP_PGNO(mp)) {
				if (!cache->decoded_slots_ready) {
					int mrc = mdb_cursor_leaf_cache_materialize(mc, mp);
					if (mrc != MDB_SUCCESS)
						return mrc;
				}
				if (!cache->decoded_ready[idx]) {
					MDB_val *slot = &cache->decoded_vals[idx];
					int drc = mdb_leaf_decode_key(&cache->decoded_vals[0],
					    encoded, node->mn_ksize,
					    slot, slot->mv_data, cache->decoded_stride, 0,
					    header.valid ? &header : NULL);
					if (drc != MDB_SUCCESS)
						return drc;
					cache->decoded_ready[idx] = 1;
				}
				*out = cache->decoded_vals[idx];
				mc->mc_seq_shared = shared_hint;
				return MDB_SUCCESS;
			}
		}

		mc->mc_key.mv_data = mc->mc_keybuf;

		int rc = mdb_leaf_decode_key(&trunk, encoded, node->mn_ksize,
		    &mc->mc_key, mc->mc_keybuf, MDB_KEYBUF_MAX, 0,
		    header.valid ? &header : NULL);
		if (rc != MDB_SUCCESS)
			return rc;

		mc->mc_key_pgno = MP_PGNO(mp);
		mc->mc_key_last = idx;
		mc->mc_seq_shared = shared_hint;
		mc->mc_seq_keybuf_valid = 1;
		mc->mc_seq_cache_valid = 0;
		mc->mc_seq_cached_pgno = P_INVALID;
		mc->mc_seq_cached_idx = (indx_t)~0;
		mc->mc_seq_cached_shared = 0;
		mc->mc_seq_cached_used = 0;
		*out = mc->mc_key;
		return MDB_SUCCESS;
	}
	out->mv_size = node->mn_ksize;
	out->mv_data = NODEKEY(mp, node);
	return MDB_SUCCESS;
}


	/** State of FreeDB old pages, stored in the MDB_env */
typedef struct MDB_pgstate {
	pgno_t		*mf_pghead;	/**< Reclaimed freeDB pages, or NULL before use */
	txnid_t		mf_pglast;	/**< ID of last used record, or 0 if !mf_pghead */
} MDB_pgstate;

	/** The database environment. */
struct MDB_env {
	HANDLE		me_fd;		/**< The main data file */
	HANDLE		me_lfd;		/**< The lock file */
	HANDLE		me_mfd;		/**< For writing and syncing the meta pages */
#ifdef _WIN32
#ifdef MDB_VL32
	HANDLE		me_fmh;		/**< File Mapping handle */
#endif /* MDB_VL32 */
	HANDLE		me_ovfd;	/**< Overlapped/async with write-through file handle */
#endif /* _WIN32 */
	/** Failed to update the meta page. Probably an I/O error. */
#define	MDB_FATAL_ERROR	0x80000000U
	/** Some fields are initialized. */
#define	MDB_ENV_ACTIVE	0x20000000U
	/** me_txkey is set */
#define	MDB_ENV_TXKEY	0x10000000U
	/** fdatasync is unreliable */
#define	MDB_FSYNCONLY	0x08000000U
	/** In-memory env uses process-local lock state (no lockfile). */
#define	MDB_ENV_INMEM_LOCKS	0x04000000U
	uint32_t 	me_flags;		/**< @ref mdb_env */
	unsigned int	me_psize;	/**< DB page size, inited from me_os_psize */
	unsigned int	me_os_psize;	/**< OS page size, from #GET_PAGESIZE */
	unsigned int	me_maxreaders;	/**< size of the reader table */
	/** Max #MDB_txninfo.%mti_numreaders of interest to #mdb_env_close() */
	volatile int	me_close_readers;
	MDB_dbi		me_numdbs;		/**< number of DBs opened */
	MDB_dbi		me_maxdbs;		/**< size of the DB table */
	MDB_PID_T	me_pid;		/**< process ID of this env */
	char		*me_path;		/**< path to the DB files */
	char		*me_map;		/**< the memory map of the data file */
	MDB_txninfo	*me_txns;		/**< the memory map of the lock file or NULL */
	MDB_meta	*me_metas[NUM_METAS];	/**< pointers to the two meta pages */
	void		*me_pbuf;		/**< scratch area for DUPSORT put() */
	MDB_txn		*me_txn;		/**< current write transaction */
	MDB_txn		*me_txn0;		/**< prealloc'd write transaction */
	mdb_size_t	me_mapsize;		/**< size of the data memory map */
	MDB_OFF_T	me_size;		/**< current file size */
	pgno_t		me_maxpg;		/**< me_mapsize / me_psize */
	MDB_dbx		*me_dbxs;		/**< array of static DB info */
	uint16_t	*me_dbflags;	/**< array of flags from MDB_db.md_flags */
	unsigned int	*me_dbiseqs;	/**< array of dbi sequence numbers */
	pthread_key_t	me_txkey;	/**< thread-key for readers */
	txnid_t		me_pgoldest;	/**< ID of oldest reader last time we looked */
	MDB_pgstate	me_pgstate;		/**< state of old pages from freeDB */
#	define		me_pglast	me_pgstate.mf_pglast
#	define		me_pghead	me_pgstate.mf_pghead
	MDB_page	*me_dpages;		/**< list of malloc'd blocks for re-use */
	/** IDL of pages that became unused in a write txn */
	MDB_IDL		me_free_pgs;
	/** ID2L of pages written during a write txn. Length MDB_IDL_UM_SIZE. */
	MDB_ID2L	me_dirty_list;
	/** Max number of freelist items that can fit in a single overflow page */
	int			me_maxfree_1pg;
	/** Max size of a node on a page */
	unsigned int	me_nodemax;
#if !(MDB_MAXKEYSIZE)
	unsigned int	me_maxkey;	/**< max size of a key */
#endif
	int		me_live_reader;		/**< have liveness lock in reader table */
#ifdef _WIN32
	int		me_pidquery;		/**< Used in OpenProcess */
	OVERLAPPED	*ov;			/**< Used for for overlapping I/O requests */
	int		ovs;				/**< Count of OVERLAPPEDs */
#endif
#ifdef MDB_USE_POSIX_MUTEX	/* Posix mutexes reside in shared mem */
#	define		me_rmutex	me_txns->mti_rmutex /**< Shared reader lock */
#	define		me_wmutex	me_txns->mti_wmutex /**< Shared writer lock */
#else
	mdb_mutex_t	me_rmutex;
	mdb_mutex_t	me_wmutex;
# if defined(_WIN32) || defined(MDB_USE_POSIX_SEM)
	/** Half-initialized name of mutexes, to be completed by #MUTEXNAME() */
	char		me_mutexname[sizeof(MUTEXNAME_PREFIX) + 11];
# endif
#endif
#ifdef MDB_VL32
	MDB_ID3L	me_rpages;	/**< like #mt_rpages, but global to env */
	pthread_mutex_t	me_rpmutex;	/**< control access to #me_rpages */
#define MDB_ERPAGE_SIZE	16384
#define MDB_ERPAGE_MAX	(MDB_ERPAGE_SIZE-1)
	unsigned int me_rpcheck;
#endif
	volatile sig_atomic_t me_interrupt; /**< interrupt requested */
	void		*me_userctx;	 /**< User-settable context */
	MDB_assert_func *me_assert_func; /**< Callback for assertion failures */
};

static int
mdb_txn_check_interrupt(MDB_txn *txn)
{
	if (!txn)
		return MDB_BAD_TXN;
	if (txn->mt_env->me_interrupt) {
		mdb_txn_mark_error(txn, EINTR);
		return EINTR;
	}
	return MDB_SUCCESS;
}

	/** Nested transaction */
typedef struct MDB_ntxn {
	MDB_txn		mnt_txn;		/**< the transaction */
	MDB_pgstate	mnt_pgstate;	/**< parent transaction's saved freestate */
} MDB_ntxn;

	/** max number of pages to commit in one writev() call */
#define MDB_COMMIT_PAGES	 64
#if defined(IOV_MAX) && IOV_MAX < MDB_COMMIT_PAGES
#undef MDB_COMMIT_PAGES
#define MDB_COMMIT_PAGES	IOV_MAX
#endif

	/** max bytes to write in one call */
#define MAX_WRITE		(0x40000000U >> (sizeof(ssize_t) == 4))

	/** Check \b txn and \b dbi arguments to a function */
#define TXN_DBI_EXIST(txn, dbi, validity) \
	((txn) && (dbi)<(txn)->mt_numdbs && ((txn)->mt_dbflags[dbi] & (validity)))

	/** Check for misused \b dbi handles */
#define TXN_DBI_CHANGED(txn, dbi) \
	((txn)->mt_dbiseqs[dbi] != (txn)->mt_env->me_dbiseqs[dbi])

static int  mdb_page_alloc(MDB_cursor *mc, int num, MDB_page **mp);
static int  mdb_page_new(MDB_cursor *mc, uint32_t flags, int num, MDB_page **mp);
static int  mdb_page_touch(MDB_cursor *mc);

#define MDB_END_NAMES {"committed", "empty-commit", "abort", "reset", \
	"reset-tmp", "fail-begin", "fail-beginchild"}
enum {
	/* mdb_txn_end operation number, for logging */
	MDB_END_COMMITTED, MDB_END_EMPTY_COMMIT, MDB_END_ABORT, MDB_END_RESET,
	MDB_END_RESET_TMP, MDB_END_FAIL_BEGIN, MDB_END_FAIL_BEGINCHILD
};
#define MDB_END_OPMASK	0x0F	/**< mask for #mdb_txn_end() operation number */
#define MDB_END_UPDATE	0x10	/**< update env state (DBIs) */
#define MDB_END_FREE	0x20	/**< free txn unless it is #MDB_env.%me_txn0 */
#define MDB_END_SLOT MDB_NOTLS	/**< release any reader slot if #MDB_NOTLS */
static void mdb_txn_end(MDB_txn *txn, unsigned mode);

static int  mdb_page_get(MDB_cursor *mc, pgno_t pgno, MDB_page **mp, int *lvl);
static int  mdb_page_search_root(MDB_cursor *mc,
			    MDB_val *key, int modify);
#define MDB_PS_MODIFY	1
#define MDB_PS_ROOTONLY	2
#define MDB_PS_FIRST	4
#define MDB_PS_LAST		8
static int  mdb_page_search(MDB_cursor *mc,
			    MDB_val *key, int flags);
static int	mdb_page_merge(MDB_cursor *csrc, MDB_cursor *cdst);

#define MDB_SPLIT_REPLACE	MDB_APPENDDUP	/**< newkey is not new */
static int	mdb_page_split(MDB_cursor *mc, MDB_val *newkey, MDB_val *newdata,
			pgno_t newpgno, unsigned int nflags);
static indx_t	mdb_page_insert_slot(MDB_cursor *mc, MDB_page *mp, indx_t indx, size_t node_size);
static void	mdb_page_remove_slot(MDB_cursor *mc, MDB_page *mp, indx_t indx, size_t node_size);

#define MDB_COUNT_HINT_NONE	((uint64_t)~0ULL)

static int  mdb_env_read_header(MDB_env *env, int prev, MDB_meta *meta);
static MDB_meta *mdb_env_pick_meta(const MDB_env *env);
static int  mdb_env_write_meta(MDB_txn *txn);
#if defined(MDB_USE_POSIX_MUTEX) && !defined(MDB_ROBUST_SUPPORTED) /* Drop unused excl arg */
# define mdb_env_close0(env, excl) mdb_env_close1(env)
#endif
static void mdb_env_close0(MDB_env *env, int excl);

static MDB_node *mdb_node_search(MDB_cursor *mc, MDB_val *key, int *exactp);
static int  mdb_node_add(MDB_cursor *mc, indx_t indx,
 		    MDB_val *key, MDB_val *data, pgno_t pgno, unsigned int flags,
 		    MDB_page *child_hint, uint64_t count_hint);
static void mdb_node_del(MDB_cursor *mc, int ksize);
static void mdb_node_shrink(MDB_page *mp, indx_t indx);
static int	mdb_node_move(MDB_cursor *csrc, MDB_cursor *cdst, int fromleft);
static int  mdb_node_read(MDB_cursor *mc, MDB_node *leaf, MDB_val *data);
static size_t	mdb_leaf_size(MDB_env *env, MDB_page *mp, indx_t indx, MDB_val *key, MDB_val *data, int prefix_enabled);
static size_t	mdb_branch_size(MDB_env *env, MDB_page *mp, MDB_val *key);
static int mdb_cursor_read_key_at(MDB_cursor *mc, MDB_page *mp, indx_t idx, MDB_val *out);
static size_t mdb_prefix_maxkey(const MDB_env *env);
static size_t mdb_prefix_decoded_length(size_t trunk_len, const unsigned char *encoded,
	size_t encoded_len);
static size_t mdb_leaf_maxkey_raw(const MDB_page *mp);
static uint64_t mdb_leaf_entry_contribution(const MDB_page *mp, const MDB_node *node);
static uint64_t mdb_leaf_prefix_contribution(MDB_cursor *mc, MDB_page *mp, indx_t limit);

static size_t
mdb_prefix_leaf_maxdecoded(const MDB_page *mp)
{
	if (!IS_LEAF(mp) || IS_LEAF2(mp))
		return 0;

	unsigned int total = NUMKEYS(mp);
	if (!total)
		return 0;

	MDB_node *trunk = NODEPTR(mp, 0);
	size_t trunk_len = trunk->mn_ksize;
	size_t max_len = trunk_len;

	for (unsigned int i = 1; i < total; ++i) {
		MDB_node *node = NODEPTR(mp, i);
		size_t encoded_len = node->mn_ksize;
		if (!encoded_len)
			continue;

		const unsigned char *encoded = NODEKEY(mp, node);
		size_t full_len = mdb_prefix_decoded_length(trunk_len, encoded, encoded_len);
		if (full_len > max_len)
			max_len = full_len;
	}

	return max_len ? max_len : trunk_len;
}

static size_t
mdb_leaf_maxkey_raw(const MDB_page *mp)
{
	if (!IS_LEAF(mp) || IS_LEAF2(mp))
		return 0;

	unsigned int total = NUMKEYS(mp);
	size_t max_len = 0;

	for (unsigned int i = 0; i < total; ++i) {
		MDB_node *node = NODEPTR(mp, i);
		if (node->mn_ksize > max_len)
			max_len = node->mn_ksize;
	}

	return max_len;
}

static void
mdb_prefix_leaf_store_stride(MDB_cursor *mc, MDB_page *mp, size_t stride)
{
	int prefix_enabled = mc && mc->mc_db &&
	    (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION);

	if (!IS_LEAF(mp) || IS_LEAF2(mp) || !prefix_enabled) {
		if (!IS_LEAF2(mp))
			MP_PAD(mp) = 0;
		return;
	}

	if (stride > UINT16_MAX)
		stride = UINT16_MAX;

	MP_PAD(mp) = (uint16_t)stride;
}

static void
mdb_prefix_leaf_refresh_stride(MDB_cursor *mc, MDB_page *mp)
{
	int prefix_enabled = mc && mc->mc_db &&
	    (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION);

	if (!IS_LEAF(mp) || IS_LEAF2(mp) || !prefix_enabled) {
		if (!IS_LEAF2(mp))
			MP_PAD(mp) = 0;
		return;
	}

	unsigned int total = NUMKEYS(mp);
	if (!total) {
		MP_PAD(mp) = 0;
		return;
	}

	size_t stride = mdb_prefix_leaf_maxdecoded(mp);
	if (!stride) {
		MDB_node *trunk = NODEPTR(mp, 0);
		stride = trunk->mn_ksize;
	}

	mdb_prefix_leaf_store_stride(mc, mp, stride);
}

static size_t
mdb_prefix_prealloc_size(MDB_txn *txn)
{
	MDB_env *env = txn->mt_env;
	size_t limit = mdb_prefix_maxkey(env);

	if (!limit) {
		if (env->me_nodemax)
			limit = env->me_nodemax;
		else if (env->me_psize)
			limit = env->me_psize;
	}

	size_t prealloc = limit;
	if (!prealloc || prealloc < 4096)
		prealloc = 4096;
	else if (prealloc <= (SIZE_MAX >> 1))
		prealloc <<= 1;

	return prealloc;
}

typedef struct MDB_prefix_rebuild_entry {
	MDB_val key;
	const unsigned char *data_ptr;
	size_t data_payload;
	size_t data_size;
	unsigned short encoded_ksize;
	unsigned short flags;
	const unsigned char *encoded_key;	/**< source encoded key bytes */
	unsigned short encoded_len;		/**< length of encoded_key */
	unsigned short encoded_used;		/**< prefix header bytes consumed during decode */
	unsigned short shared_prefix;		/**< decoded shared prefix length; UINT16_MAX when raw */
} MDB_prefix_rebuild_entry;

static int
mdb_prefix_ensure_snapshot(MDB_txn *txn, size_t size, MDB_page **out)
{
	MDB_prefix_scratch *scratch = &txn->mt_prefix;
	size_t baseline = mdb_prefix_prealloc_size(txn);
	size_t required = size;

	if (required < baseline)
		required = baseline;

	if (scratch->snapshot_size < required) {
		size_t newsize = scratch->snapshot_size;

		if (!newsize || newsize < baseline)
			newsize = baseline;

		if (!newsize)
			newsize = required;

		while (newsize < required) {
			if (newsize > (SIZE_MAX >> 1)) {
				newsize = required;
				break;
			}
			newsize <<= 1;
		}

		void *ptr = realloc(scratch->snapshot, newsize);
		if (!ptr)
			return ENOMEM;
		scratch->snapshot = ptr;
		scratch->snapshot_size = newsize;
	}
	if (out)
		*out = (MDB_page *)scratch->snapshot;
	return MDB_SUCCESS;
}

static void
mdb_prefix_snapshot_capture(MDB_page *dst, MDB_page *src, size_t capacity)
{
	unsigned char *dst_bytes = (unsigned char *)dst;
	const unsigned char *src_bytes = (const unsigned char *)src;
	size_t header_bytes = PAGEHDRSZ;

	if (capacity < header_bytes)
		header_bytes = capacity;
	memcpy(dst_bytes, src_bytes, header_bytes);

	unsigned int keys = NUMKEYS(src);
	if (keys) {
		memcpy(MP_PTRS(dst), MP_PTRS(src), keys * sizeof(indx_t));
	}

	indx_t upper = MP_UPPER(src);
	if ((size_t)upper < capacity) {
		size_t tail = capacity - (size_t)upper;
		memcpy(dst_bytes + upper, src_bytes + upper, tail);
	}
}

static int
mdb_prefix_ensure_entries(MDB_txn *txn, unsigned int count, MDB_prefix_rebuild_entry **out)
{
	MDB_prefix_scratch *scratch = &txn->mt_prefix;
	size_t baseline_bytes = mdb_prefix_prealloc_size(txn);
	unsigned int baseline = 0;

	if (count == 0) {
		if (out)
			*out = NULL;
		return MDB_SUCCESS;
	}

	if (baseline_bytes >= sizeof(MDB_prefix_rebuild_entry))
		baseline = (unsigned int)(baseline_bytes / sizeof(MDB_prefix_rebuild_entry));
	if (!baseline)
		baseline = 32;
	unsigned int target = count;
	if (target < baseline)
		target = baseline;

	if (scratch->entries_cap < target) {
		unsigned int newcap = scratch->entries_cap;

		if (!newcap || newcap < baseline)
			newcap = baseline;

		while (newcap < target) {
			if (newcap > (UINT_MAX >> 1)) {
				newcap = target;
				break;
			}
			newcap <<= 1;
		}

		MDB_prefix_rebuild_entry *ptr = realloc(scratch->entries,
		    sizeof(MDB_prefix_rebuild_entry) * newcap);
		if (!ptr)
			return ENOMEM;
		scratch->entries = ptr;
		scratch->entries_cap = newcap;
	}
	if (out)
		*out = scratch->entries;
	return MDB_SUCCESS;
}

static int
mdb_prefix_ensure_keybuf(MDB_txn *txn, size_t size, unsigned char **out)
{
	MDB_prefix_scratch *scratch = &txn->mt_prefix;
	size_t baseline = mdb_prefix_prealloc_size(txn);
	size_t required = size;

	if (size == 0)
		required = 1;
	if (required < baseline)
		required = baseline;

	if (scratch->keybuf_size < required) {
		size_t newsize = scratch->keybuf_size;

		if (!newsize || newsize < baseline)
			newsize = baseline;
		if (!newsize)
			newsize = required;

		while (newsize < required) {
			if (newsize > (SIZE_MAX >> 1)) {
				newsize = required;
				break;
			}
			newsize <<= 1;
		}

		unsigned char *ptr = realloc(scratch->keybuf, newsize);
		if (!ptr)
			return ENOMEM;
		scratch->keybuf = ptr;
		scratch->keybuf_size = newsize;
	}
	if (out)
		*out = scratch->keybuf;
	return MDB_SUCCESS;
}

static int
mdb_prefix_ensure_encbuf(MDB_txn *txn, size_t size, unsigned char **out)
{
	MDB_prefix_scratch *scratch = &txn->mt_prefix;
	size_t baseline = mdb_prefix_prealloc_size(txn);
	size_t required = size;

	if (size == 0)
		required = 1;
	if (required < baseline)
		required = baseline;

	if (scratch->encodedbuf_size < required) {
		size_t newsize = scratch->encodedbuf_size;

		if (!newsize || newsize < baseline)
			newsize = baseline;
		if (!newsize)
			newsize = required;

		while (newsize < required) {
			if (newsize > (SIZE_MAX >> 1)) {
				newsize = required;
				break;
			}
			newsize <<= 1;
		}

		unsigned char *ptr = realloc(scratch->encodedbuf, newsize);
		if (!ptr)
			return ENOMEM;
		scratch->encodedbuf = ptr;
		scratch->encodedbuf_size = newsize;
	}
	if (out)
		*out = scratch->encodedbuf;
	return MDB_SUCCESS;
}

static void
mdb_prefix_stride_cache_clear(MDB_prefix_stride_cache *cache)
{
	if (!cache || !cache->entries)
		return;
	for (unsigned int i = 0; i < cache->capacity; ++i) {
		MDB_prefix_stride_entry *entry = &cache->entries[i];
		if (entry->pgno == P_INVALID)
			continue;
		free(entry->lengths);
		entry->lengths = NULL;
		entry->length_cap = 0;
		entry->count = 0;
		entry->max_len = 0;
		entry->max_valid = 1;
		entry->valid = 0;
		entry->pgno = P_INVALID;
	}
	free(cache->entries);
	cache->entries = NULL;
	cache->count = 0;
	cache->capacity = 0;
}

static unsigned int
mdb_prefix_stride_hash(pgno_t pgno)
{
	return (unsigned int)(pgno ^ (pgno >> 32));
}

static MDB_prefix_stride_entry *
mdb_prefix_stride_entry_find_slot(MDB_prefix_stride_entry *entries,
	unsigned int capacity, pgno_t pgno)
{
	if (!entries || !capacity)
		return NULL;
	unsigned int mask = capacity - 1;
	unsigned int hash = mdb_prefix_stride_hash(pgno);
	unsigned int idx = hash & mask;
	for (unsigned int probe = 0; probe < capacity; ++probe) {
		MDB_prefix_stride_entry *entry = &entries[idx];
		if (entry->pgno == pgno)
			return entry;
		if (entry->pgno == P_INVALID)
			return NULL;
		idx = (idx + 1) & mask;
	}
	return NULL;
}

static int
mdb_prefix_stride_cache_grow(MDB_prefix_stride_cache *cache)
{
	unsigned int newcap = cache->capacity ? cache->capacity << 1 : 8;
	MDB_prefix_stride_entry *fresh = calloc(newcap,
	    sizeof(MDB_prefix_stride_entry));
	if (!fresh)
		return ENOMEM;
	for (unsigned int i = 0; i < newcap; ++i)
		fresh[i].pgno = P_INVALID;
	if (cache->entries) {
		for (unsigned int i = 0; i < cache->capacity; ++i) {
			MDB_prefix_stride_entry *old = &cache->entries[i];
			MDB_prefix_stride_entry tmp;
			if (old->pgno == P_INVALID)
				continue;
			tmp = *old;
			unsigned int mask = newcap - 1;
			unsigned int hash = mdb_prefix_stride_hash(tmp.pgno);
			unsigned int idx = hash & mask;
			while (fresh[idx].pgno != P_INVALID)
				idx = (idx + 1) & mask;
			fresh[idx] = tmp;
		}
		free(cache->entries);
	}
	cache->entries = fresh;
	cache->capacity = newcap;
	return MDB_SUCCESS;
}

static int
mdb_prefix_stride_entry_acquire(MDB_prefix_stride_cache *cache, pgno_t pgno,
	MDB_prefix_stride_entry **out)
{
	if (!cache || !out)
		return EINVAL;

	MDB_prefix_stride_entry *entry =
	    mdb_prefix_stride_entry_find_slot(cache->entries, cache->capacity,
	    pgno);
	if (entry) {
		*out = entry;
		return MDB_SUCCESS;
	}

	if (!cache->capacity ||
	    cache->count + 1 > (cache->capacity * 3) / 4) {
		int grc = mdb_prefix_stride_cache_grow(cache);
		if (grc != MDB_SUCCESS)
			return grc;
	}

	unsigned int mask = cache->capacity - 1;
	unsigned int hash = mdb_prefix_stride_hash(pgno);
	unsigned int idx = hash & mask;
	for (;;) {
		entry = &cache->entries[idx];
		if (entry->pgno == P_INVALID) {
			memset(entry, 0, sizeof(*entry));
			entry->pgno = pgno;
			cache->count++;
			*out = entry;
			return MDB_SUCCESS;
		}
		idx = (idx + 1) & mask;
	}
}

static uint32_t
mdb_prefix_stride_pack_length(size_t len)
{
	if (len > UINT32_MAX)
		return UINT32_MAX;
	return (uint32_t)len;
}

static size_t
mdb_prefix_stride_entry_max(MDB_prefix_stride_entry *entry)
{
	if (!entry || !entry->count)
		return 0;
	if (entry->max_valid)
		return entry->max_len;
	size_t max_len = entry->lengths ? entry->lengths[0] : 0;
	for (unsigned int i = 1; i < entry->count; ++i) {
		size_t len = entry->lengths ? entry->lengths[i] : 0;
		if (len > max_len)
			max_len = len;
	}
	entry->max_len = max_len;
	entry->max_valid = 1;
	return max_len;
}

static int
mdb_prefix_stride_entry_reserve(MDB_prefix_stride_entry *entry,
	unsigned int need)
{
	if (!entry)
		return EINVAL;
	if (entry->length_cap >= need)
		return MDB_SUCCESS;

	unsigned int newcap = entry->length_cap ? entry->length_cap : 16;
	while (newcap < need) {
		if (newcap > (UINT_MAX >> 1)) {
			newcap = need;
			break;
		}
		newcap <<= 1;
	}

	uint32_t *buf = realloc(entry->lengths, sizeof(uint32_t) * newcap);
	if (!buf)
		return ENOMEM;
	entry->lengths = buf;
	entry->length_cap = newcap;
	return MDB_SUCCESS;
}

static size_t
mdb_prefix_decoded_length(size_t trunk_len, const unsigned char *encoded,
	size_t encoded_len)
{
	if (!encoded_len)
		return 0;
	uint64_t shared64 = 0;
	size_t used = 0;
	int rc = mdb_prefix_decode_shared(encoded, encoded_len, &shared64, &used);
	if (rc != MDB_SUCCESS)
		return encoded_len;

	if (shared64 > trunk_len || used > encoded_len)
		return encoded_len;

	size_t suffix_len = encoded_len - used;
	if ((size_t)shared64 > SIZE_MAX - suffix_len)
		return encoded_len;
	size_t decoded_len = (size_t)shared64 + suffix_len;
	/* Never underestimate: non-canonical/raw payloads can decode shorter. */
	if (decoded_len < encoded_len)
		decoded_len = encoded_len;
	return decoded_len;
}

/* Parse a prefix-compressed payload and verify it is canonical against trunk.
 * Returns 1 when canonical prefix encoding is confirmed, else 0 (treat raw). */
static int
mdb_prefix_decode_canonical(const MDB_val *trunk, const unsigned char *encoded,
	size_t encoded_len, size_t *shared_out, size_t *used_out,
	size_t *full_len_out)
{
	uint64_t shared64 = 0;
	size_t used = 0, shared, suffix_len, full_len;
	const unsigned char *trunk_bytes;
	size_t trunk_tail, extra = 0;

	if (!trunk || !trunk->mv_data || !encoded || !encoded_len)
		return 0;

	if (mdb_prefix_decode_shared(encoded, encoded_len, &shared64, &used) != MDB_SUCCESS)
		return 0;
	if (shared64 > trunk->mv_size || used > encoded_len)
		return 0;

	shared = (size_t)shared64;
	suffix_len = encoded_len - used;
	if (shared > SIZE_MAX - suffix_len)
		return 0;
	full_len = shared + suffix_len;

	trunk_bytes = (const unsigned char *)trunk->mv_data;
	trunk_tail = trunk->mv_size - shared;

	/* Canonical encoding must not leave additional trunk bytes in suffix. */
	while (extra < suffix_len && extra < trunk_tail &&
	    trunk_bytes[shared + extra] == encoded[used + extra])
		extra++;
	if (extra)
		return 0;

	/* Fast path: single-byte header is canonical by construction. */
	if (!(used == 1 && encoded[0] < 0x80)) {
		size_t canonical_used = mdb_varint_length(shared);
		unsigned char hdr[MDB_VARINT_MAX];
		size_t hdr_len;

		if (canonical_used != used)
			return 0;
		hdr_len = mdb_varint_encode(shared, hdr);
		if (hdr_len != canonical_used || memcmp(encoded, hdr, hdr_len) != 0)
			return 0;
	}

	if (shared_out)
		*shared_out = shared;
	if (used_out)
		*used_out = used;
	if (full_len_out)
		*full_len_out = full_len;
	return 1;
}

static int
mdb_prefix_stride_entry_rebuild(MDB_cursor *mc, MDB_page *mp,
	MDB_prefix_stride_entry *entry)
{
	if (!entry)
		return MDB_SUCCESS;
	if (!mc || !mp)
		return MDB_SUCCESS;
	if (!IS_LEAF(mp) || IS_LEAF2(mp) || IS_SUBP(mp))
		return MDB_SUCCESS;

	unsigned int total = NUMKEYS(mp);
	int rc = mdb_prefix_stride_entry_reserve(entry, total ? total : 1);
	if (rc != MDB_SUCCESS)
		return rc;

	entry->count = total;
	size_t max_len = 0;

	if (!total) {
		entry->max_len = 0;
		entry->max_valid = 1;
		entry->valid = 1;
		mdb_prefix_leaf_store_stride(mc, mp, 0);
		return MDB_SUCCESS;
	}

	MDB_node *trunk = NODEPTR(mp, 0);
	size_t trunk_len = trunk ? trunk->mn_ksize : 0;
	entry->lengths[0] = mdb_prefix_stride_pack_length(trunk_len);
	max_len = trunk_len;

	for (unsigned int i = 1; i < total; ++i) {
		MDB_node *node = NODEPTR(mp, i);
		const unsigned char *encoded = NODEKEY(mp, node);
		size_t len = mdb_prefix_decoded_length(trunk_len, encoded,
		    node->mn_ksize);
		entry->lengths[i] = mdb_prefix_stride_pack_length(len);
		if (len > max_len)
			max_len = len;
	}

	entry->max_len = max_len;
	entry->max_valid = 1;
	entry->valid = 1;
	mdb_prefix_leaf_store_stride(mc, mp, max_len);
	return MDB_SUCCESS;
}

static int
mdb_prefix_stride_prepare(MDB_cursor *mc, MDB_page *mp,
	MDB_prefix_stride_entry **out)
{
	if (out)
		*out = NULL;
	if (!mc || !mp || IS_SUBP(mp))
		return MDB_SUCCESS;
	if (mc->mc_txn->mt_flags & MDB_TXN_RDONLY)
		return MDB_SUCCESS;
	if (!(mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION))
		return MDB_SUCCESS;
	if (!IS_LEAF(mp) || IS_LEAF2(mp))
		return MDB_SUCCESS;

	MDB_prefix_scratch *scratch = &mc->mc_txn->mt_prefix;
	MDB_prefix_stride_entry *entry = NULL;
	int rc = mdb_prefix_stride_entry_acquire(&scratch->stride_cache,
	    mp->mp_pgno, &entry);
	if (rc != MDB_SUCCESS)
		return rc;
	if (!entry)
		return MDB_SUCCESS;

	if (!entry->valid || entry->count != NUMKEYS(mp)) {
		entry->valid = 0;
		rc = mdb_prefix_stride_entry_rebuild(mc, mp, entry);
		if (rc != MDB_SUCCESS)
			return rc;
	}

	if (out)
		*out = entry;
	return MDB_SUCCESS;
}

static int
mdb_prefix_stride_apply_insert(MDB_prefix_stride_entry *entry, MDB_cursor *mc,
	MDB_page *mp, indx_t indx, size_t keylen)
{
	if (!entry || !mc || !mp)
		return MDB_SUCCESS;

	unsigned int needed = entry->count + 1;
	int rc = mdb_prefix_stride_entry_reserve(entry, needed);
	if (rc != MDB_SUCCESS)
		return rc;

	if (indx > entry->count)
		indx = entry->count;
	if (entry->count > (unsigned int)indx) {
		memmove(&entry->lengths[indx + 1], &entry->lengths[indx],
		    (entry->count - indx) * sizeof(uint32_t));
	}
	entry->lengths[indx] = mdb_prefix_stride_pack_length(keylen);
	entry->count++;
	if (keylen >= entry->max_len) {
		entry->max_len = keylen;
		entry->max_valid = 1;
	}
	mdb_prefix_leaf_store_stride(mc, mp, mdb_prefix_stride_entry_max(entry));
	return MDB_SUCCESS;
}

static void
mdb_prefix_stride_apply_delete(MDB_prefix_stride_entry *entry, MDB_cursor *mc,
	MDB_page *mp, indx_t indx)
{
	if (!entry || !mc || !mp)
		return;
	if (indx >= entry->count)
		return;

	size_t removed = entry->lengths[indx];
	if (indx + 1 < entry->count) {
		memmove(&entry->lengths[indx], &entry->lengths[indx + 1],
		    (entry->count - indx - 1) * sizeof(uint32_t));
	}
	if (entry->count)
		entry->count--;

	if (!entry->count) {
		entry->max_len = 0;
		entry->max_valid = 1;
		mdb_prefix_leaf_store_stride(mc, mp, 0);
		return;
	}

	if (removed >= entry->max_len)
		entry->max_valid = 0;
	mdb_prefix_leaf_store_stride(mc, mp, mdb_prefix_stride_entry_max(entry));
}

static void
mdb_prefix_stride_entry_invalidate(MDB_txn *txn, pgno_t pgno)
{
	if (!txn)
		return;
	MDB_prefix_stride_cache *cache = &txn->mt_prefix.stride_cache;
	MDB_prefix_stride_entry *entry =
	    mdb_prefix_stride_entry_find_slot(cache->entries, cache->capacity,
	    pgno);
	if (entry)
		entry->valid = 0;
}

static void
mdb_cursor_leaf_cache_init(MDB_cursor_leaf_cache *cache)
{
	if (!cache)
		return;
	cache->decoded_vals = NULL;
	cache->decoded_vals_cap = 0;
	cache->decoded_ready = NULL;
	cache->decoded_ready_cap = 0;
	cache->decoded_buf = NULL;
	cache->decoded_buf_size = 0;
	cache->decoded_stride = 0;
	cache->decoded_prefix = NULL;
	cache->decoded_prefix_cap = 0;
	cache->decoded_prefix_count = 0;
	cache->decoded_count = 0;
	cache->decoded_slots_ready = 0;
	cache->decoded_pgno = P_INVALID;
	cache->decoded_gen = 0;
	cache->decoded_source = NULL;
}

static void
mdb_cursor_leaf_cache_reset(MDB_cursor_leaf_cache *cache)
{
	if (!cache)
		return;
	cache->decoded_count = 0;
	cache->decoded_stride = 0;
	cache->decoded_prefix_count = 0;
	cache->decoded_slots_ready = 0;
	cache->decoded_pgno = P_INVALID;
	cache->decoded_gen = 0;
	cache->decoded_source = NULL;
}

static void
mdb_cursor_leaf_cache_clear(MDB_cursor_leaf_cache *cache)
{
	if (!cache)
		return;
	if (cache->decoded_vals) {
		free(cache->decoded_vals);
		cache->decoded_vals = NULL;
	}
	if (cache->decoded_ready) {
		free(cache->decoded_ready);
		cache->decoded_ready = NULL;
	}
	if (cache->decoded_buf) {
		free(cache->decoded_buf);
		cache->decoded_buf = NULL;
	}
	if (cache->decoded_prefix) {
		free(cache->decoded_prefix);
		cache->decoded_prefix = NULL;
	}
	mdb_cursor_leaf_cache_init(cache);
}

static int
mdb_cursor_leaf_cache_ensure_vals(MDB_cursor_leaf_cache *cache, unsigned int count)
{
	if (count == 0)
		return MDB_SUCCESS;
	if (cache->decoded_vals_cap < count) {
		unsigned int newcap = cache->decoded_vals_cap ? cache->decoded_vals_cap : 16;
		while (newcap < count)
			newcap *= 2;
		MDB_val *vals = realloc(cache->decoded_vals, sizeof(MDB_val) * newcap);
		if (!vals)
			return ENOMEM;
		cache->decoded_vals = vals;
		cache->decoded_vals_cap = newcap;
	}
	return MDB_SUCCESS;
}

static int
mdb_cursor_leaf_cache_ensure_ready(MDB_cursor_leaf_cache *cache, unsigned int count)
{
	if (count == 0)
		return MDB_SUCCESS;
	if (cache->decoded_ready_cap < count) {
		unsigned int newcap = cache->decoded_ready_cap ? cache->decoded_ready_cap : 16;
		while (newcap < count)
			newcap *= 2;
		unsigned char *bits = realloc(cache->decoded_ready, newcap);
		if (!bits)
			return ENOMEM;
		cache->decoded_ready = bits;
		cache->decoded_ready_cap = newcap;
	}
	return MDB_SUCCESS;
}

static int
mdb_cursor_leaf_cache_reserve_buf(MDB_cursor_leaf_cache *cache, size_t need)
{
	size_t size = cache->decoded_buf_size;

	if (size < need) {
		size_t newsize = size ? size : 256;
		while (newsize < need) {
			if (newsize > (SIZE_MAX >> 1)) {
				newsize = need;
				break;
			}
			newsize *= 2;
		}
		unsigned char *buf = realloc(cache->decoded_buf, newsize);
		if (!buf)
			return ENOMEM;
		cache->decoded_buf = buf;
		cache->decoded_buf_size = newsize;
	}
	return MDB_SUCCESS;
}

static void
mdb_cursor_leaf_cache_assign_slots(MDB_cursor_leaf_cache *cache)
{
	if (!cache || !cache->decoded_buf || !cache->decoded_vals ||
	    !cache->decoded_stride || !cache->decoded_count)
		return;
	for (unsigned int i = 0; i < cache->decoded_count; ++i)
		cache->decoded_vals[i].mv_data =
		    cache->decoded_buf + (size_t)i * cache->decoded_stride;
}

static int
mdb_cursor_leaf_cache_materialize(MDB_cursor *mc, MDB_page *mp)
{
	if (!mc)
		return MDB_SUCCESS;
	MDB_cursor_leaf_cache *cache = &mc->mc_leaf_cache;
	if (!cache->decoded_count || cache->decoded_slots_ready)
		return MDB_SUCCESS;

	size_t need = (size_t)cache->decoded_count * cache->decoded_stride;
	int rc = mdb_cursor_leaf_cache_reserve_buf(cache, need);
	if (rc != MDB_SUCCESS)
		return rc;

	mdb_cursor_leaf_cache_assign_slots(cache);
	for (unsigned int i = 0; i < cache->decoded_count; ++i)
		cache->decoded_vals[i].mv_size = 0;
	if (cache->decoded_ready && cache->decoded_count)
		memset(cache->decoded_ready, 0, cache->decoded_count);

	cache->decoded_slots_ready = 1;

	if (!cache->decoded_count)
		return MDB_SUCCESS;

	const MDB_page *source = cache->decoded_source ?
	    cache->decoded_source : mp;
	if (!source || IS_LEAF2(source))
		return MDB_SUCCESS;

	MDB_node *trunk = NODEPTR((MDB_page *)source, 0);
	if (!trunk || !cache->decoded_vals[0].mv_data)
		return MDB_SUCCESS;

	size_t copy = trunk->mn_ksize;
	if (copy > cache->decoded_stride)
		copy = cache->decoded_stride;
	if (copy)
		memcpy(cache->decoded_vals[0].mv_data, NODEKEY(source, trunk), copy);
	cache->decoded_vals[0].mv_size = trunk->mn_ksize;
	if (cache->decoded_ready)
		cache->decoded_ready[0] = 1;
	return MDB_SUCCESS;
}

static int
mdb_cursor_leaf_cache_ensure_prefix(MDB_txn *txn, MDB_cursor_leaf_cache *cache,
	unsigned int count, uint64_t **out)
{
	if (!cache)
		return EINVAL;

	if (count == 0)
		count = 1;
	if (cache->decoded_prefix_cap < count) {
		size_t baseline_bytes = mdb_prefix_prealloc_size(txn);
		unsigned int baseline = 0;
		unsigned int target = count;

		if (baseline_bytes >= sizeof(uint64_t))
			baseline = (unsigned int)(baseline_bytes / sizeof(uint64_t));
		if (!baseline)
			baseline = 32;
		if (target < baseline)
			target = baseline;

		unsigned int newcap = cache->decoded_prefix_cap;
		if (!newcap || newcap < baseline)
			newcap = baseline;

		while (newcap < target) {
			if (newcap > (UINT_MAX >> 1)) {
				newcap = target;
				break;
			}
			newcap <<= 1;
		}
		uint64_t *vals = realloc(cache->decoded_prefix, sizeof(uint64_t) * newcap);
		if (!vals)
			return ENOMEM;
		cache->decoded_prefix = vals;
		cache->decoded_prefix_cap = newcap;
	}
	if (out)
		*out = cache->decoded_prefix;
	return MDB_SUCCESS;
}

static int
mdb_cursor_leaf_cache_clone(MDB_cursor_leaf_cache *dst,
	const MDB_cursor_leaf_cache *src, MDB_txn *txn)
{
	if (!dst || !src || !txn)
		return EINVAL;

	unsigned int count = src->decoded_count;
	size_t stride = src->decoded_stride;
	size_t buf_need = (src->decoded_slots_ready && stride) ?
	    (size_t)count * stride : 0;
	int rc;

	rc = mdb_cursor_leaf_cache_ensure_vals(dst, count);
	if (rc != MDB_SUCCESS)
		return rc;
	rc = mdb_cursor_leaf_cache_ensure_ready(dst, count);
	if (rc != MDB_SUCCESS)
		return rc;
	if (count && dst->decoded_ready)
		memset(dst->decoded_ready, 0, count);
	for (unsigned int i = 0; i < count; ++i) {
		dst->decoded_vals[i].mv_data = NULL;
		dst->decoded_vals[i].mv_size = 0;
	}

	if (src->decoded_prefix_count) {
		rc = mdb_cursor_leaf_cache_ensure_prefix(txn, dst,
		    src->decoded_prefix_count, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
		if (src->decoded_prefix && dst->decoded_prefix)
			memcpy(dst->decoded_prefix, src->decoded_prefix,
			    sizeof(uint64_t) * src->decoded_prefix_count);
		dst->decoded_prefix_count = src->decoded_prefix_count;
	} else {
		dst->decoded_prefix_count = 0;
	}

	if (buf_need) {
		rc = mdb_cursor_leaf_cache_reserve_buf(dst, buf_need);
		if (rc != MDB_SUCCESS)
			return rc;
		mdb_cursor_leaf_cache_assign_slots(dst);
		if (dst->decoded_buf && src->decoded_buf)
			memcpy(dst->decoded_buf, src->decoded_buf, buf_need);
		for (unsigned int i = 0; i < count; ++i)
			dst->decoded_vals[i].mv_size = src->decoded_vals[i].mv_size;
		dst->decoded_slots_ready = 1;
	} else {
		dst->decoded_slots_ready = 0;
	}

	if (count && src->decoded_ready && dst->decoded_ready)
		memcpy(dst->decoded_ready, src->decoded_ready, count);

	dst->decoded_stride = stride;
	dst->decoded_count = count;
	dst->decoded_pgno = src->decoded_pgno;
	dst->decoded_gen = src->decoded_gen;
	dst->decoded_source = src->decoded_source;

	return MDB_SUCCESS;
}

static size_t
mdb_prefix_maxkey(const MDB_env *env)
{
#if MDB_MAXKEYSIZE
	(void)env;
	return MDB_MAXKEYSIZE;
#else
	return env->me_maxkey;
#endif
}

static int
mdb_cursor_leaf_cache_prepare(MDB_cursor *mc, MDB_page *mp)
{
	MDB_txn *txn = mc->mc_txn;
	MDB_env *env = txn->mt_env;
	MDB_cursor_leaf_cache *cache = &mc->mc_leaf_cache;
	unsigned int total = NUMKEYS(mp);
	int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;
	int rc;
	const MDB_page *source = IS_SUBP(mp) ? mp : NULL;

	if (cache->decoded_pgno == MP_PGNO(mp) &&
	    cache->decoded_gen == txn->mt_txnid &&
	    cache->decoded_count == total &&
	    cache->decoded_source == source)
		return MDB_SUCCESS;

	mdb_cursor_leaf_cache_reset(cache);

	rc = mdb_cursor_leaf_cache_ensure_vals(cache, total);
	if (rc != MDB_SUCCESS)
		goto fail;
	rc = mdb_cursor_leaf_cache_ensure_ready(cache, total);
	if (rc != MDB_SUCCESS)
		goto fail;
	size_t stride = 0;
	if (total && prefix_enabled && !IS_LEAF2(mp))
		stride = MP_PAD(mp);
	if (total && prefix_enabled && !IS_LEAF2(mp) && !stride)
		stride = mdb_prefix_leaf_maxdecoded(mp);
	if (total && !prefix_enabled) {
		if (IS_LEAF2(mp))
			stride = mc->mc_db ? mc->mc_db->md_pad : 0;
		else
			stride = mdb_leaf_maxkey_raw(mp);
	}
	if (total && !stride)
		stride = mdb_prefix_maxkey(env);
	if (!stride)
		stride = env->me_psize ? env->me_psize : 1;
	if (!stride)
		stride = 1;
	else {
		size_t limit = mdb_prefix_maxkey(env);
		if (limit && stride > limit)
			stride = limit;
	}
	if (cache->decoded_ready && total > 0)
		memset(cache->decoded_ready, 0, total);
	for (unsigned int i = 0; i < total; ++i) {
		cache->decoded_vals[i].mv_data = NULL;
		cache->decoded_vals[i].mv_size = 0;
	}
	cache->decoded_slots_ready = 0;
	cache->decoded_prefix_count = 0;

	cache->decoded_pgno = MP_PGNO(mp);
	cache->decoded_gen = txn->mt_txnid;
	cache->decoded_count = total;
	cache->decoded_stride = stride;
	cache->decoded_source = source;

	return MDB_SUCCESS;

fail:
	mdb_cursor_leaf_cache_reset(cache);
	return rc;
}

static void
mdb_prefix_scratch_reset(MDB_prefix_scratch *scratch)
{
	if (!scratch)
		return;

	memset(&scratch->measure_cache, 0, sizeof(scratch->measure_cache));

	if (scratch->stride_cache.entries) {
		for (unsigned int i = 0; i < scratch->stride_cache.capacity; ++i) {
			MDB_prefix_stride_entry *entry = &scratch->stride_cache.entries[i];
			if (entry->pgno != P_INVALID)
				entry->valid = 0;
		}
	}
}

static void
mdb_prefix_scratch_clear(MDB_prefix_scratch *scratch)
{
	if (!scratch)
		return;
	if (scratch->snapshot) {
		free(scratch->snapshot);
		scratch->snapshot = NULL;
	}
	if (scratch->entries) {
		free(scratch->entries);
		scratch->entries = NULL;
	}
	if (scratch->keybuf) {
		free(scratch->keybuf);
		scratch->keybuf = NULL;
	}
	if (scratch->encodedbuf) {
		free(scratch->encodedbuf);
		scratch->encodedbuf = NULL;
	}
	mdb_prefix_stride_cache_clear(&scratch->stride_cache);
	scratch->snapshot_size = 0;
	scratch->entries_cap = 0;
	scratch->keybuf_size = 0;
	scratch->encodedbuf_size = 0;
	memset(&scratch->measure_cache, 0, sizeof(scratch->measure_cache));
}

static MDB_xcursor *
mdb_cursor_container(MDB_cursor *mc)
{
	if (!mc || !(mc->mc_flags & C_SUB))
		return NULL;
	MDB_xcursor *mx = (MDB_xcursor *)((char *)mc - offsetof(MDB_xcursor, mx_cursor));

	/* Guard against stack copies of sub-cursors. Real embedded xcursor cursors
	 * always point their db/dbx/dbflag fields back to the container. */
	if (&mx->mx_cursor != mc ||
	    mc->mc_db != &mx->mx_db ||
	    mc->mc_dbx != &mx->mx_dbx ||
	    mc->mc_dbflag != &mx->mx_dbflag)
		return NULL;

	return mx;
}

static size_t
mdb_prefix_page_capacity(MDB_cursor *mc, MDB_page *mp)
{
	MDB_env *env = mc->mc_txn->mt_env;

	if (!IS_SUBP(mp))
		return env->me_psize;

	MDB_xcursor *mx = mdb_cursor_container(mc);
	if (mx && mx->mx_inline_bytes) {
		return mx->mx_inline_bytes;
	}

	if (!(mc->mc_flags & C_SUB) && mc->mc_top >= 0) {
		MDB_node *node = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
		return NODEDSZ(node);
	}

	return env->me_psize;
}

static int mdb_prefix_inline_build_pair(MDB_cursor *mc, MDB_page *mp, size_t capacity,
		    const MDB_val *first, const MDB_val *second);

static int mdb_leaf_rebuild_after_trunk_insert(MDB_cursor *mc, MDB_page *mp,
		    const MDB_val *old_trunk, indx_t insert, const MDB_val *new_key,
		    MDB_val *new_data, unsigned int new_flags, MDB_page *ofp);
static int mdb_leaf_rebuild_measure(MDB_env *env, MDB_txn *txn,
		    MDB_prefix_rebuild_entry *entries, unsigned int count, size_t capacity,
		    size_t *used_out);
static int mdb_leaf_rebuild_apply(MDB_cursor *mc, MDB_page *mp,
		    MDB_prefix_rebuild_entry *entries, unsigned int count, size_t capacity);
static void mdb_leaf_refresh_xcursor(MDB_cursor *mc, MDB_page *mp, indx_t idx);
static void mdb_xcursor_init1(MDB_cursor *mc, MDB_node *node);

static int
mdb_leaf_rebuild_after_trunk_delete(MDB_cursor *mc, MDB_page *mp, indx_t removed,
	const MDB_val *old_trunk)
{
	MDB_env *env = mc->mc_txn->mt_env;
	MDB_txn *txn = mc->mc_txn;
	unsigned int total = NUMKEYS(mp);
	unsigned int remain;
	MDB_page *snapshot;
	MDB_prefix_rebuild_entry *entries = NULL;
	unsigned char *key_storage;
	unsigned char *key_cursor;
	unsigned int out, i;
	size_t total_key_bytes = 0;
	int rc = MDB_SUCCESS;
	size_t capacity = mdb_prefix_page_capacity(mc, mp);

	if (!IS_LEAF(mp) || IS_LEAF2(mp) || total == 0 || removed >= total)
		return MDB_SUCCESS;

	remain = total - 1;

	rc = mdb_prefix_ensure_snapshot(txn, env->me_psize, &snapshot);
	if (rc != MDB_SUCCESS)
		return rc;
	mdb_prefix_snapshot_capture(snapshot, mp, capacity);

	if (remain == 0) {
		rc = mdb_leaf_rebuild_apply(mc, mp, NULL, 0, capacity);
		if (rc == MDB_SUCCESS)
			mdb_leaf_refresh_xcursor(mc, mp, 0);
		return rc;
	}

	rc = mdb_prefix_ensure_entries(txn, remain, &entries);
	if (rc != MDB_SUCCESS)
		return rc;

	out = 0;
	for (i = 0; i < total; ++i) {
		MDB_node *src;
		if (i == removed)
			continue;
		src = NODEPTR(snapshot, i);
		entries[out].flags = src->mn_flags;
		entries[out].data_size = NODEDSZ(src);
		if (F_ISSET(src->mn_flags, F_BIGDATA))
			entries[out].data_payload = sizeof(pgno_t);
		else
			entries[out].data_payload = entries[out].data_size;
		entries[out].data_ptr = NODEDATA(src);
		entries[out].encoded_ksize = 0;
		entries[out].encoded_key = NODEKEY(snapshot, src);
		entries[out].encoded_len = src->mn_ksize;
		entries[out].encoded_used = 0;
		entries[out].shared_prefix = UINT16_MAX;

		size_t full_len = entries[out].encoded_len;
		if (i != 0 && old_trunk && old_trunk->mv_data &&
		    entries[out].encoded_len > 0) {
			size_t shared = 0, used = 0;
			if (mdb_prefix_decode_canonical(old_trunk,
			    entries[out].encoded_key, entries[out].encoded_len,
			    &shared, &used, &full_len)) {
				if (shared <= UINT16_MAX)
					entries[out].shared_prefix = (unsigned short)shared;
				entries[out].encoded_used = (unsigned short)used;
			}
		}

		entries[out].key.mv_size = full_len;
		total_key_bytes += full_len;
		++out;
	}

	rc = mdb_prefix_ensure_keybuf(txn, total_key_bytes, &key_storage);
	if (rc != MDB_SUCCESS)
		return rc;

	key_cursor = key_storage;
	out = 0;
	for (i = 0; i < total; ++i) {
		MDB_prefix_rebuild_entry *entry;
		if (i == removed)
			continue;
		entry = &entries[out];
		entry->key.mv_data = key_cursor;
		if (entry->key.mv_size) {
			if (i == 0) {
				size_t copy_len = entry->encoded_len;
				if (copy_len > entry->key.mv_size)
					copy_len = entry->key.mv_size;
				if (copy_len)
					memcpy(key_cursor, entry->encoded_key, copy_len);
				if (entry->key.mv_size > copy_len)
					memset(key_cursor + copy_len, 0,
					    entry->key.mv_size - copy_len);
			} else if (entry->shared_prefix != UINT16_MAX &&
			    entry->encoded_used <= entry->encoded_len &&
			    old_trunk && old_trunk->mv_data &&
			    entry->shared_prefix <= old_trunk->mv_size) {
				size_t shared = entry->shared_prefix;
				size_t suffix_len = entry->encoded_len - entry->encoded_used;
				if (shared)
					memcpy(key_cursor, old_trunk->mv_data, shared);
				if (suffix_len)
					memcpy(key_cursor + shared,
					    entry->encoded_key + entry->encoded_used,
					    suffix_len);
			} else {
				size_t copy_len = entry->encoded_len;
				if (copy_len > entry->key.mv_size)
					copy_len = entry->key.mv_size;
				if (copy_len)
					memcpy(key_cursor, entry->encoded_key, copy_len);
				if (entry->key.mv_size > copy_len)
					memset(key_cursor + copy_len, 0,
					    entry->key.mv_size - copy_len);
			}
		}
		key_cursor += entry->key.mv_size;
		entry->encoded_ksize = 0;
		++out;
	}

	rc = mdb_leaf_rebuild_measure(mc->mc_txn->mt_env, mc->mc_txn,
	    entries, remain, capacity, NULL);
	if (rc != MDB_SUCCESS)
		return rc;

	rc = mdb_leaf_rebuild_apply(mc, mp, entries, remain, capacity);
	if (rc != MDB_SUCCESS)
		return rc;

	{
		indx_t refresh_idx = removed;
		if (remain > 0 && refresh_idx >= remain)
			refresh_idx = remain - 1;
		mdb_leaf_refresh_xcursor(mc, mp, refresh_idx);
	}

	return MDB_SUCCESS;
}

static int
mdb_prefix_inline_measure_after_insert(MDB_cursor *mc, MDB_page *mp,
	size_t current_capacity, unsigned int insert, const MDB_val *new_key,
	MDB_val *new_data, unsigned int new_flags, size_t *needed_out)
{
	MDB_env *env = mc->mc_txn->mt_env;
	MDB_txn *txn = mc->mc_txn;
	MDB_prefix_scratch *scratch = &txn->mt_prefix;
	MDB_prefix_measure_cache *measure_cache = &scratch->measure_cache;
	unsigned int total = NUMKEYS(mp);
	unsigned int new_total;
	MDB_page *snapshot = NULL;
	MDB_prefix_rebuild_entry *entries;
	unsigned char *key_storage;
	unsigned char *key_cursor;
	unsigned char old_trunk_buf[MDB_KEYBUF_MAX];
	MDB_val old_trunk = {0, NULL};
	size_t total_key_bytes = 0;
	unsigned int i;
	int rc;
	int is_reserve = F_ISSET(new_flags, MDB_RESERVE);
	unsigned short node_flags =
	    (unsigned short)(new_flags & (F_BIGDATA|F_DUPDATA|F_SUBDATA));
	size_t needed = 0;
	if (measure_cache) {
		measure_cache->valid = 0;
		measure_cache->snapshot = NULL;
		measure_cache->snapshot_bytes = 0;
	}

	if (insert > total)
		insert = total;
	new_total = total + 1;

	rc = mdb_prefix_ensure_snapshot(txn, env->me_psize, &snapshot);
	if (rc != MDB_SUCCESS)
		return rc;

	mdb_prefix_snapshot_capture(snapshot, mp, current_capacity);

	rc = mdb_prefix_ensure_entries(txn, new_total, &entries);
	if (rc != MDB_SUCCESS)
		return rc;

	if (total > 0) {
		MDB_node *first = NODEPTR(snapshot, 0);
		if (first->mn_ksize > MDB_KEYBUF_MAX)
			return MDB_BAD_VALSIZE;
		old_trunk.mv_size = first->mn_ksize;
		old_trunk.mv_data = old_trunk_buf;
		memcpy(old_trunk_buf, NODEKEY(snapshot, first), old_trunk.mv_size);
	}

	int have_old_trunk = old_trunk.mv_data != NULL;
	const unsigned char *trunk_bytes = have_old_trunk ? (const unsigned char *)old_trunk.mv_data : NULL;
	size_t trunk_len = have_old_trunk ? old_trunk.mv_size : 0;

	for (i = 0; i < new_total; ++i) {
		if (i == insert) {
			entries[i].flags = node_flags;
			entries[i].data_size = new_data->mv_size;
			if (F_ISSET(new_flags, F_BIGDATA))
				entries[i].data_payload = sizeof(pgno_t);
			else
				entries[i].data_payload = new_data->mv_size;
			if (F_ISSET(new_flags, F_BIGDATA))
				entries[i].data_ptr = new_data->mv_data;
			else if (is_reserve)
				entries[i].data_ptr = NULL;
			else
				entries[i].data_ptr = new_data->mv_data;
			entries[i].key.mv_size = new_key->mv_size;
			entries[i].encoded_ksize = 0;
			entries[i].encoded_key = NULL;
			entries[i].encoded_len = 0;
			entries[i].encoded_used = 0;
			entries[i].shared_prefix = UINT16_MAX;
			total_key_bytes += entries[i].key.mv_size;
			continue;
		}

		unsigned int src_idx = (i < insert) ? i : i - 1;
		MDB_node *src = NODEPTR(snapshot, src_idx);

		entries[i].flags = src->mn_flags;
		entries[i].data_size = NODEDSZ(src);
		if (F_ISSET(src->mn_flags, F_BIGDATA))
			entries[i].data_payload = sizeof(pgno_t);
		else
			entries[i].data_payload = entries[i].data_size;
		entries[i].data_ptr = NODEDATA(src);
		entries[i].encoded_ksize = 0;
		entries[i].encoded_key = NODEKEY(snapshot, src);
		entries[i].encoded_len = src->mn_ksize;
		entries[i].encoded_used = 0;
		entries[i].shared_prefix = UINT16_MAX;

		if (src_idx == 0) {
			if (have_old_trunk &&
			    src->mn_ksize == old_trunk.mv_size &&
			    memcmp(NODEKEY(snapshot, src), old_trunk.mv_data, src->mn_ksize) == 0) {
				entries[i].key.mv_size = old_trunk.mv_size;
			} else {
				entries[i].key.mv_size = src->mn_ksize;
			}
		} else if (have_old_trunk &&
		    src->mn_ksize == old_trunk.mv_size &&
		    memcmp(NODEKEY(snapshot, src), old_trunk.mv_data, src->mn_ksize) == 0) {
			entries[i].key.mv_size = old_trunk.mv_size;
		} else {
			if (trunk_bytes && src->mn_ksize > 0) {
				size_t shared = 0, used = 0, full_len = entries[i].encoded_len;
				if (mdb_prefix_decode_canonical(&old_trunk,
				    entries[i].encoded_key, entries[i].encoded_len,
				    &shared, &used, &full_len)) {
					if (shared <= UINT16_MAX)
						entries[i].shared_prefix = (unsigned short)shared;
					entries[i].encoded_used = (unsigned short)used;
					entries[i].key.mv_size = full_len;
				} else {
					entries[i].key.mv_size = entries[i].encoded_len;
				}
			} else {
				entries[i].key.mv_size = entries[i].encoded_len;
			}
		}
		total_key_bytes += entries[i].key.mv_size;
	}

	rc = mdb_prefix_ensure_keybuf(txn, total_key_bytes, &key_storage);
	if (rc != MDB_SUCCESS)
		return rc;

	key_cursor = key_storage;
	for (i = 0; i < new_total; ++i) {
		if (entries[i].key.mv_size == 0) {
			entries[i].key.mv_data = key_cursor;
		} else if (i == insert) {
			memcpy(key_cursor, new_key->mv_data, entries[i].key.mv_size);
			entries[i].key.mv_data = key_cursor;
		} else {
			unsigned int src_idx = (i < insert) ? i : i - 1;
			if (src_idx == 0) {
				size_t copy_len = entries[i].encoded_len;
				if (copy_len > entries[i].key.mv_size)
					copy_len = entries[i].key.mv_size;
				if (copy_len)
					memcpy(key_cursor, entries[i].encoded_key, copy_len);
				if (entries[i].key.mv_size > copy_len)
					memset(key_cursor + copy_len, 0,
					    entries[i].key.mv_size - copy_len);
			} else if (entries[i].encoded_key &&
			    entries[i].shared_prefix != UINT16_MAX &&
			    trunk_bytes &&
			    entries[i].encoded_used <= entries[i].encoded_len &&
			    entries[i].shared_prefix <= trunk_len) {
				size_t shared = entries[i].shared_prefix;
				size_t suffix_len = entries[i].encoded_len - entries[i].encoded_used;
				if (shared)
					memcpy(key_cursor, trunk_bytes, shared);
				if (suffix_len)
					memcpy(key_cursor + shared,
					    entries[i].encoded_key + entries[i].encoded_used,
					    suffix_len);
			} else if (entries[i].encoded_key && entries[i].encoded_len) {
				size_t copy_len = entries[i].encoded_len;
				if (copy_len > entries[i].key.mv_size)
					copy_len = entries[i].key.mv_size;
				memcpy(key_cursor, entries[i].encoded_key, copy_len);
				if (entries[i].key.mv_size > copy_len)
					memset(key_cursor + copy_len,
					    0, entries[i].key.mv_size - copy_len);
			}
			entries[i].key.mv_data = key_cursor;
		}
		key_cursor += entries[i].key.mv_size;
	}

	rc = mdb_leaf_rebuild_measure(env, txn, entries, new_total, env->me_psize,
	    &needed);
	if (rc == MDB_SUCCESS && measure_cache) {
		measure_cache->entries = entries;
		measure_cache->keybuf = key_storage;
		measure_cache->cursor = mc;
		measure_cache->pgno = MP_PGNO(mp);
		measure_cache->count = new_total;
		measure_cache->insert = insert;
		measure_cache->node_flags = node_flags;
		measure_cache->is_reserve = is_reserve ? 1U : 0U;
		measure_cache->is_sub = (mc->mc_flags & C_SUB) ? 1U : 0U;
		measure_cache->key_bytes = total_key_bytes;
		measure_cache->insert_key_size = new_key ? new_key->mv_size : 0;
		measure_cache->insert_data_size = new_data ? new_data->mv_size : 0;
		measure_cache->snapshot = snapshot;
		measure_cache->snapshot_bytes = current_capacity;
		measure_cache->valid = 1;
	}
	if (needed_out)
		*needed_out = needed;
	return rc;
}

static int
mdb_leaf_rebuild_after_trunk_insert(MDB_cursor *mc, MDB_page *mp,
	const MDB_val *old_trunk, indx_t insert, const MDB_val *new_key,
	MDB_val *new_data, unsigned int new_flags, MDB_page *ofp)
{
	MDB_env *env = mc->mc_txn->mt_env;
	MDB_txn *txn = mc->mc_txn;
	MDB_prefix_scratch *scratch = &txn->mt_prefix;
	MDB_prefix_measure_cache *measure_cache =
	    scratch ? &scratch->measure_cache : NULL;
	unsigned int total = NUMKEYS(mp);
	unsigned int new_total;
	MDB_page *snapshot = NULL;
	MDB_prefix_rebuild_entry *entries;
	unsigned char *key_storage;
	unsigned char *key_cursor;
	size_t total_key_bytes = 0;
	unsigned int i;
	int rc = MDB_SUCCESS;
	int is_reserve = F_ISSET(new_flags, MDB_RESERVE);
	unsigned short node_flags = (unsigned short)(new_flags & (F_BIGDATA|F_DUPDATA|F_SUBDATA));
	size_t capacity = mdb_prefix_page_capacity(mc, mp);
	int reuse_cache = 0;
	if (!IS_LEAF(mp) || IS_LEAF2(mp))
		return MDB_SUCCESS;

	if (insert > total)
		insert = total;
	new_total = total + 1;

	rc = mdb_prefix_ensure_entries(txn, new_total, &entries);
	if (rc != MDB_SUCCESS)
		return rc;

	int have_old_trunk = old_trunk && old_trunk->mv_data;
	const MDB_node *snapshot_trunk = NULL;
	MDB_val trunk_ref = (MDB_val){0, NULL};
	const unsigned char *trunk_bytes = NULL;
	size_t trunk_len = 0;

	int cache_match = 0;
	if (measure_cache && measure_cache->valid &&
	    measure_cache->entries == entries &&
	    measure_cache->cursor == mc &&
	    measure_cache->pgno == MP_PGNO(mp) &&
	    measure_cache->count == new_total &&
	    measure_cache->insert == insert &&
	    measure_cache->node_flags == node_flags &&
	    measure_cache->is_reserve == (is_reserve ? 1U : 0U) &&
	    measure_cache->is_sub == ((mc->mc_flags & C_SUB) ? 1U : 0U) &&
	    measure_cache->insert_key_size == (new_key ? new_key->mv_size : 0) &&
	    measure_cache->insert_data_size == (new_data ? new_data->mv_size : 0)) {
		cache_match = 1;
		reuse_cache = 1;
		total_key_bytes = measure_cache->key_bytes;
		if (measure_cache->snapshot &&
		    measure_cache->snapshot_bytes == capacity)
			snapshot = measure_cache->snapshot;
	}

	if (!snapshot) {
		rc = mdb_prefix_ensure_snapshot(txn, env->me_psize, &snapshot);
		if (rc != MDB_SUCCESS)
			return rc;
		mdb_prefix_snapshot_capture(snapshot, mp, capacity);
	}

	if (have_old_trunk) {
		trunk_ref = *old_trunk;
	} else if (total > 0) {
		snapshot_trunk = NODEPTR(snapshot, 0);
		trunk_ref.mv_size = snapshot_trunk->mn_ksize;
		trunk_ref.mv_data = NODEKEY(snapshot, snapshot_trunk);
	}
	trunk_bytes = (const unsigned char *)trunk_ref.mv_data;
	trunk_len = trunk_ref.mv_size;

	if (!cache_match)
		reuse_cache = 0;
	if (measure_cache) {
		measure_cache->valid = 0;
		measure_cache->snapshot = NULL;
		measure_cache->snapshot_bytes = 0;
	}

	MDB_prefix_rebuild_entry *insert_entry = &entries[insert];
	insert_entry->flags = node_flags;

	insert_entry->data_size = new_data ? new_data->mv_size : 0;
	if (F_ISSET(new_flags, F_BIGDATA)) {
		if (!ofp)
			return MDB_PROBLEM;
		insert_entry->data_payload = sizeof(pgno_t);
		insert_entry->data_ptr = (unsigned char *)&ofp->mp_pgno;
	} else {
		insert_entry->data_payload = new_data ? new_data->mv_size : 0;
		if (is_reserve)
			insert_entry->data_ptr = NULL;
		else
			insert_entry->data_ptr = new_data ? new_data->mv_data : NULL;
	}

	insert_entry->key.mv_size = new_key ? new_key->mv_size : 0;
	insert_entry->encoded_ksize = 0;
	insert_entry->encoded_key = NULL;
	insert_entry->encoded_len = 0;
	insert_entry->encoded_used = 0;
	insert_entry->shared_prefix = UINT16_MAX;

	if (!reuse_cache) {
		for (i = 0; i < new_total; ++i) {
			if (i == insert)
				continue;

			unsigned int src_idx = (i < insert) ? i : i - 1;
			MDB_node *src = NODEPTR(snapshot, src_idx);

			entries[i].flags = src->mn_flags;
			entries[i].data_size = NODEDSZ(src);
			if (F_ISSET(src->mn_flags, F_BIGDATA))
				entries[i].data_payload = sizeof(pgno_t);
			else
				entries[i].data_payload = entries[i].data_size;
			entries[i].data_ptr = NODEDATA(src);
			entries[i].encoded_ksize = 0;
			entries[i].encoded_key = NODEKEY(snapshot, src);
			entries[i].encoded_len = src->mn_ksize;
			entries[i].encoded_used = 0;
			entries[i].shared_prefix = UINT16_MAX;

			if (src_idx == 0) {
				if (have_old_trunk &&
				    src->mn_ksize == old_trunk->mv_size &&
				    memcmp(NODEKEY(snapshot, src), old_trunk->mv_data, src->mn_ksize) == 0) {
					entries[i].key.mv_size = old_trunk->mv_size;
				} else {
					entries[i].key.mv_size = src->mn_ksize;
				}
			} else if (have_old_trunk &&
			    src->mn_ksize == old_trunk->mv_size &&
			    memcmp(NODEKEY(snapshot, src), old_trunk->mv_data, src->mn_ksize) == 0) {
				entries[i].key.mv_size = old_trunk->mv_size;
			} else {
				if (trunk_bytes && src->mn_ksize > 0) {
					size_t shared = 0, used = 0, full_len = entries[i].encoded_len;
					if (mdb_prefix_decode_canonical(&trunk_ref,
					    entries[i].encoded_key, entries[i].encoded_len,
					    &shared, &used, &full_len)) {
						if (shared <= UINT16_MAX)
							entries[i].shared_prefix = (unsigned short)shared;
						entries[i].encoded_used = (unsigned short)used;
						entries[i].key.mv_size = full_len;
					} else {
						entries[i].key.mv_size = entries[i].encoded_len;
					}
				} else {
					entries[i].key.mv_size = entries[i].encoded_len;
				}
			}
			total_key_bytes += entries[i].key.mv_size;
		}
	}
	total_key_bytes += insert_entry->key.mv_size;

	rc = mdb_prefix_ensure_keybuf(txn, total_key_bytes, &key_storage);
	if (rc != MDB_SUCCESS)
		return rc;

	key_cursor = key_storage;
	for (i = 0; i < new_total; ++i) {
		if (i == insert) {
			if (entries[i].key.mv_size)
				memcpy(key_cursor, new_key->mv_data, entries[i].key.mv_size);
		} else {
			unsigned int src_idx = (i < insert) ? i : i - 1;
			if (src_idx == 0) {
				size_t copy_len = entries[i].encoded_len;
				if (copy_len > entries[i].key.mv_size)
					copy_len = entries[i].key.mv_size;
				if (copy_len)
					memcpy(key_cursor, entries[i].encoded_key, copy_len);
				if (entries[i].key.mv_size > copy_len)
					memset(key_cursor + copy_len, 0,
					    entries[i].key.mv_size - copy_len);
			} else if (entries[i].encoded_key &&
			    entries[i].shared_prefix != UINT16_MAX &&
			    trunk_bytes &&
			    entries[i].encoded_used <= entries[i].encoded_len &&
			    entries[i].shared_prefix <= trunk_len) {
				size_t shared = entries[i].shared_prefix;
				size_t suffix_len = entries[i].encoded_len - entries[i].encoded_used;
				if (shared)
					memcpy(key_cursor, trunk_bytes, shared);
				if (suffix_len)
					memcpy(key_cursor + shared,
					    entries[i].encoded_key + entries[i].encoded_used,
					    suffix_len);
			} else if (entries[i].encoded_key && entries[i].encoded_len) {
				size_t copy_len = entries[i].encoded_len;
				if (copy_len > entries[i].key.mv_size)
					copy_len = entries[i].key.mv_size;
				memcpy(key_cursor, entries[i].encoded_key, copy_len);
				if (entries[i].key.mv_size > copy_len)
					memset(key_cursor + copy_len,
					    0, entries[i].key.mv_size - copy_len);
			}
		}
		entries[i].key.mv_data = key_cursor;
		key_cursor += entries[i].key.mv_size;
		entries[i].encoded_ksize = 0;
	}

	rc = mdb_leaf_rebuild_measure(env, txn, entries, new_total, capacity, NULL);
	if (rc != MDB_SUCCESS)
		return rc;

	rc = mdb_leaf_rebuild_apply(mc, mp, entries, new_total, capacity);
	if (rc != MDB_SUCCESS)
		return rc;

	mdb_leaf_refresh_xcursor(mc, mp, insert);

	if (is_reserve && !F_ISSET(new_flags, F_BIGDATA)) {
		MDB_node *inserted = NODEPTR(mp, insert);
		new_data->mv_data = NODEDATA(inserted);
	}

	return MDB_SUCCESS;
}

static int
mdb_leaf_rebuild_measure(MDB_env *env, MDB_txn *txn,
	MDB_prefix_rebuild_entry *entries, unsigned int count, size_t capacity,
	size_t *used_out)
{
	size_t lower_base;
	size_t used;
	size_t limit;
	unsigned int i;
	const MDB_val *trunk_full = NULL;
	int rc = MDB_SUCCESS;
	MDB_prefix_scratch *scratch = txn ? &txn->mt_prefix : NULL;
	unsigned char *encoded_storage = NULL;
	size_t total_encoded = 0;
	(void)env;

	limit = (capacity > PAGEBASE) ? (capacity - PAGEBASE) : 0;
	lower_base = (PAGEHDRSZ - PAGEBASE) + (size_t)count * sizeof(indx_t);
	used = lower_base;

	if (count == 0) {
		if (used_out)
			*used_out = used;
		if (used > limit)
			return MDB_PAGE_FULL;
		return MDB_SUCCESS;
	}

	trunk_full = &entries[0].key;
	for (i = 0; i < count; ++i) {
		size_t key_bytes;
		size_t node_bytes;
		size_t shared = 0;
		MDB_prefix_rebuild_entry *entry = &entries[i];

		if (i == 0)
			key_bytes = entry->key.mv_size;
		else
			key_bytes = mdb_leaf_encoded_size(trunk_full, &entry->key, &shared);

		if (i == 0) {
			entry->shared_prefix = UINT16_MAX;
			entry->encoded_used = 0;
		} else {
			if (shared <= UINT16_MAX)
				entry->shared_prefix = (unsigned short)shared;
			else
				entry->shared_prefix = UINT16_MAX;
			entry->encoded_used = (unsigned short)mdb_varint_length(shared);
		}

		if (key_bytes > UINT16_MAX)
			return MDB_BAD_VALSIZE;

		node_bytes = NODESIZE + EVEN(key_bytes) + entries[i].data_payload;
		node_bytes = EVEN(node_bytes);

		if (SIZE_MAX - used < node_bytes) {
			if (used_out)
				*used_out = SIZE_MAX;
			return MDB_PAGE_FULL;
		}

		used += node_bytes;
		entries[i].encoded_ksize = (unsigned short)key_bytes;
		total_encoded += key_bytes;
	}

	if (used_out)
		*used_out = used;

	if (used > limit)
		rc = MDB_PAGE_FULL;

	if (!scratch || rc != MDB_SUCCESS || total_encoded == 0)
		return rc;

	int erc = mdb_prefix_ensure_encbuf(txn, total_encoded, &encoded_storage);
	if (erc != MDB_SUCCESS)
		return erc;

	unsigned char *cursor = encoded_storage;
	for (i = 0; i < count; ++i) {
		MDB_prefix_rebuild_entry *entry = &entries[i];
		size_t key_bytes = entry->encoded_ksize;
		int have_shared_hint = (i != 0) &&
		    (entry->shared_prefix != UINT16_MAX) &&
		    entry->shared_prefix <= entry->key.mv_size;

		entry->encoded_key = cursor;
		entry->encoded_len = key_bytes;

		if (i == 0) {
			if (key_bytes && entry->key.mv_data)
				memcpy(cursor, entry->key.mv_data, key_bytes);
			entry->encoded_used = 0;
			entry->shared_prefix = UINT16_MAX;
		} else {
			size_t shared = 0;
			size_t wrote;
			if (have_shared_hint) {
				shared = entry->shared_prefix;
				wrote = mdb_leaf_encode_key_fast(&entry->key, shared, cursor);
				if (!entry->encoded_used)
					entry->encoded_used =
					    (unsigned short)mdb_varint_length(shared);
			} else {
				wrote = mdb_leaf_encode_key(trunk_full, &entry->key,
				    cursor, &shared);
				if (shared <= UINT16_MAX)
					entry->shared_prefix = (unsigned short)shared;
				else
					entry->shared_prefix = UINT16_MAX;
				entry->encoded_used = (unsigned short)mdb_varint_length(shared);
			}
			if (wrote != key_bytes)
				return MDB_CORRUPTED;
		}

		cursor += key_bytes;
	}

	return rc;
}

static int
mdb_leaf_rebuild_apply(MDB_cursor *mc, MDB_page *mp,
	MDB_prefix_rebuild_entry *entries, unsigned int count, size_t capacity)
{
	unsigned int i;
	size_t lower_base;
	size_t ofs;

	if (mc && mc->mc_txn)
		mdb_prefix_stride_entry_invalidate(mc->mc_txn, mp->mp_pgno);

	if (capacity <= PAGEBASE)
		return MDB_PAGE_FULL;

	if (!IS_LEAF(mp) || IS_LEAF2(mp)) {
		MP_LOWER(mp) = (PAGEHDRSZ - PAGEBASE);
		MP_UPPER(mp) = (indx_t)(capacity - PAGEBASE);
		if (!IS_LEAF2(mp))
			mdb_prefix_leaf_store_stride(mc, mp, 0);
		return MDB_SUCCESS;
	}

	MP_LOWER(mp) = (PAGEHDRSZ - PAGEBASE);
	MP_UPPER(mp) = (indx_t)(capacity - PAGEBASE);
	if (count == 0) {
		mdb_prefix_leaf_store_stride(mc, mp, 0);
		return MDB_SUCCESS;
	}

	lower_base = (PAGEHDRSZ - PAGEBASE) + (size_t)count * sizeof(indx_t);
	ofs = capacity - PAGEBASE;

	if (lower_base > ofs)
		return MDB_PAGE_FULL;

	{
		const MDB_val *trunk_full = &entries[0].key;
		int trunk_cached = entries[0].encoded_key != NULL;
		size_t max_decoded = 0;

		for (i = 0; i < count; ++i) {
			MDB_node *node;
			size_t key_bytes;
			size_t node_bytes;
			MDB_prefix_rebuild_entry *entry = &entries[i];

			if (entries && entry->key.mv_size > max_decoded)
				max_decoded = entry->key.mv_size;

			key_bytes = entry->encoded_ksize;
			node_bytes = NODESIZE + EVEN(key_bytes) + entry->data_payload;
			node_bytes = EVEN(node_bytes);

			if (node_bytes > ofs)
				return MDB_PAGE_FULL;
			ofs -= node_bytes;
			if (ofs < lower_base)
				return MDB_PAGE_FULL;
			MP_PTRS(mp)[i] = (indx_t)ofs;

			node = NODEPTR(mp, i);
			node->mn_ksize = key_bytes;
			node->mn_flags = entry->flags;
			mdb_node_set_count(mp, node, 0);
			SETDSZ(node, entry->data_size);

				if (entry->encoded_key &&
				    entry->encoded_len == key_bytes) {
					if (key_bytes)
						memcpy(NODEKEY(mp, node), entry->encoded_key, key_bytes);
				} else if (i == 0) {
					if (trunk_full->mv_size && trunk_full->mv_data)
						memcpy(NODEKEY(mp, node), trunk_full->mv_data,
						    trunk_full->mv_size);
				} else if (trunk_cached &&
				    entry->shared_prefix != UINT16_MAX &&
				    entry->shared_prefix <= trunk_full->mv_size &&
				    entry->key.mv_size >= entry->shared_prefix) {
					size_t shared = entry->shared_prefix;
					size_t suffix_len = entry->key.mv_size - shared;
					unsigned char *dst = NODEKEY(mp, node);
					size_t header = mdb_varint_encode(shared, dst);
					if (suffix_len)
						memcpy(dst + header,
						    (const unsigned char *)entry->key.mv_data + shared,
						    suffix_len);
				} else {
					mdb_leaf_encode_key(trunk_full, &entry->key,
					    NODEKEY(mp, node), NULL);
				}

			if (F_ISSET(entry->flags, F_BIGDATA)) {
				if (entry->data_ptr)
					memcpy(NODEDATA(node), entry->data_ptr, sizeof(pgno_t));
			} else if (entry->data_payload) {
				if (entry->data_ptr)
					memcpy(NODEDATA(node), entry->data_ptr, entry->data_payload);
				else
					memset(NODEDATA(node), 0, entry->data_payload);
			}
		}

		MP_UPPER(mp) = (indx_t)ofs;
		MP_LOWER(mp) = (PAGEHDRSZ - PAGEBASE) + count * sizeof(indx_t);

		if (entries)
			mdb_prefix_leaf_store_stride(mc, mp, max_decoded);
		else
			mdb_prefix_leaf_refresh_stride(mc, mp);
	}

	return MDB_SUCCESS;
}

static int
mdb_prefix_leaf_split_collect_entries(MDB_cursor *mc, MDB_page *mp,
	indx_t insert, const MDB_val *new_key, const MDB_val *new_data,
	unsigned int new_flags, MDB_prefix_rebuild_entry **entries_out,
	unsigned int *count_out)
{
	MDB_txn *txn = mc->mc_txn;
	unsigned int total = NUMKEYS(mp);
	unsigned int new_total;
	MDB_prefix_rebuild_entry *entries;
	unsigned char *key_storage;
	unsigned char *key_cursor;
	MDB_val old_trunk = {0, NULL};
	unsigned short node_flags =
	    (unsigned short)(new_flags & (F_BIGDATA|F_DUPDATA|F_SUBDATA));
	size_t total_key_bytes = 0;
	unsigned int i;
	int rc;

	if (insert > total)
		insert = total;
	new_total = total + 1;

	rc = mdb_prefix_ensure_entries(txn, new_total, &entries);
	if (rc != MDB_SUCCESS)
		return rc;

	if (total > 0) {
		MDB_node *first = NODEPTR(mp, 0);
		old_trunk.mv_size = first->mn_ksize;
		old_trunk.mv_data = NODEKEY(mp, first);
	}

	for (i = 0; i < new_total; ++i) {
		MDB_prefix_rebuild_entry *entry = &entries[i];
		if (i == insert) {
			entry->flags = node_flags;
			entry->data_size = new_data ? new_data->mv_size : 0;
			if (F_ISSET(node_flags, F_BIGDATA))
				entry->data_payload = sizeof(pgno_t);
			else
				entry->data_payload = entry->data_size;
			entry->data_ptr = new_data ? new_data->mv_data : NULL;
			entry->key.mv_size = new_key ? new_key->mv_size : 0;
			entry->encoded_ksize = 0;
			entry->encoded_key = NULL;
			entry->encoded_len = 0;
			entry->encoded_used = 0;
			entry->shared_prefix = UINT16_MAX;
		} else {
			unsigned int src_idx = (i < insert) ? i : i - 1;
			MDB_node *src = NODEPTR(mp, src_idx);

			entry->flags = src->mn_flags;
			entry->data_size = NODEDSZ(src);
			if (F_ISSET(src->mn_flags, F_BIGDATA))
				entry->data_payload = sizeof(pgno_t);
			else
				entry->data_payload = entry->data_size;
			entry->data_ptr = NODEDATA(src);
			entry->encoded_ksize = 0;
			entry->encoded_key = NODEKEY(mp, src);
			entry->encoded_len = src->mn_ksize;
			entry->encoded_used = 0;
			entry->shared_prefix = UINT16_MAX;
			if (src_idx == 0) {
				entry->key.mv_size = src->mn_ksize;
			} else {
				entry->key.mv_size = mdb_prefix_decoded_length(
				    old_trunk.mv_size, NODEKEY(mp, src), src->mn_ksize);
			}
		}

		if (SIZE_MAX - total_key_bytes < entry->key.mv_size)
			return MDB_PAGE_FULL;
		total_key_bytes += entry->key.mv_size;
	}

	rc = mdb_prefix_ensure_keybuf(txn, total_key_bytes, &key_storage);
	if (rc != MDB_SUCCESS)
		return rc;

	key_cursor = key_storage;
	for (i = 0; i < new_total; ++i) {
		MDB_prefix_rebuild_entry *entry = &entries[i];
		entry->key.mv_data = key_cursor;

		if (entry->key.mv_size) {
			if (i == insert) {
				if (!new_key || !new_key->mv_data)
					return MDB_BAD_VALSIZE;
				memcpy(key_cursor, new_key->mv_data, entry->key.mv_size);
			} else {
				unsigned int src_idx = (i < insert) ? i : i - 1;
				MDB_node *src = NODEPTR(mp, src_idx);

				if (src_idx == 0) {
					memcpy(key_cursor, NODEKEY(mp, src), entry->key.mv_size);
				} else {
					MDB_val decoded = { entry->key.mv_size, key_cursor };
					rc = mdb_leaf_decode_key(&old_trunk, NODEKEY(mp, src),
					    src->mn_ksize, &decoded, key_cursor,
					    entry->key.mv_size, 0, NULL);
					if (rc != MDB_SUCCESS)
						return rc;
					entry->key.mv_size = decoded.mv_size;
				}
			}
		}

		key_cursor += entry->key.mv_size;
	}

	if (entries_out)
		*entries_out = entries;
	if (count_out)
		*count_out = new_total;
	return MDB_SUCCESS;
}

static int
mdb_prefix_leaf_split_side_used(MDB_env *env,
	MDB_prefix_rebuild_entry *entries, unsigned int begin, unsigned int end,
	size_t *used_out)
{
	size_t used = (size_t)(end - begin) * sizeof(indx_t);
	const MDB_val *trunk = NULL;
	unsigned int i;

	if (begin < end)
		trunk = &entries[begin].key;

	for (i = begin; i < end; ++i) {
		MDB_prefix_rebuild_entry *entry = &entries[i];
		size_t key_bytes;
		size_t payload;
		size_t node_size;

		if (i == begin)
			key_bytes = entry->key.mv_size;
		else
			key_bytes = mdb_leaf_encoded_size(trunk, &entry->key, NULL);
		if (key_bytes > UINT16_MAX)
			return MDB_BAD_VALSIZE;

		node_size = NODESIZE + EVEN(key_bytes);

		if (F_ISSET(entry->flags, F_BIGDATA)) {
			payload = sizeof(pgno_t);
		} else {
			payload = entry->data_size;
			if (node_size + payload > env->me_nodemax) {
				int inline_dup = F_ISSET(entry->flags, F_DUPDATA) &&
				    !F_ISSET(entry->flags, F_SUBDATA);
				if (!inline_dup)
					payload = sizeof(pgno_t);
			}
		}

		node_size = EVEN(node_size + payload);
		if (SIZE_MAX - used < node_size)
			return MDB_PAGE_FULL;
		used += node_size;
	}

	if (used_out)
		*used_out = used;
	return MDB_SUCCESS;
}

static int
mdb_prefix_leaf_adjust_split_index(MDB_cursor *mc,
	MDB_prefix_rebuild_entry *entries, unsigned int total, indx_t newindx,
	int *split_indx)
{
	MDB_env *env = mc->mc_txn->mt_env;
	size_t pmax = env->me_psize - PAGEHDRSZ;
	int split;
	unsigned int attempts;
	(void)newindx;

	if (!split_indx)
		return EINVAL;

	if (total < 2)
		return MDB_SUCCESS;

	split = *split_indx;
	if (split < 1)
		split = 1;
	if (split > (int)(total - 1))
		split = (int)(total - 1);

	for (attempts = 0; attempts <= total; ++attempts) {
		size_t left_used = 0, right_used = 0;
		size_t left_over = 0, right_over = 0;
		int left_fit, right_fit;
		int rc;

		rc = mdb_prefix_leaf_split_side_used(env, entries, 0,
		    (unsigned int)split, &left_used);
		if (rc != MDB_SUCCESS)
			return rc;
		rc = mdb_prefix_leaf_split_side_used(env, entries, (unsigned int)split,
		    total, &right_used);
		if (rc != MDB_SUCCESS)
			return rc;

		left_fit = left_used <= pmax;
		right_fit = right_used <= pmax;
		if (left_fit && right_fit) {
			*split_indx = split;
			return MDB_SUCCESS;
		}

		if (!right_fit && split < (int)(total - 1)) {
			split++;
			continue;
		}
		if (!left_fit && split > 1) {
			split--;
			continue;
		}

		if (left_used > pmax)
			left_over = left_used - pmax;
		if (right_used > pmax)
			right_over = right_used - pmax;
		if (right_over >= left_over && split < (int)(total - 1)) {
			split++;
			continue;
		}
		if (split > 1) {
			split--;
			continue;
		}
		break;
	}

	return MDB_PAGE_FULL;
}

static void
mdb_leaf_refresh_xcursor(MDB_cursor *mc, MDB_page *mp, indx_t idx)
{
	MDB_xcursor *mx;
	unsigned int numkeys;
	MDB_page *parent;
	MDB_cursor *sub;

	if (mc == NULL)
		return;
	if (!(mc->mc_txn->mt_dbs[mc->mc_dbi].md_flags & MDB_DUPSORT))
		return;

	mx = mc->mc_xcursor;
	if (mx == NULL)
		return;

	if (mc->mc_top >= mc->mc_snum)
		return;

	parent = mc->mc_pg[mc->mc_top];
	if (IS_SUBP(mp)) {
		if (!parent || !IS_LEAF(parent))
			return;
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(parent))
			return;
		{
			MDB_node *node = NODEPTR(parent, mc->mc_ki[mc->mc_top]);
			if (!F_ISSET(node->mn_flags, F_DUPDATA))
				return;
			mdb_xcursor_init1(mc, node);
		}
		sub = &mx->mx_cursor;
		numkeys = NUMKEYS(mp);
		if (numkeys == 0) {
			sub->mc_flags &= ~(C_INITIALIZED|C_EOF);
			return;
		}
		if (idx >= numkeys)
			idx = numkeys - 1;
		sub->mc_pg[0] = mp;
		sub->mc_snum = 1;
		sub->mc_top = 0;
		sub->mc_ki[0] = idx;
		sub->mc_flags |= C_INITIALIZED;
		sub->mc_flags &= ~C_EOF;
		mx->mx_db.md_entries = numkeys;
		return;
	}

	if (mp != parent)
		return;

	if (!IS_LEAF(mp) || IS_LEAF2(mp)) {
		mx->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
		return;
	}

	numkeys = NUMKEYS(mp);
	if (numkeys == 0) {
		mx->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
		return;
	}

	if (idx >= numkeys)
		idx = numkeys - 1;

	mc->mc_ki[mc->mc_top] = idx;

	{
		MDB_node *node = NODEPTR(mp, idx);
		if (F_ISSET(node->mn_flags, F_DUPDATA)) {
			mdb_xcursor_init1(mc, node);
		} else {
			mx->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
		}
	}
}

static int	mdb_rebalance(MDB_cursor *mc);
static int	mdb_update_key(MDB_cursor *mc, MDB_val *key);

static void	mdb_cursor_pop(MDB_cursor *mc);
static int	mdb_cursor_push(MDB_cursor *mc, MDB_page *mp);
static void	mdb_cursor_copy(const MDB_cursor *csrc, MDB_cursor *cdst);

static int	_mdb_cursor_del(MDB_cursor *mc, unsigned int flags);
static int	_mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data, unsigned int flags);

static int	mdb_cursor_del0(MDB_cursor *mc);
static int	mdb_del0(MDB_txn *txn, MDB_dbi dbi, MDB_val *key, MDB_val *data, unsigned flags);
static int	mdb_cursor_sibling(MDB_cursor *mc, int move_right);
static int	mdb_cursor_next(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op);
static int	mdb_cursor_prev(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op);
static int	mdb_cursor_set(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op,
				int *exactp);
static int	mdb_cursor_first(MDB_cursor *mc, MDB_val *key, MDB_val *data);
static int	mdb_cursor_last(MDB_cursor *mc, MDB_val *key, MDB_val *data);

static void	mdb_cursor_init(MDB_cursor *mc, MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx);
static void	mdb_xcursor_init0(MDB_cursor *mc);
static void	mdb_xcursor_init2(MDB_cursor *mc, MDB_xcursor *src_mx, int force);
#ifdef MDB_VL32
static void mdb_cursor_unref(MDB_cursor *mc);
#endif

static int	mdb_drop0(MDB_cursor *mc, int subs);
static void mdb_default_cmp(MDB_txn *txn, MDB_dbi dbi);
static int mdb_reader_check0(MDB_env *env, int rlocked, int *dead);

static uint64_t
mdb_leaf_count(const MDB_page *mp)
{
	return NUMKEYS(mp);
}

static uint64_t
mdb_branch_sum(const MDB_page *mp)
{
	uint64_t total = 0;
	if (!IS_BRANCH(mp) || !IS_COUNTED(mp))
		return total;
	for (indx_t i = 0; i < NUMKEYS(mp); ++i)
		total += mdb_node_get_count(mp, NODEPTR(mp, i));
	return total;
}

static uint64_t
mdb_leaf_entry_contribution(const MDB_page *mp, const MDB_node *node)
{
	if (IS_LEAF2(mp))
		return 1;
	if (!IS_LEAF(mp))
		return 0;
	if (!(node->mn_flags & F_DUPDATA))
		return 1;
	if (node->mn_flags & F_SUBDATA) {
		MDB_db dbcopy;
		memcpy(&dbcopy, NODEDATA(node), sizeof(dbcopy));
		return dbcopy.md_entries;
	}
	const MDB_page *sub = (const MDB_page *)NODEDATA(node);
	return mdb_leaf_count(sub);
}

static uint64_t
mdb_leaf_prefix_contribution(MDB_cursor *mc, MDB_page *mp, indx_t limit)
{
	if (!IS_LEAF(mp) && !IS_LEAF2(mp))
		return 0;

	indx_t count = NUMKEYS(mp);
	if (limit > count)
		limit = count;
	if (IS_LEAF2(mp))
		return limit;
	if (limit == 0)
		return 0;

	MDB_txn *txn = mc ? mc->mc_txn : NULL;
	if (mc && txn && (txn->mt_flags & MDB_TXN_RDONLY) &&
	    (mc->mc_flags & C_LEAFCACHE) && !IS_SUBP(mp)) {
		MDB_cursor_leaf_cache *cache = &mc->mc_leaf_cache;
		int prc = MDB_SUCCESS;

	if (cache->decoded_pgno != MP_PGNO(mp) ||
		    cache->decoded_gen != txn->mt_txnid ||
		    cache->decoded_count != (unsigned int)count) {
			prc = mdb_cursor_leaf_cache_prepare(mc, mp);
		}

		if (prc == MDB_SUCCESS) {
			uint64_t *prefix = cache->decoded_prefix;
			unsigned int needed = (unsigned int)(count + 1);

			if (!prefix || cache->decoded_prefix_cap < needed) {
				int erc = mdb_cursor_leaf_cache_ensure_prefix(txn, cache,
				    needed, &prefix);
				if (erc != MDB_SUCCESS)
					prefix = NULL;
			}

			if (prefix) {
				unsigned int filled = cache->decoded_prefix_count;

				if (!filled) {
					prefix[0] = 0;
					filled = cache->decoded_prefix_count = 1;
				}
				if ((unsigned int)limit < filled)
					return prefix[limit];

				uint64_t running = prefix[filled - 1];
				unsigned int start = filled ? filled - 1 : 0;

				for (unsigned int i = start; i < (unsigned int)limit; ++i) {
					MDB_node *node = NODEPTR(mp, i);
					running += mdb_leaf_entry_contribution(mp, node);
					prefix[i + 1] = running;
				}
				cache->decoded_prefix_count = (unsigned int)limit + 1;
				return prefix[limit];
			}
		}
	}

	uint64_t total = 0;
	for (indx_t i = 0; i < limit; ++i) {
		MDB_node *node = NODEPTR(mp, i);
		total += mdb_leaf_entry_contribution(mp, node);
	}
	return total;
}

static uint64_t
mdb_leaf_contribution(const MDB_page *mp)
{
	if (IS_LEAF2(mp))
		return mdb_leaf_count(mp);
	if (!IS_LEAF(mp))
		return 0;
	uint64_t total = 0;
	for (indx_t i = 0; i < NUMKEYS(mp); ++i) {
		MDB_node *node = NODEPTR(mp, i);
		total += mdb_leaf_entry_contribution(mp, node);
	}
	return total;
}

static uint64_t
mdb_page_subtree_count(MDB_page *mp)
{
	if (IS_LEAF(mp) || IS_LEAF2(mp))
		return mdb_leaf_contribution(mp);
	if (IS_BRANCH(mp) && IS_COUNTED(mp))
		return mdb_branch_sum(mp);
	return 0;
}

static int64_t
mdb_update_parent_count(MDB_page *parent, pgno_t child_pgno, uint64_t total)
{
	if (!parent || !IS_BRANCH(parent) || !IS_COUNTED(parent))
		return 0;
	for (indx_t i = 0; i < NUMKEYS(parent); ++i) {
		MDB_node *node = NODEPTR(parent, i);
		if (NODEPGNO(node) == child_pgno) {
			uint64_t before = mdb_node_get_count(parent, node);
			if (before != total)
				mdb_node_set_count(parent, node, total);
			return (int64_t)total - (int64_t)before;
		}
	}
	return 0;
}

static void
mdb_propagate_count_delta(MDB_cursor *mc, int level, int64_t delta)
{
	if (!delta)
		return;
	for (int i = level; i >= 0; --i) {
		MDB_page *parent = mc->mc_pg[i];
		if (!parent)
			break;
		if (!IS_BRANCH(parent) || !IS_COUNTED(parent))
			continue;
		indx_t idx = mc->mc_ki[i];
		if (idx >= NUMKEYS(parent))
			continue;
		MDB_node *node = NODEPTR(parent, idx);
		uint64_t before = mdb_node_get_count(parent, node);
		int64_t acc = (int64_t)before + delta;
		if (acc < 0)
			acc = 0;
		mdb_node_set_count(parent, node, (uint64_t)acc);
	}
}

static void
mdb_adjust_counted_parents(MDB_cursor *mc, MDB_page *leaf,
	int split_performed, int64_t value_delta)
{
	if (!(mc->mc_db->md_flags & MDB_COUNTED))
		return;
	if (mc->mc_top <= 0 || !leaf)
		return;

	int level = mc->mc_top - 1;
	MDB_page *parent = mc->mc_pg[level];
	uint64_t leaf_total = 0;
	uint64_t before = 0;
	uint64_t after = 0;
	int64_t diff = 0;
	int slot_matched = 0;

	if (parent && IS_BRANCH(parent) && IS_COUNTED(parent)) {
		indx_t idx = mc->mc_ki[level];
		if (idx < NUMKEYS(parent)) {
			MDB_node *node = NODEPTR(parent, idx);
			if (NODEPGNO(node) == leaf->mp_pgno) {
				slot_matched = 1;
				before = mdb_node_get_count(parent, node);
				if (!split_performed) {
					if (value_delta) {
						int64_t tmp = (int64_t)before + value_delta;
						if (tmp < 0)
							tmp = 0;
						after = (uint64_t)tmp;
					} else {
						return;
					}
				} else {
					leaf_total = mdb_page_subtree_count(leaf);
					after = leaf_total;
				}
				if (after != before)
					mdb_node_set_count(parent, node, after);
				diff = (int64_t)after - (int64_t)before;
			}
		}
	}

	if (!slot_matched) {
		leaf_total = leaf_total ? leaf_total : mdb_page_subtree_count(leaf);
		diff = mdb_update_parent_count(parent, leaf->mp_pgno, leaf_total);
		if (!diff && value_delta)
			diff = value_delta;
	}

	if (diff)
		mdb_propagate_count_delta(mc, level - 1, diff);
}

static uint64_t mdb_branch_child_count(MDB_cursor *mc, MDB_page *mp, MDB_node *node);

static uint64_t
mdb_page_count(MDB_cursor *mc, MDB_page *mp)
{
	if (IS_LEAF(mp) || IS_LEAF2(mp))
		return mdb_leaf_count(mp);
	if (!IS_COUNTED(mp)) {
		uint64_t total = 0;
		for (indx_t i = 0; i < NUMKEYS(mp); ++i) {
			MDB_node *node = NODEPTR(mp, i);
			total += mdb_branch_child_count(mc, mp, node);
		}
		return total;
	}
	return mdb_branch_sum(mp);
}

static uint64_t
mdb_branch_child_count(MDB_cursor *mc, MDB_page *mp, MDB_node *node)
{
	uint64_t count = 0;
	MDB_page *child;
	if (mdb_page_get(mc, NODEPGNO(node), &child, NULL) == MDB_SUCCESS) {
		if (IS_LEAF(child) || IS_LEAF2(child))
			count = mdb_leaf_count(child);
		else if (IS_COUNTED(child))
			count = mdb_branch_sum(child);
		else
			count = mdb_page_count(mc, child);
	}
	return count;
}

static int
mdb_prefix_count(MDB_txn *txn, MDB_dbi dbi, const MDB_val *key,
	int inclusive, uint64_t *out)
{
	MDB_cursor mc = {0};
	int rc = MDB_SUCCESS;
	uint64_t total = 0;
	MDB_val search;
	MDB_cmp_func *cmp;

	if (!key) {
		*out = 0;
		return MDB_SUCCESS;
	}

	mdb_cursor_init(&mc, txn, dbi, NULL);
	search = *key;
	rc = mdb_cursor_set(&mc, &search, NULL, MDB_SET_RANGE, NULL);
	if (rc == MDB_NOTFOUND) {
		*out = txn->mt_dbs[dbi].md_entries;
		rc = MDB_SUCCESS;
		goto done;
	}
	if (rc != MDB_SUCCESS)
		goto done;
	cmp = txn->mt_dbxs[dbi].md_cmp;
	int exact = (cmp((MDB_val *)key, &search) == 0);

	for (int level = 0; level < mc.mc_top; ++level) {
		MDB_page *branch = mc.mc_pg[level];
		if (!IS_COUNTED(branch))
			continue;
		for (indx_t i = 0; i < mc.mc_ki[level]; ++i) {
			MDB_node *node = NODEPTR(branch, i);
			total += mdb_node_get_count(branch, node);
		}
	}

	MDB_page *leaf = mc.mc_pg[mc.mc_top];
	indx_t idx = mc.mc_ki[mc.mc_top];
	total += mdb_leaf_prefix_contribution(&mc, leaf, idx);
	if (inclusive && exact && idx < NUMKEYS(leaf)) {
		MDB_node *node = NODEPTR(leaf, idx);
		total += mdb_leaf_entry_contribution(leaf, node);
	}

	*out = total;
	rc = MDB_SUCCESS;

done:
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
#ifdef MDB_VL32
	mdb_cursor_unref(&mc);
#endif
	return rc;
}

static int
mdb_dup_prefix_count_internal(const MDB_cursor *base, MDB_cursor *cur,
	MDB_cmp_func *cmp, const MDB_val *value, int inclusive, uint64_t *out)
{
	MDB_val search, data = {0};
	uint64_t total = 0;
	int rc, exact = 0;
	uint64_t entries = base->mc_db ? (uint64_t)base->mc_db->md_entries : 0;

	*out = 0;
	if (!value)
		return MDB_SUCCESS;
	if (!(cur->mc_flags & C_INITIALIZED) || !cur->mc_snum)
		return MDB_SUCCESS;

	search = *value;
	rc = mdb_cursor_set(cur, &search, &data, MDB_SET_RANGE, &exact);
	if (rc == MDB_NOTFOUND) {
		MDB_val edge_key = {0}, edge_data = {0};
		int frc = mdb_cursor_get(cur, &edge_key, &edge_data, MDB_FIRST);
		if (frc == MDB_SUCCESS) {
			int cmp_first = cmp(&edge_key, (MDB_val *)value);
			if (cmp_first > 0 || (cmp_first == 0 && !inclusive)) {
				*out = 0;
				return MDB_SUCCESS;
			}
		} else if (frc != MDB_NOTFOUND) {
			return frc;
		}

		frc = mdb_cursor_get(cur, &edge_key, &edge_data, MDB_LAST);
		if (frc == MDB_SUCCESS) {
			int cmp_last = cmp(&edge_key, (MDB_val *)value);
			if (cmp_last < 0 || (cmp_last == 0 && inclusive)) {
				*out = entries;
				return MDB_SUCCESS;
			}
		} else if (frc != MDB_NOTFOUND) {
			return frc;
		}

		*out = entries;
		return MDB_SUCCESS;
	}
	if (rc != MDB_SUCCESS)
		return rc;
	if (!(cur->mc_flags & C_INITIALIZED) || !cur->mc_snum)
		return MDB_SUCCESS;

	for (int level = 0; level < cur->mc_top; ++level) {
		MDB_page *branch = cur->mc_pg[level];
		indx_t limit = cur->mc_ki[level];
		if (IS_BRANCH(branch) && IS_COUNTED(branch)) {
			for (indx_t i = 0; i < limit; ++i) {
				MDB_node *node = NODEPTR(branch, i);
				total += mdb_node_get_count(branch, node);
			}
		} else if (IS_BRANCH(branch)) {
			for (indx_t i = 0; i < limit; ++i) {
				MDB_node *node = NODEPTR(branch, i);
				total += mdb_branch_child_count(cur, branch, node);
			}
		}
	}

	MDB_page *leaf = cur->mc_pg[cur->mc_top];
	indx_t leaf_idx = cur->mc_ki[cur->mc_top];
	total += mdb_leaf_prefix_contribution(cur, leaf, leaf_idx);
	if (inclusive && exact && leaf_idx < NUMKEYS(leaf)) {
		MDB_node *node = NODEPTR(leaf, leaf_idx);
		total += mdb_leaf_entry_contribution(leaf, node);
	}

	*out = total;
	return MDB_SUCCESS;
}

/** @cond */
static MDB_cmp_func	mdb_cmp_memn, mdb_cmp_memnr, mdb_cmp_int, mdb_cmp_cint, mdb_cmp_long;
/** @endcond */

/** Compare two items pointing at '#mdb_size_t's of unknown alignment. */
#ifdef MISALIGNED_OK
# define mdb_cmp_clong mdb_cmp_long
#else
# define mdb_cmp_clong mdb_cmp_cint
#endif

/** True if we need #mdb_cmp_clong() instead of \b cmp for #MDB_INTEGERDUP */
#define NEED_CMP_CLONG(cmp, ksize) \
	(UINT_MAX < MDB_SIZE_MAX && \
	 (cmp) == mdb_cmp_int && (ksize) == sizeof(mdb_size_t))

#ifdef _WIN32
static SECURITY_DESCRIPTOR mdb_null_sd;
static SECURITY_ATTRIBUTES mdb_all_sa;
static int mdb_sec_inited;

struct MDB_name;
static int utf8_to_utf16(const char *src, struct MDB_name *dst, int xtra);
#endif

/** Return the library version info. */
char * ESECT
mdb_version(int *major, int *minor, int *patch)
{
	if (major) *major = MDB_VERSION_MAJOR;
	if (minor) *minor = MDB_VERSION_MINOR;
	if (patch) *patch = MDB_VERSION_PATCH;
	return MDB_VERSION_STRING;
}

/** Table of descriptions for LMDB @ref errors */
static char *const mdb_errstr[] = {
	"MDB_KEYEXIST: Key/data pair already exists",
	"MDB_NOTFOUND: No matching key/data pair found",
	"MDB_PAGE_NOTFOUND: Requested page not found",
	"MDB_CORRUPTED: Located page was wrong type",
	"MDB_PANIC: Update of meta page failed or environment had fatal error",
	"MDB_VERSION_MISMATCH: Database environment version mismatch",
	"MDB_INVALID: File is not an LMDB file",
	"MDB_MAP_FULL: Environment mapsize limit reached",
	"MDB_DBS_FULL: Environment maxdbs limit reached",
	"MDB_READERS_FULL: Environment maxreaders limit reached",
	"MDB_TLS_FULL: Thread-local storage keys full - too many environments open",
	"MDB_TXN_FULL: Transaction has too many dirty pages - transaction too big",
	"MDB_CURSOR_FULL: Internal error - cursor stack limit reached",
	"MDB_PAGE_FULL: Internal error - page has no more space",
	"MDB_MAP_RESIZED: Database contents grew beyond environment mapsize",
	"MDB_INCOMPATIBLE: Operation and DB incompatible, or DB flags changed",
	"MDB_BAD_RSLOT: Invalid reuse of reader locktable slot",
	"MDB_BAD_TXN: Transaction must abort, has a child, or is invalid",
	"MDB_BAD_VALSIZE: Unsupported size of key/DB name/data, or wrong DUPFIXED size",
	"MDB_BAD_DBI: The specified DBI handle was closed/changed unexpectedly",
	"MDB_PROBLEM: Unexpected problem - txn should abort",
};

char *
mdb_strerror(int err)
{
#ifdef _WIN32
	/** Use Thread Local Storage for the buffer to avoid returning a stack pointer.
	 */
#define MSGSIZE	1024
#define PADSIZE	4096
	static __declspec(thread) char buf[MSGSIZE+PADSIZE];
	char *ptr = buf;
#endif
	int i;
	if (!err)
		return ("Successful return: 0");

	if (err >= MDB_KEYEXIST && err <= MDB_LAST_ERRCODE) {
		i = err - MDB_KEYEXIST;
		return mdb_errstr[i];
	}

#ifdef _WIN32
	/* These are the C-runtime error codes we use. The comment indicates
	 * their numeric value, and the Win32 error they would correspond to
	 * if the error actually came from a Win32 API. A major mess, we should
	 * have used LMDB-specific error codes for everything.
	 */
	switch(err) {
	case ENOENT:	/* 2, FILE_NOT_FOUND */
	case EIO:		/* 5, ACCESS_DENIED */
	case ENOMEM:	/* 12, INVALID_ACCESS */
	case EACCES:	/* 13, INVALID_DATA */
	case EBUSY:		/* 16, CURRENT_DIRECTORY */
	case EINVAL:	/* 22, BAD_COMMAND */
	case ENOSPC:	/* 28, OUT_OF_PAPER */
		return strerror(err);
	default:
		;
	}
	buf[0] = 0;
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, err, 0, ptr, MSGSIZE, NULL);
	return ptr;
#else
	if (err < 0)
		return "Invalid error code";
	return strerror(err);
#endif
}

/** assert(3) variant in cursor context */
#define mdb_cassert(mc, expr)	mdb_assert0((mc)->mc_txn->mt_env, expr, #expr)
/** assert(3) variant in transaction context */
#define mdb_tassert(txn, expr)	mdb_assert0((txn)->mt_env, expr, #expr)
/** assert(3) variant in environment context */
#define mdb_eassert(env, expr)	mdb_assert0(env, expr, #expr)

#ifndef NDEBUG
# define mdb_assert0(env, expr, expr_txt) ((expr) ? (void)0 : \
		mdb_assert_fail(env, expr_txt, mdb_func_, __FILE__, __LINE__))

static void ESECT
mdb_assert_fail(MDB_env *env, const char *expr_txt,
	const char *func, const char *file, int line)
{
	char buf[400];
	sprintf(buf, "%.100s:%d: Assertion '%.200s' failed in %.40s()",
		file, line, expr_txt, func);
	if (env->me_assert_func)
		env->me_assert_func(env, buf);
	fprintf(stderr, "%s\n", buf);
	abort();
}
#else
# define mdb_assert0(env, expr, expr_txt) ((void) 0)
#endif /* NDEBUG */

static indx_t
mdb_page_insert_slot(MDB_cursor *mc, MDB_page *mp, indx_t indx, size_t node_size)
{
	unsigned int numkeys = NUMKEYS(mp);
	indx_t *ptrs = MP_PTRS(mp);
	indx_t delta = (indx_t)node_size;

	mdb_cassert(mc, node_size == (size_t)delta);
	mdb_cassert(mc, indx <= numkeys);

	for (unsigned int i = numkeys; i > indx; --i)
		ptrs[i] = ptrs[i - 1];

	indx_t ofs = MP_UPPER(mp) - delta;
	mdb_cassert(mc, ofs >= MP_LOWER(mp) + sizeof(indx_t));

	ptrs[indx] = ofs;
	MP_UPPER(mp) = ofs;
	MP_LOWER(mp) += sizeof(indx_t);

	return ofs;
}

static void
mdb_page_remove_slot(MDB_cursor *mc, MDB_page *mp, indx_t indx, size_t node_size)
{
	unsigned int numkeys = NUMKEYS(mp);
	indx_t *ptrs = MP_PTRS(mp);
	indx_t delta = (indx_t)node_size;

	mdb_cassert(mc, node_size == (size_t)delta);
	mdb_cassert(mc, indx < numkeys);

	indx_t ofs = ptrs[indx];

	for (unsigned int i = indx; i + 1 < numkeys; ++i)
		ptrs[i] = ptrs[i + 1];

	for (unsigned int i = 0; i < numkeys - 1; ++i) {
		if (ptrs[i] < ofs)
			ptrs[i] += delta;
	}

	unsigned char *base = (unsigned char *)mp + MP_UPPER(mp) + PAGEBASE;
	size_t span = (size_t)(ofs - MP_UPPER(mp));
	if (span)
		memmove(base + delta, base, span);

	MP_LOWER(mp) -= sizeof(indx_t);
	MP_UPPER(mp) += delta;
}

#if MDB_DEBUG
/** Return the page number of \b mp which may be sub-page, for debug output */
static pgno_t
mdb_dbg_pgno(MDB_page *mp)
{
	pgno_t ret;
	ret = MP_PGNO(mp);
	return ret;
}

/** Display a key in hexadecimal and return the address of the result.
 * @param[in] key the key to display
 * @param[in] buf the buffer to write into. Should always be #DKBUF.
 * @return The key in hexadecimal form.
 */
char *
mdb_dkey(MDB_val *key, char *buf)
{
	char *ptr = buf;
	unsigned char *c = key->mv_data;
	unsigned int i;

	if (!key)
		return "";

	if (key->mv_size > DKBUF_MAXKEYSIZE)
		return "MDB_MAXKEYSIZE";
	/* may want to make this a dynamic check: if the key is mostly
	 * printable characters, print it as-is instead of converting to hex.
	 */
#if 1
	buf[0] = '\0';
	for (i=0; i<key->mv_size; i++)
		ptr += sprintf(ptr, "%02x", *c++);
#else
	sprintf(buf, "%.*s", key->mv_size, key->mv_data);
#endif
	return buf;
}

static char *
mdb_dval(MDB_txn *txn, MDB_dbi dbi, MDB_val *data, char *buf)
{
	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT) {
		mdb_dkey(data, buf+1);
		*buf = '[';
		strcpy(buf + data->mv_size * 2 + 1, "]");
	} else
		*buf = '\0';
	return buf;
}

static const char *
mdb_leafnode_type(MDB_node *n)
{
	static char *const tp[2][2] = {{"", ": DB"}, {": sub-page", ": sub-DB"}};
	return F_ISSET(n->mn_flags, F_BIGDATA) ? ": overflow page" :
		tp[F_ISSET(n->mn_flags, F_DUPDATA)][F_ISSET(n->mn_flags, F_SUBDATA)];
}

/** Display all the keys in the page. */
void
mdb_page_list(MDB_page *mp)
{
	pgno_t pgno = mdb_dbg_pgno(mp);
	const char *type, *state = (MP_FLAGS(mp) & P_DIRTY) ? ", dirty" : "";
	MDB_node *node;
	unsigned int i, nkeys, nsize, total = 0;
	MDB_val key;
	DKBUF;

	switch (MP_FLAGS(mp) & (P_BRANCH|P_LEAF|P_LEAF2|P_META|P_OVERFLOW|P_SUBP)) {
	case P_BRANCH:              type = "Branch page";		break;
	case P_LEAF:                type = "Leaf page";			break;
	case P_LEAF|P_SUBP:         type = "Sub-page";			break;
	case P_LEAF|P_LEAF2:        type = "LEAF2 page";		break;
	case P_LEAF|P_LEAF2|P_SUBP: type = "LEAF2 sub-page";	break;
	case P_OVERFLOW:
		fprintf(stderr, "Overflow page %"Yu" pages %u%s\n",
			pgno, mp->mp_pages, state);
		return;
	case P_META:
		fprintf(stderr, "Meta-page %"Yu" txnid %"Yu"\n",
			pgno, ((MDB_meta *)METADATA(mp))->mm_txnid);
		return;
	default:
		fprintf(stderr, "Bad page %"Yu" flags 0x%X\n", pgno, MP_FLAGS(mp));
		return;
	}

	nkeys = NUMKEYS(mp);
	fprintf(stderr, "%s %"Yu" numkeys %d%s\n", type, pgno, nkeys, state);

	for (i=0; i<nkeys; i++) {
		if (IS_LEAF2(mp)) {	/* LEAF2 pages have no mp_ptrs[] or node headers */
			key.mv_size = nsize = mp->mp_pad;
			key.mv_data = LEAF2KEY(mp, i, nsize);
			total += nsize;
			fprintf(stderr, "key %d: nsize %d, %s\n", i, nsize, DKEY(&key));
			continue;
		}
		node = NODEPTR(mp, i);
		key.mv_size = node->mn_ksize;
		key.mv_data = node->mn_data;
		nsize = NODESIZE + key.mv_size;
		if (IS_BRANCH(mp)) {
			fprintf(stderr, "key %d: page %"Yu", %s\n", i, NODEPGNO(node),
				DKEY(&key));
			total += nsize;
		} else {
			if (F_ISSET(node->mn_flags, F_BIGDATA))
				nsize += sizeof(pgno_t);
			else
				nsize += NODEDSZ(node);
			total += nsize;
			nsize += sizeof(indx_t);
			fprintf(stderr, "key %d: nsize %d, %s%s\n",
				i, nsize, DKEY(&key), mdb_leafnode_type(node));
		}
		total = EVEN(total);
	}
	fprintf(stderr, "Total: header %d + contents %d + unused %d\n",
		IS_LEAF2(mp) ? PAGEHDRSZ : PAGEBASE + MP_LOWER(mp), total, SIZELEFT(mp));
}

void
mdb_cursor_chk(MDB_cursor *mc)
{
	unsigned int i;
	MDB_node *node;
	MDB_page *mp;

	if (!mc->mc_snum || !(mc->mc_flags & C_INITIALIZED)) return;
	for (i=0; i<mc->mc_top; i++) {
		mp = mc->mc_pg[i];
		node = NODEPTR(mp, mc->mc_ki[i]);
		if (NODEPGNO(node) != mc->mc_pg[i+1]->mp_pgno)
			printf("oops!\n");
	}
	if (mc->mc_ki[i] >= NUMKEYS(mc->mc_pg[i]))
		printf("ack!\n");
	if (XCURSOR_INITED(mc)) {
		node = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
		if (((node->mn_flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA) &&
			mc->mc_xcursor->mx_cursor.mc_pg[0] != NODEDATA(node)) {
			printf("blah!\n");
		}
	}
}
#endif

#if (MDB_DEBUG) > 2
/** Count all the pages in each DB and in the freelist
 *  and make sure it matches the actual number of pages
 *  being used.
 *  All named DBs must be open for a correct count.
 */
static void mdb_audit(MDB_txn *txn)
{
	MDB_cursor mc = (MDB_cursor){0};
	MDB_xcursor free_mx = (MDB_xcursor){0};
	MDB_val key, data;
	MDB_ID freecount, count;
	MDB_dbi i;
	int rc;

	freecount = 0;
	mdb_cursor_init(&mc, txn, FREE_DBI, &free_mx);
	while ((rc = mdb_cursor_get(&mc, &key, &data, MDB_NEXT)) == 0)
		freecount += *(MDB_ID *)data.mv_data;
	mdb_tassert(txn, rc == MDB_NOTFOUND);

	count = 0;
	for (i = 0; i<txn->mt_numdbs; i++) {
		MDB_xcursor mx = (MDB_xcursor){0};
		if (!(txn->mt_dbflags[i] & DB_VALID))
			continue;
		mdb_cursor_init(&mc, txn, i, &mx);
		if (txn->mt_dbs[i].md_root == P_INVALID)
			continue;
		count += txn->mt_dbs[i].md_branch_pages +
			txn->mt_dbs[i].md_leaf_pages +
			txn->mt_dbs[i].md_overflow_pages;
		if (txn->mt_dbs[i].md_flags & MDB_DUPSORT) {
			rc = mdb_page_search(&mc, NULL, MDB_PS_FIRST);
			for (; rc == MDB_SUCCESS; rc = mdb_cursor_sibling(&mc, 1)) {
				unsigned j;
				MDB_page *mp;
				mp = mc.mc_pg[mc.mc_top];
				for (j=0; j<NUMKEYS(mp); j++) {
					MDB_node *leaf = NODEPTR(mp, j);
					if (leaf->mn_flags & F_SUBDATA) {
						MDB_db db;
						memcpy(&db, NODEDATA(leaf), sizeof(db));
						count += db.md_branch_pages + db.md_leaf_pages +
							db.md_overflow_pages;
					}
				}
			}
			mdb_tassert(txn, rc == MDB_NOTFOUND);
		}
	}
	if (freecount + count + NUM_METAS != txn->mt_next_pgno) {
		fprintf(stderr, "audit: %"Yu" freecount: %"Yu" count: %"Yu" total: %"Yu" next_pgno: %"Yu"\n",
			txn->mt_txnid, freecount, count+NUM_METAS,
			freecount+count+NUM_METAS, txn->mt_next_pgno);
	}
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
}
#endif

int
mdb_cmp(MDB_txn *txn, MDB_dbi dbi, const MDB_val *a, const MDB_val *b)
{
	return txn->mt_dbxs[dbi].md_cmp(a, b);
}

int
mdb_dcmp(MDB_txn *txn, MDB_dbi dbi, const MDB_val *a, const MDB_val *b)
{
	MDB_cmp_func *dcmp = txn->mt_dbxs[dbi].md_dcmp;
	if (NEED_CMP_CLONG(dcmp, a->mv_size))
		dcmp = mdb_cmp_clong;
	return dcmp(a, b);
}

/** Allocate memory for a page.
 * Re-use old malloc'd pages first for singletons, otherwise just malloc.
 * Set #MDB_TXN_ERROR on failure.
 */
static MDB_page *
mdb_page_malloc(MDB_txn *txn, unsigned num)
{
	MDB_env *env = txn->mt_env;
	MDB_page *ret = env->me_dpages;
	size_t psize = env->me_psize, sz = psize, off;
	/* For ! #MDB_NOMEMINIT, psize counts how much to init.
	 * For a single page alloc, we init everything after the page header.
	 * For multi-page, we init the final page; if the caller needed that
	 * many pages they will be filling in at least up to the last page.
	 */
	if (num == 1) {
		if (ret) {
			VGMEMP_ALLOC(env, ret, sz);
			VGMEMP_DEFINED(ret, sizeof(ret->mp_next));
			env->me_dpages = ret->mp_next;
			return ret;
		}
		psize -= off = PAGEHDRSZ;
	} else {
		sz *= num;
		off = sz - psize;
	}
	if ((ret = malloc(sz)) != NULL) {
		VGMEMP_ALLOC(env, ret, sz);
		if (!(env->me_flags & MDB_NOMEMINIT)) {
			memset((char *)ret + off, 0, psize);
			ret->mp_pad = 0;
		}
	} else {
		mdb_txn_mark_error(txn, ENOMEM);
	}
	return ret;
}
/** Free a single page.
 * Saves single pages to a list, for future reuse.
 * (This is not used for multi-page overflow pages.)
 */
static void
mdb_page_free(MDB_env *env, MDB_page *mp)
{
	mp->mp_next = env->me_dpages;
	VGMEMP_FREE(env, mp);
	env->me_dpages = mp;
}

/** Free a dirty page */
static void
mdb_dpage_free(MDB_env *env, MDB_page *dp)
{
	if (!IS_OVERFLOW(dp) || dp->mp_pages == 1) {
		mdb_page_free(env, dp);
	} else {
		/* large pages just get freed directly */
		VGMEMP_FREE(env, dp);
		free(dp);
	}
}

/**	Return all dirty pages to dpage list */
static void
mdb_dlist_free(MDB_txn *txn)
{
	MDB_env *env = txn->mt_env;
	MDB_ID2L dl = txn->mt_u.dirty_list;
	unsigned i, n = dl[0].mid;

	for (i = 1; i <= n; i++) {
		mdb_dpage_free(env, dl[i].mptr);
	}
	dl[0].mid = 0;
}

#ifdef MDB_VL32
static void
mdb_page_unref(MDB_txn *txn, MDB_page *mp)
{
	pgno_t pgno;
	MDB_ID3L tl = txn->mt_rpages;
	unsigned x, rem;
	if (mp->mp_flags & (P_SUBP|P_DIRTY))
		return;
	rem = mp->mp_pgno & (MDB_RPAGE_CHUNK-1);
	pgno = mp->mp_pgno ^ rem;
	x = mdb_mid3l_search(tl, pgno);
	if (x != tl[0].mid && tl[x+1].mid == mp->mp_pgno)
		x++;
	if (tl[x].mref)
		tl[x].mref--;
}
#define MDB_PAGE_UNREF(txn, mp)	mdb_page_unref(txn, mp)

static void
mdb_cursor_unref(MDB_cursor *mc)
{
	int i;
	if (mc->mc_txn->mt_rpages[0].mid) {
		if (!mc->mc_snum || !mc->mc_pg[0] || IS_SUBP(mc->mc_pg[0]))
			return;
		for (i=0; i<mc->mc_snum; i++)
			mdb_page_unref(mc->mc_txn, mc->mc_pg[i]);
		if (mc->mc_ovpg) {
			mdb_page_unref(mc->mc_txn, mc->mc_ovpg);
			mc->mc_ovpg = 0;
		}
	}
	mc->mc_snum = mc->mc_top = 0;
	mc->mc_pg[0] = NULL;
	mc->mc_key_pgno = P_INVALID;
	mc->mc_key_last = (indx_t)~0;
	mdb_cursor_seq_invalidate(mc);
	mdb_cursor_leaf_cache_reset(&mc->mc_leaf_cache);
	mc->mc_flags &= ~C_INITIALIZED;
}
#define MDB_CURSOR_UNREF(mc, force) \
	(((force) || ((mc)->mc_flags & C_INITIALIZED)) \
	 ? mdb_cursor_unref(mc) \
	 : (void)0)

#else
#define MDB_PAGE_UNREF(txn, mp)
#define MDB_CURSOR_UNREF(mc, force) ((void)0)
#endif /* MDB_VL32 */

static int
mdb_prefix_pair_leq_cursor(MDB_cursor *mc,
	const MDB_val *key, int key_inclusive,
	const MDB_val *value, int value_inclusive,
	uint64_t *out)
{
	MDB_db *db;
	MDB_cmp_func *kcmp;
	MDB_cmp_func *dcmp;
	MDB_val search = {0};
	MDB_val data = {0};
	uint64_t total = 0;
	int rc;
	int exact = 0;

	if (!mc || !out)
		return EINVAL;

	db = mc->mc_db;
	if (!db)
		return MDB_PROBLEM;

	kcmp = mc->mc_dbx ? mc->mc_dbx->md_cmp : NULL;
	if (!kcmp)
		return MDB_PROBLEM;
	dcmp = (mc->mc_dbx && mc->mc_dbx->md_dcmp) ?
	    mc->mc_dbx->md_dcmp : kcmp;

	if (!key) {
		*out = key_inclusive ? db->md_entries : 0;
		return MDB_SUCCESS;
	}

	search = *key;
	rc = mdb_cursor_set(mc, &search, &data, MDB_SET_RANGE, &exact);
	if (rc == MDB_NOTFOUND) {
		MDB_val edge_key = {0}, edge_data = {0};
		int frc;

		if (!db->md_entries) {
			*out = 0;
			return MDB_SUCCESS;
		}

		frc = mdb_cursor_get(mc, &edge_key, &edge_data, MDB_LAST);
		if (frc == MDB_SUCCESS) {
			int cmp_last = kcmp(&edge_key, (MDB_val *)key);
			if (cmp_last < 0 || (cmp_last == 0 && key_inclusive)) {
				*out = db->md_entries;
				return MDB_SUCCESS;
			}
		} else if (frc != MDB_NOTFOUND) {
			return frc;
		}

		frc = mdb_cursor_get(mc, &edge_key, &edge_data, MDB_FIRST);
		if (frc == MDB_SUCCESS) {
			int cmp_first = kcmp(&edge_key, (MDB_val *)key);
			if (cmp_first > 0 || (cmp_first == 0 && !key_inclusive)) {
				*out = 0;
				return MDB_SUCCESS;
			}
			/* Otherwise treat as high-end search. */
			*out = db->md_entries;
			return MDB_SUCCESS;
		} else if (frc == MDB_NOTFOUND) {
			*out = 0;
			return MDB_SUCCESS;
		} else {
			return frc;
		}
	}
	if (rc != MDB_SUCCESS)
		return rc;

	for (int level = 0; level < mc->mc_top; ++level) {
		MDB_page *branch = mc->mc_pg[level];
		indx_t limit = mc->mc_ki[level];
		if (IS_BRANCH(branch) && IS_COUNTED(branch)) {
			for (indx_t i = 0; i < limit; ++i) {
				MDB_node *node = NODEPTR(branch, i);
				total += mdb_node_get_count(branch, node);
			}
		} else if (IS_BRANCH(branch)) {
			for (indx_t i = 0; i < limit; ++i) {
				MDB_node *node = NODEPTR(branch, i);
				total += mdb_branch_child_count(mc, branch, node);
			}
		}
	}

	MDB_page *leaf = mc->mc_pg[mc->mc_top];
	indx_t idx = mc->mc_ki[mc->mc_top];
	total += mdb_leaf_prefix_contribution(mc, leaf, idx);

	if (key_inclusive && exact && idx < NUMKEYS(leaf)) {
		if (IS_LEAF2(leaf))
			return MDB_CORRUPTED;
		MDB_node *node = NODEPTR(leaf, idx);
		if (node->mn_flags & F_DUPDATA) {
			if (!mc->mc_xcursor)
				return MDB_INCOMPATIBLE;
			if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))
				mdb_xcursor_init1(mc, node);
			uint64_t dup_total = 0;
			if (!value) {
				dup_total = mc->mc_xcursor->mx_db.md_entries;
			} else {
				MDB_cmp_func *dup_cmp =
				    mc->mc_xcursor->mx_cursor.mc_dbx &&
				    mc->mc_xcursor->mx_cursor.mc_dbx->md_cmp ?
				    mc->mc_xcursor->mx_cursor.mc_dbx->md_cmp : dcmp;
				MDB_cursor *dupcur = &mc->mc_xcursor->mx_cursor;
				MDB_cursor dup_work = (MDB_cursor){0};

				mdb_cursor_copy(dupcur, &dup_work);
				dup_work.mc_dbflag = dupcur->mc_dbflag;
				rc = mdb_dup_prefix_count_internal(
				    dupcur, &dup_work,
				    dup_cmp, value, value_inclusive, &dup_total);
				mdb_cursor_leaf_cache_clear(&dup_work.mc_leaf_cache);
				if (rc != MDB_SUCCESS)
					return rc;
			}
			total += dup_total;
		} else {
			MDB_val datum;
			rc = mdb_node_read(mc, node, &datum);
			if (rc != MDB_SUCCESS)
				return rc;
			int cmp = value ? dcmp(&datum, (MDB_val *)value) : 0;
			if (!value) {
				total += 1;
			} else if (cmp < 0 || (cmp == 0 && value_inclusive)) {
				total += 1;
			}
		}
	}

	*out = total;
	return MDB_SUCCESS;
}

static int
mdb_prefix_pair_leq(MDB_txn *txn, MDB_dbi dbi,
	const MDB_val *key, int key_inclusive,
	const MDB_val *value, int value_inclusive,
	uint64_t *out)
{
	MDB_db *db = &txn->mt_dbs[dbi];
	MDB_cursor mc = {0};
	MDB_xcursor mx = {0};
	int rc;

	if (!out)
		return EINVAL;
	if (!key) {
		*out = key_inclusive ? db->md_entries : 0;
		return MDB_SUCCESS;
	}

	mdb_cursor_init(&mc, txn, dbi, &mx);
	rc = mdb_prefix_pair_leq_cursor(&mc, key, key_inclusive,
	    value, value_inclusive, out);
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	MDB_CURSOR_UNREF(&mc, 1);
	return rc;
}

/** Loosen or free a single page.
 * Saves single pages to a list for future reuse
 * in this same txn. It has been pulled from the freeDB
 * and already resides on the dirty list, but has been
 * deleted. Use these pages first before pulling again
 * from the freeDB.
 *
 * If the page wasn't dirtied in this txn, just add it
 * to this txn's free list.
 */
static int
mdb_page_loose(MDB_cursor *mc, MDB_page *mp)
{
	int loose = 0;
	pgno_t pgno = mp->mp_pgno;
	MDB_txn *txn = mc->mc_txn;

	if ((mp->mp_flags & P_DIRTY) && mc->mc_dbi != FREE_DBI) {
		if (txn->mt_parent) {
			MDB_ID2 *dl = txn->mt_u.dirty_list;
			/* If txn has a parent, make sure the page is in our
			 * dirty list.
			 */
			if (dl[0].mid) {
				unsigned x = mdb_mid2l_search(dl, pgno);
			if (x <= dl[0].mid && dl[x].mid == pgno) {
				if (mp != dl[x].mptr) { /* bad cursor? */
					mc->mc_flags &= ~(C_INITIALIZED|C_EOF);
					mdb_txn_mark_error(txn, MDB_PROBLEM);
					return MDB_PROBLEM;
				}
				/* ok, it's ours */
				loose = 1;
			}
			}
		} else {
			/* no parent txn, so it's just ours */
			loose = 1;
		}
	}
	if (loose) {
		DPRINTF(("loosen db %d page %"Yu, DDBI(mc), mp->mp_pgno));
		NEXT_LOOSE_PAGE(mp) = txn->mt_loose_pgs;
		txn->mt_loose_pgs = mp;
		txn->mt_loose_count++;
		mp->mp_flags |= P_LOOSE;
	} else {
		int rc = mdb_midl_append(&txn->mt_free_pgs, pgno);
		if (rc)
			return rc;
	}

	return MDB_SUCCESS;
}

/** Set or clear P_KEEP in dirty, non-overflow, non-sub pages watched by txn.
 * @param[in] mc A cursor handle for the current operation.
 * @param[in] pflags Flags of the pages to update:
 * P_DIRTY to set P_KEEP, P_DIRTY|P_KEEP to clear it.
 * @param[in] all No shortcuts. Needed except after a full #mdb_page_flush().
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_pages_xkeep(MDB_cursor *mc, unsigned pflags, int all)
{
	enum { Mask = P_SUBP|P_DIRTY|P_LOOSE|P_KEEP };
	MDB_txn *txn = mc->mc_txn;
	MDB_cursor *m3, *m0 = mc;
	MDB_xcursor *mx;
	MDB_page *dp, *mp;
	MDB_node *leaf;
	unsigned i, j;
	int rc = MDB_SUCCESS, level;

	/* Mark pages seen by cursors: First m0, then tracked cursors */
	for (i = txn->mt_numdbs;; ) {
		if (mc->mc_flags & C_INITIALIZED) {
			for (m3 = mc;; m3 = &mx->mx_cursor) {
				mp = NULL;
				for (j=0; j<m3->mc_snum; j++) {
					mp = m3->mc_pg[j];
					if ((mp->mp_flags & Mask) == pflags)
						mp->mp_flags ^= P_KEEP;
				}
				mx = m3->mc_xcursor;
				/* Proceed to mx if it is at a sub-database */
				if (! (mx && (mx->mx_cursor.mc_flags & C_INITIALIZED)))
					break;
				if (! (mp && (mp->mp_flags & P_LEAF)))
					break;
				leaf = NODEPTR(mp, m3->mc_ki[j-1]);
				if (!(leaf->mn_flags & F_SUBDATA))
					break;
			}
		}
		mc = mc->mc_next;
		for (; !mc || mc == m0; mc = txn->mt_cursors[--i])
			if (i == 0)
				goto mark_done;
	}

mark_done:
	if (all) {
		/* Mark dirty root pages */
		for (i=0; i<txn->mt_numdbs; i++) {
			if (txn->mt_dbflags[i] & DB_DIRTY) {
				pgno_t pgno = txn->mt_dbs[i].md_root;
				if (pgno == P_INVALID)
					continue;
				if ((rc = mdb_page_get(m0, pgno, &dp, &level)) != MDB_SUCCESS)
					break;
				if ((dp->mp_flags & Mask) == pflags && level <= 1)
					dp->mp_flags ^= P_KEEP;
			}
		}
	}

	return rc;
}

static int mdb_page_flush(MDB_txn *txn, int keep);

/**	Spill pages from the dirty list back to disk.
 * This is intended to prevent running into #MDB_TXN_FULL situations,
 * but note that they may still occur in a few cases:
 *	1) our estimate of the txn size could be too small. Currently this
 *	 seems unlikely, except with a large number of #MDB_MULTIPLE items.
 *	2) child txns may run out of space if their parents dirtied a
 *	 lot of pages and never spilled them. TODO: we probably should do
 *	 a preemptive spill during #mdb_txn_begin() of a child txn, if
 *	 the parent's dirty_room is below a given threshold.
 *
 * Otherwise, if not using nested txns, it is expected that apps will
 * not run into #MDB_TXN_FULL any more. The pages are flushed to disk
 * the same way as for a txn commit, e.g. their P_DIRTY flag is cleared.
 * If the txn never references them again, they can be left alone.
 * If the txn only reads them, they can be used without any fuss.
 * If the txn writes them again, they can be dirtied immediately without
 * going thru all of the work of #mdb_page_touch(). Such references are
 * handled by #mdb_page_unspill().
 *
 * Also note, we never spill DB root pages, nor pages of active cursors,
 * because we'll need these back again soon anyway. And in nested txns,
 * we can't spill a page in a child txn if it was already spilled in a
 * parent txn. That would alter the parent txns' data even though
 * the child hasn't committed yet, and we'd have no way to undo it if
 * the child aborted.
 *
 * @param[in] m0 cursor A cursor handle identifying the transaction and
 *	database for which we are checking space.
 * @param[in] key For a put operation, the key being stored.
 * @param[in] data For a put operation, the data being stored.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_spill(MDB_cursor *m0, MDB_val *key, MDB_val *data)
{
	MDB_txn *txn = m0->mc_txn;
	MDB_page *dp;
	MDB_ID2L dl = txn->mt_u.dirty_list;
	unsigned int i, j, need;
	int rc;

	if (m0->mc_flags & C_SUB)
		return MDB_SUCCESS;

	/* Estimate how much space this op will take */
	i = m0->mc_db->md_depth;
	/* Named DBs also dirty the main DB */
	if (m0->mc_dbi >= CORE_DBS)
		i += txn->mt_dbs[MAIN_DBI].md_depth;
	/* For puts, roughly factor in the key+data size */
	if (key)
		i += (LEAFSIZE(key, data) + txn->mt_env->me_psize) / txn->mt_env->me_psize;
	i += i;	/* double it for good measure */
	need = i;

	if (txn->mt_dirty_room > i)
		return MDB_SUCCESS;

	if (!txn->mt_spill_pgs) {
		txn->mt_spill_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX);
		if (!txn->mt_spill_pgs)
			return ENOMEM;
	} else {
		/* purge deleted slots */
		MDB_IDL sl = txn->mt_spill_pgs;
		unsigned int num = sl[0];
		j=0;
		for (i=1; i<=num; i++) {
			if (!(sl[i] & 1))
				sl[++j] = sl[i];
		}
		sl[0] = j;
	}

	/* Preserve pages which may soon be dirtied again */
	if ((rc = mdb_pages_xkeep(m0, P_DIRTY, 1)) != MDB_SUCCESS)
		goto done;

	/* Less aggressive spill - we originally spilled the entire dirty list,
	 * with a few exceptions for cursor pages and DB root pages. But this
	 * turns out to be a lot of wasted effort because in a large txn many
	 * of those pages will need to be used again. So now we spill only 1/8th
	 * of the dirty pages. Testing revealed this to be a good tradeoff,
	 * better than 1/2, 1/4, or 1/10.
	 */
	if (need < MDB_IDL_UM_MAX / 8)
		need = MDB_IDL_UM_MAX / 8;

	/* Save the page IDs of all the pages we're flushing */
	/* flush from the tail forward, this saves a lot of shifting later on. */
	for (i=dl[0].mid; i && need; i--) {
		MDB_ID pn = dl[i].mid << 1;
		dp = dl[i].mptr;
		if (dp->mp_flags & (P_LOOSE|P_KEEP))
			continue;
		/* Can't spill twice, make sure it's not already in a parent's
		 * spill list.
		 */
		if (txn->mt_parent) {
			MDB_txn *tx2;
			for (tx2 = txn->mt_parent; tx2; tx2 = tx2->mt_parent) {
				if (tx2->mt_spill_pgs) {
					j = mdb_midl_search(tx2->mt_spill_pgs, pn);
					if (j <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[j] == pn) {
						dp->mp_flags |= P_KEEP;
						break;
					}
				}
			}
			if (tx2)
				continue;
		}
		if ((rc = mdb_midl_append(&txn->mt_spill_pgs, pn)))
			goto done;
		need--;
	}
	mdb_midl_sort(txn->mt_spill_pgs);

	/* Flush the spilled part of dirty list */
	if ((rc = mdb_page_flush(txn, i)) != MDB_SUCCESS)
		goto done;

	/* Reset any dirty pages we kept that page_flush didn't see */
	rc = mdb_pages_xkeep(m0, P_DIRTY|P_KEEP, i);

done:
	if (rc)
		mdb_txn_mark_error(txn, rc);
	else
		txn->mt_flags |= MDB_TXN_SPILLS;
	return rc;
}

/** Find oldest txnid still referenced. Expects txn->mt_txnid > 0. */
static txnid_t
mdb_find_oldest(MDB_txn *txn)
{
	int i;
	txnid_t mr, oldest = txn->mt_txnid - 1;
	if (txn->mt_env->me_txns) {
		MDB_reader *r = txn->mt_env->me_txns->mti_readers;
		for (i = txn->mt_env->me_txns->mti_numreaders; --i >= 0; ) {
			if (r[i].mr_pid) {
				mr = r[i].mr_txnid;
				if (oldest > mr)
					oldest = mr;
			}
		}
	}
	return oldest;
}

/** Add a page to the txn's dirty list */
static void
mdb_page_dirty(MDB_txn *txn, MDB_page *mp)
{
	MDB_ID2 mid;
	int rc, (*insert)(MDB_ID2L, MDB_ID2 *);
#ifdef _WIN32	/* With Windows we always write dirty pages with WriteFile,
				 * so we always want them ordered */
	insert = mdb_mid2l_insert;
#else			/* but otherwise with writemaps, we just use msync, we
				 * don't need the ordering and just append */
	if (txn->mt_flags & MDB_TXN_WRITEMAP)
		insert = mdb_mid2l_append;
	else
		insert = mdb_mid2l_insert;
#endif
	mid.mid = mp->mp_pgno;
	mid.mptr = mp;
	rc = insert(txn->mt_u.dirty_list, &mid);
	mdb_tassert(txn, rc == 0);
	txn->mt_dirty_room--;
}

/** Allocate page numbers and memory for writing.  Maintain me_pglast,
 * me_pghead and mt_next_pgno.  Set #MDB_TXN_ERROR on failure.
 *
 * If there are free pages available from older transactions, they
 * are re-used first. Otherwise allocate a new page at mt_next_pgno.
 * Do not modify the freedB, just merge freeDB records into me_pghead[]
 * and move me_pglast to say which records were consumed.  Only this
 * function can create me_pghead and move me_pglast/mt_next_pgno.
 * When #MDB_DEVEL & 2, it is not affected by #mdb_freelist_save(): it
 * then uses the transaction's original snapshot of the freeDB.
 * @param[in] mc cursor A cursor handle identifying the transaction and
 *	database for which we are allocating.
 * @param[in] num the number of pages to allocate.
 * @param[out] mp Address of the allocated page(s). Requests for multiple pages
 *  will always be satisfied by a single contiguous chunk of memory.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_alloc(MDB_cursor *mc, int num, MDB_page **mp)
{
#ifdef MDB_PARANOID	/* Seems like we can ignore this now */
	/* Get at most <Max_retries> more freeDB records once me_pghead
	 * has enough pages.  If not enough, use new pages from the map.
	 * If <Paranoid> and mc is updating the freeDB, only get new
	 * records if me_pghead is empty. Then the freelist cannot play
	 * catch-up with itself by growing while trying to save it.
	 */
	enum { Paranoid = 1, Max_retries = 500 };
#else
	enum { Paranoid = 0, Max_retries = INT_MAX /*infinite*/ };
#endif
	int rc, retry = num * 60;
	MDB_txn *txn = mc->mc_txn;
	MDB_env *env = txn->mt_env;
	pgno_t pgno, *mop = env->me_pghead;
	unsigned i, j, mop_len = mop ? mop[0] : 0, n2 = num-1;
	MDB_page *np;
	txnid_t oldest = 0, last;
	MDB_cursor_op op;
	MDB_cursor m2 = (MDB_cursor){0};
	MDB_xcursor m2x = (MDB_xcursor){0};
	int found_old = 0;

	rc = MDB_SUCCESS;

	/* If there are any loose pages, just use them */
	if (num == 1 && txn->mt_loose_pgs) {
		np = txn->mt_loose_pgs;
		txn->mt_loose_pgs = NEXT_LOOSE_PAGE(np);
		txn->mt_loose_count--;
		DPRINTF(("db %d use loose page %"Yu, DDBI(mc), np->mp_pgno));
		*mp = np;
		goto done;
	}

	*mp = NULL;

	/* If our dirty list is already full, we can't do anything */
	if (txn->mt_dirty_room == 0) {
		rc = MDB_TXN_FULL;
		goto fail;
	}

	for (op = MDB_FIRST;; op = MDB_NEXT) {
		MDB_val key, data;
		MDB_node *leaf;
		pgno_t *idl;

		/* Seek a big enough contiguous page range. Prefer
		 * pages at the tail, just truncating the list.
		 */
		if (mop_len > n2) {
			i = mop_len;
			do {
				pgno = mop[i];
				if (mop[i-n2] == pgno+n2)
					goto search_done;
			} while (--i > n2);
			if (--retry < 0)
				break;
		}

		if (op == MDB_FIRST) {	/* 1st iteration */
			/* Prepare to fetch more and coalesce */
			last = env->me_pglast;
			oldest = env->me_pgoldest;
			mdb_cursor_init(&m2, txn, FREE_DBI, &m2x);
#if (MDB_DEVEL) & 2	/* "& 2" so MDB_DEVEL=1 won't hide bugs breaking freeDB */
			/* Use original snapshot. TODO: Should need less care in code
			 * which modifies the database. Maybe we can delete some code?
			 */
			m2.mc_flags |= C_ORIG_RDONLY;
			m2.mc_db = &env->me_metas[(txn->mt_txnid-1) & 1]->mm_dbs[FREE_DBI];
			m2.mc_dbflag = (unsigned char *)""; /* probably unnecessary */
#endif
			if (last) {
				op = MDB_SET_RANGE;
				key.mv_data = &last; /* will look up last+1 */
				key.mv_size = sizeof(last);
			}
			if (Paranoid && mc->mc_dbi == FREE_DBI)
				retry = -1;
		}
		if (Paranoid && retry < 0 && mop_len)
			break;

		last++;
		/* Do not fetch more if the record will be too recent */
		if (oldest <= last) {
			if (!found_old) {
				oldest = mdb_find_oldest(txn);
				env->me_pgoldest = oldest;
				found_old = 1;
			}
			if (oldest <= last)
				break;
		}
		rc = mdb_cursor_get(&m2, &key, NULL, op);
		if (rc) {
			if (rc == MDB_NOTFOUND)
				break;
			goto fail;
		}
		last = *(txnid_t*)key.mv_data;
		if (oldest <= last) {
			if (!found_old) {
				oldest = mdb_find_oldest(txn);
				env->me_pgoldest = oldest;
				found_old = 1;
			}
			if (oldest <= last)
				break;
		}
		np = m2.mc_pg[m2.mc_top];
		leaf = NODEPTR(np, m2.mc_ki[m2.mc_top]);
		if ((rc = mdb_node_read(&m2, leaf, &data)) != MDB_SUCCESS)
			goto fail;

		idl = (MDB_ID *) data.mv_data;
		i = idl[0];
		if (!mop) {
			if (!(env->me_pghead = mop = mdb_midl_alloc(i))) {
				rc = ENOMEM;
				goto fail;
			}
		} else {
			if ((rc = mdb_midl_need(&env->me_pghead, i)) != 0)
				goto fail;
			mop = env->me_pghead;
		}
		env->me_pglast = last;
#if (MDB_DEBUG) > 1
		DPRINTF(("IDL read txn %"Yu" root %"Yu" num %u",
			last, txn->mt_dbs[FREE_DBI].md_root, i));
		for (j = i; j; j--)
			DPRINTF(("IDL %"Yu, idl[j]));
#endif
		/* Merge in descending sorted order */
		mdb_midl_xmerge(mop, idl);
		mop_len = mop[0];
	}

	/* Use new pages from the map when nothing suitable in the freeDB */
	i = 0;
	pgno = txn->mt_next_pgno;
	if (pgno + num >= env->me_maxpg) {
			DPUTS("DB size maxed out");
			rc = MDB_MAP_FULL;
			goto fail;
	}
#if defined(_WIN32) && !defined(MDB_VL32)
	if (!(env->me_flags & MDB_RDONLY)) {
		void *p;
		p = (MDB_page *)(env->me_map + env->me_psize * pgno);
		p = VirtualAlloc(p, env->me_psize * num, MEM_COMMIT,
			(env->me_flags & MDB_WRITEMAP) ? PAGE_READWRITE:
			PAGE_READONLY);
		if (!p) {
			DPUTS("VirtualAlloc failed");
			rc = ErrCode();
			goto fail;
		}
	}
#endif

search_done:
	if (env->me_flags & MDB_WRITEMAP) {
		np = (MDB_page *)(env->me_map + env->me_psize * pgno);
	} else {
		if (!(np = mdb_page_malloc(txn, num))) {
			rc = ENOMEM;
			goto fail;
		}
	}
	if (i) {
		mop[0] = mop_len -= num;
		/* Move any stragglers down */
		for (j = i-num; j < mop_len; )
			mop[++j] = mop[++i];
	} else {
		txn->mt_next_pgno = pgno + num;
	}
	np->mp_pgno = pgno;
	mdb_page_dirty(txn, np);
	*mp = np;
	rc = MDB_SUCCESS;
	goto done;

fail:
	mdb_txn_mark_error(txn, rc);
done:
	mdb_cursor_leaf_cache_clear(&m2.mc_leaf_cache);
	return rc;
}

/** Copy the used portions of a non-overflow page.
 * @param[in] dst page to copy into
 * @param[in] src page to copy from
 * @param[in] psize size of a page
 */
static void
mdb_page_copy(MDB_page *dst, MDB_page *src, unsigned int psize)
{
	enum { Align = sizeof(pgno_t) };
	indx_t upper = src->mp_upper, lower = src->mp_lower, unused = upper-lower;

	/* If page isn't full, just copy the used portion. Adjust
	 * alignment so memcpy may copy words instead of bytes.
	 */
	if ((unused &= -Align) && !IS_LEAF2(src)) {
		upper = (upper + PAGEBASE) & -Align;
		memcpy(dst, src, (lower + PAGEBASE + (Align-1)) & -Align);
		memcpy((pgno_t *)((char *)dst+upper), (pgno_t *)((char *)src+upper),
			psize - upper);
	} else {
		memcpy(dst, src, psize - unused);
	}
}

/** Pull a page off the txn's spill list, if present.
 * If a page being referenced was spilled to disk in this txn, bring
 * it back and make it dirty/writable again.
 * @param[in] txn the transaction handle.
 * @param[in] mp the page being referenced. It must not be dirty.
 * @param[out] ret the writable page, if any. ret is unchanged if
 * mp wasn't spilled.
 */
static int
mdb_page_unspill(MDB_txn *txn, MDB_page *mp, MDB_page **ret)
{
	MDB_env *env = txn->mt_env;
	const MDB_txn *tx2;
	unsigned x;
	pgno_t pgno = mp->mp_pgno, pn = pgno << 1;

	for (tx2 = txn; tx2; tx2=tx2->mt_parent) {
		if (!tx2->mt_spill_pgs)
			continue;
		x = mdb_midl_search(tx2->mt_spill_pgs, pn);
		if (x <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[x] == pn) {
			MDB_page *np;
			int num;
			if (txn->mt_dirty_room == 0)
				return MDB_TXN_FULL;
			if (IS_OVERFLOW(mp))
				num = mp->mp_pages;
			else
				num = 1;
			if (env->me_flags & MDB_WRITEMAP) {
				np = mp;
			} else {
				np = mdb_page_malloc(txn, num);
				if (!np)
					return ENOMEM;
				if (num > 1)
					memcpy(np, mp, num * env->me_psize);
				else
					mdb_page_copy(np, mp, env->me_psize);
			}
			if (tx2 == txn) {
				/* If in current txn, this page is no longer spilled.
				 * If it happens to be the last page, truncate the spill list.
				 * Otherwise mark it as deleted by setting the LSB.
				 */
				if (x == txn->mt_spill_pgs[0])
					txn->mt_spill_pgs[0]--;
				else
					txn->mt_spill_pgs[x] |= 1;
			}	/* otherwise, if belonging to a parent txn, the
				 * page remains spilled until child commits
				 */

			mdb_page_dirty(txn, np);
			np->mp_flags |= P_DIRTY;
			*ret = np;
			break;
		}
	}
	return MDB_SUCCESS;
}

/** Touch a page: make it dirty and re-insert into tree with updated pgno.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc cursor pointing to the page to be touched
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_touch(MDB_cursor *mc)
{
	MDB_page *mp = mc->mc_pg[mc->mc_top], *np;
	MDB_txn *txn = mc->mc_txn;
	MDB_cursor *m2, *m3;
	pgno_t	pgno;
	int rc;

	if (!F_ISSET(MP_FLAGS(mp), P_DIRTY)) {
		if (txn->mt_flags & MDB_TXN_SPILLS) {
			np = NULL;
			rc = mdb_page_unspill(txn, mp, &np);
			if (rc)
				goto fail;
			if (np)
				goto done;
		}
		if ((rc = mdb_midl_need(&txn->mt_free_pgs, 1)) ||
			(rc = mdb_page_alloc(mc, 1, &np)))
			goto fail;
		pgno = np->mp_pgno;
		DPRINTF(("touched db %d page %"Yu" -> %"Yu, DDBI(mc),
			mp->mp_pgno, pgno));
		mdb_cassert(mc, mp->mp_pgno != pgno);
		mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
		/* Update the parent page, if any, to point to the new page */
		if (mc->mc_top) {
			MDB_page *parent = mc->mc_pg[mc->mc_top-1];
			MDB_node *node = NODEPTR(parent, mc->mc_ki[mc->mc_top-1]);
			SETPGNO(node, pgno);
		} else {
			mc->mc_db->md_root = pgno;
		}
	} else if (txn->mt_parent && !IS_SUBP(mp)) {
		MDB_ID2 mid, *dl = txn->mt_u.dirty_list;
		pgno = mp->mp_pgno;
		/* If txn has a parent, make sure the page is in our
		 * dirty list.
		 */
		if (dl[0].mid) {
			unsigned x = mdb_mid2l_search(dl, pgno);
			if (x <= dl[0].mid && dl[x].mid == pgno) {
				if (mp != dl[x].mptr) { /* bad cursor? */
					mc->mc_flags &= ~(C_INITIALIZED|C_EOF);
					mdb_txn_mark_error(txn, MDB_PROBLEM);
					return MDB_PROBLEM;
				}
				return 0;
			}
		}
		mdb_cassert(mc, dl[0].mid < MDB_IDL_UM_MAX);
		/* No - copy it */
		np = mdb_page_malloc(txn, 1);
		if (!np)
			return ENOMEM;
		mid.mid = pgno;
		mid.mptr = np;
		rc = mdb_mid2l_insert(dl, &mid);
		mdb_cassert(mc, rc == 0);
	} else {
		return 0;
	}

	mdb_page_copy(np, mp, txn->mt_env->me_psize);
	np->mp_pgno = pgno;
	np->mp_flags |= P_DIRTY;

done:
	/* Adjust cursors pointing to mp */
	mc->mc_pg[mc->mc_top] = np;
	m2 = txn->mt_cursors[mc->mc_dbi];
	if (mc->mc_flags & C_SUB) {
		for (; m2; m2=m2->mc_next) {
			m3 = &m2->mc_xcursor->mx_cursor;
			if (m3->mc_snum < mc->mc_snum) continue;
			if (m3->mc_pg[mc->mc_top] == mp)
				m3->mc_pg[mc->mc_top] = np;
		}
	} else {
		for (; m2; m2=m2->mc_next) {
			if (m2->mc_snum < mc->mc_snum) continue;
			if (m2 == mc) continue;
			if (m2->mc_pg[mc->mc_top] == mp) {
				m2->mc_pg[mc->mc_top] = np;
				if (IS_LEAF(np))
					XCURSOR_REFRESH(m2, mc->mc_top, np);
			}
		}
	}
	MDB_PAGE_UNREF(mc->mc_txn, mp);
	return 0;

fail:
	mdb_txn_mark_error(txn, rc);
	return rc;
}

int
mdb_env_sync0(MDB_env *env, int force, pgno_t numpgs)
{
	int rc = 0;
	if (env->me_flags & MDB_INMEMORY)
		return MDB_SUCCESS;
	if (env->me_flags & MDB_RDONLY)
		return EACCES;
	if (force
#ifndef _WIN32	/* Sync is normally achieved in Windows by doing WRITE_THROUGH writes */
		|| !(env->me_flags & MDB_NOSYNC)
#endif
		) {
		if (env->me_flags & MDB_WRITEMAP) {
			int flags = ((env->me_flags & MDB_MAPASYNC) && !force)
				? MS_ASYNC : MS_SYNC;
			if (MDB_MSYNC(env->me_map, env->me_psize * numpgs, flags))
				rc = ErrCode();
#if defined(_WIN32) || defined(__APPLE__)
			else if (flags == MS_SYNC && MDB_FDATASYNC(env->me_fd))
				rc = ErrCode();
#endif
		} else {
#ifdef BROKEN_FDATASYNC
			if (env->me_flags & MDB_FSYNCONLY) {
				if (fsync(env->me_fd))
					rc = ErrCode();
			} else
#endif
			if (MDB_FDATASYNC(env->me_fd))
				rc = ErrCode();
		}
	}
	return rc;
}

int
mdb_env_sync(MDB_env *env, int force)
{
	MDB_meta *m = mdb_env_pick_meta(env);
	return mdb_env_sync0(env, force, m->mm_last_pg+1);
}

/** Back up parent txn's cursors, then grab the originals for tracking */
static int
mdb_cursor_shadow(MDB_txn *src, MDB_txn *dst)
{
	MDB_cursor *mc, *bk;
	MDB_xcursor *mx;
	size_t size;
	int i;

	for (i = src->mt_numdbs; --i >= 0; ) {
		if ((mc = src->mt_cursors[i]) != NULL) {
			size = sizeof(MDB_cursor);
			if (mc->mc_xcursor)
				size += sizeof(MDB_xcursor);
			for (; mc; mc = bk->mc_next) {
				bk = malloc(size);
				if (!bk)
					return ENOMEM;
				*bk = *mc;
				mc->mc_backup = bk;
				mc->mc_db = &dst->mt_dbs[i];
				/* Kill pointers into src to reduce abuse: The
				 * user may not use mc until dst ends. But we need a valid
				 * txn pointer here for cursor fixups to keep working.
				 */
				mc->mc_txn    = dst;
				mc->mc_dbflag = &dst->mt_dbflags[i];
				if ((mx = mc->mc_xcursor) != NULL) {
					*(MDB_xcursor *)(bk+1) = *mx;
					mx->mx_cursor.mc_txn = dst;
				}
				mc->mc_next = dst->mt_cursors[i];
				dst->mt_cursors[i] = mc;
			}
		}
	}
	return MDB_SUCCESS;
}

/** Close this write txn's cursors, give parent txn's cursors back to parent.
 * @param[in] txn the transaction handle.
 * @param[in] merge true to keep changes to parent cursors, false to revert.
 * @return 0 on success, non-zero on failure.
 */
static void
mdb_cursors_close(MDB_txn *txn, unsigned merge)
{
	MDB_cursor **cursors = txn->mt_cursors, *mc, *next, *bk;
	MDB_xcursor *mx;
	int i;

	for (i = txn->mt_numdbs; --i >= 0; ) {
		for (mc = cursors[i]; mc; mc = next) {
			next = mc->mc_next;
			if ((bk = mc->mc_backup) != NULL) {
				if (merge) {
					/* Commit changes to parent txn */
					mc->mc_next = bk->mc_next;
					mc->mc_backup = bk->mc_backup;
					mc->mc_txn = bk->mc_txn;
					mc->mc_db = bk->mc_db;
					mc->mc_dbflag = bk->mc_dbflag;
					if ((mx = mc->mc_xcursor) != NULL)
						mx->mx_cursor.mc_txn = bk->mc_txn;
				} else {
					/* Abort nested txn */
					*mc = *bk;
					if ((mx = mc->mc_xcursor) != NULL)
						*mx = *(MDB_xcursor *)(bk+1);
				}
				mc = bk;
			}
			/* Only malloced cursors are permanently tracked. */
			free(mc);
		}
		cursors[i] = NULL;
	}
}

#if !(MDB_PIDLOCK)		/* Currently the same as defined(_WIN32) */
enum Pidlock_op {
	Pidset, Pidcheck
};
#else
enum Pidlock_op {
	Pidset = F_SETLK, Pidcheck = F_GETLK
};
#endif

/** Set or check a pid lock. Set returns 0 on success.
 * Check returns 0 if the process is certainly dead, nonzero if it may
 * be alive (the lock exists or an error happened so we do not know).
 *
 * On Windows Pidset is a no-op, we merely check for the existence
 * of the process with the given pid. On POSIX we use a single byte
 * lock on the lockfile, set at an offset equal to the pid.
 */
static int
mdb_reader_pid(MDB_env *env, enum Pidlock_op op, MDB_PID_T pid)
{
#if !(MDB_PIDLOCK)		/* Currently the same as defined(_WIN32) */
	int ret = 0;
	HANDLE h;
	if (op == Pidcheck) {
		h = OpenProcess(env->me_pidquery, FALSE, pid);
		/* No documented "no such process" code, but other program use this: */
		if (!h)
			return ErrCode() != ERROR_INVALID_PARAMETER;
		/* A process exists until all handles to it close. Has it exited? */
		ret = WaitForSingleObject(h, 0) != 0;
		CloseHandle(h);
	}
	return ret;
#else
	for (;;) {
		int rc;
		struct flock lock_info;
		memset(&lock_info, 0, sizeof(lock_info));
		lock_info.l_type = F_WRLCK;
		lock_info.l_whence = SEEK_SET;
		lock_info.l_start = pid;
		lock_info.l_len = 1;
		if ((rc = fcntl(env->me_lfd, op, &lock_info)) == 0) {
			if (op == F_GETLK && lock_info.l_type != F_UNLCK)
				rc = -1;
		} else if ((rc = ErrCode()) == EINTR) {
			continue;
		}
		return rc;
	}
#endif
}

/** Common code for #mdb_txn_begin() and #mdb_txn_renew().
 * @param[in] txn the transaction handle to initialize
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_txn_renew0(MDB_txn *txn)
{
	MDB_env *env = txn->mt_env;
	MDB_txninfo *ti = env->me_txns;
	MDB_meta *meta;
	unsigned int i, nr, flags = txn->mt_flags;
	uint16_t x;
	int rc, new_notls = 0;

	if ((flags &= MDB_TXN_RDONLY) != 0) {
		if (!ti) {
			meta = mdb_env_pick_meta(env);
			txn->mt_txnid = meta->mm_txnid;
			txn->mt_u.reader = NULL;
		} else {
			MDB_reader *r = (env->me_flags & MDB_NOTLS) ? txn->mt_u.reader :
				pthread_getspecific(env->me_txkey);
			if (r) {
				if (r->mr_pid != env->me_pid || r->mr_txnid != (txnid_t)-1)
					return MDB_BAD_RSLOT;
			} else {
				MDB_PID_T pid = env->me_pid;
				MDB_THR_T tid = pthread_self();
				mdb_mutexref_t rmutex = env->me_rmutex;

				if (!env->me_live_reader) {
					rc = mdb_reader_pid(env, Pidset, pid);
					if (rc)
						return rc;
					env->me_live_reader = 1;
				}

				if (LOCK_MUTEX(rc, env, rmutex))
					return rc;
				nr = ti->mti_numreaders;
				for (i=0; i<nr; i++)
					if (ti->mti_readers[i].mr_pid == 0)
						break;
				if (i == env->me_maxreaders) {
					UNLOCK_MUTEX(rmutex);
					return MDB_READERS_FULL;
				}
				r = &ti->mti_readers[i];
				/* Claim the reader slot, carefully since other code
				 * uses the reader table un-mutexed: First reset the
				 * slot, next publish it in mti_numreaders.  After
				 * that, it is safe for mdb_env_close() to touch it.
				 * When it will be closed, we can finally claim it.
				 */
				r->mr_pid = 0;
				r->mr_txnid = (txnid_t)-1;
				r->mr_tid = tid;
				if (i == nr)
					ti->mti_numreaders = ++nr;
				env->me_close_readers = nr;
				r->mr_pid = pid;
				UNLOCK_MUTEX(rmutex);

				new_notls = (env->me_flags & MDB_NOTLS);
				if (!new_notls && (rc=pthread_setspecific(env->me_txkey, r))) {
					r->mr_pid = 0;
					return rc;
				}
			}
			do /* LY: Retry on a race, ITS#7970. */
				r->mr_txnid = ti->mti_txnid;
			while(r->mr_txnid != ti->mti_txnid);
			if (!r->mr_txnid && (env->me_flags & MDB_RDONLY)) {
				meta = mdb_env_pick_meta(env);
				r->mr_txnid = meta->mm_txnid;
			} else {
				meta = env->me_metas[r->mr_txnid & 1];
			}
			txn->mt_txnid = r->mr_txnid;
			txn->mt_u.reader = r;
		}

	} else {
		/* Not yet touching txn == env->me_txn0, it may be active */
		if (ti) {
			if (LOCK_MUTEX(rc, env, env->me_wmutex))
				return rc;
			txn->mt_txnid = ti->mti_txnid;
			meta = env->me_metas[txn->mt_txnid & 1];
		} else {
			meta = mdb_env_pick_meta(env);
			txn->mt_txnid = meta->mm_txnid;
		}
		txn->mt_txnid++;
#if MDB_DEBUG
		if (txn->mt_txnid == mdb_debug_start)
			mdb_debug = MDB_DBG_INFO;
#endif
		txn->mt_child = NULL;
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
		txn->mt_dirty_room = MDB_IDL_UM_MAX;
		txn->mt_u.dirty_list = env->me_dirty_list;
		txn->mt_u.dirty_list[0].mid = 0;
		txn->mt_free_pgs = env->me_free_pgs;
		txn->mt_free_pgs[0] = 0;
		txn->mt_spill_pgs = NULL;
		env->me_txn = txn;
		memcpy(txn->mt_dbiseqs, env->me_dbiseqs, env->me_maxdbs * sizeof(unsigned int));
	}

	/* Copy the DB info and flags */
	memcpy(txn->mt_dbs, meta->mm_dbs, CORE_DBS * sizeof(MDB_db));

	/* Moved to here to avoid a data race in read TXNs */
	txn->mt_next_pgno = meta->mm_last_pg+1;
#ifdef MDB_VL32
	txn->mt_last_pgno = txn->mt_next_pgno - 1;
#endif

	txn->mt_flags = flags;
	txn->mt_last_err = MDB_SUCCESS;

	/* Setup db info */
	txn->mt_numdbs = env->me_numdbs;
	for (i=CORE_DBS; i<txn->mt_numdbs; i++) {
		x = env->me_dbflags[i];
		txn->mt_dbs[i].md_flags = x & PERSISTENT_FLAGS;
		txn->mt_dbflags[i] = (x & MDB_VALID) ? DB_VALID|DB_USRVALID|DB_STALE : 0;
	}
	txn->mt_dbflags[MAIN_DBI] = DB_VALID|DB_USRVALID;
	txn->mt_dbflags[FREE_DBI] = DB_VALID;

	if (env->me_flags & MDB_FATAL_ERROR) {
		DPUTS("environment had fatal error, must shutdown!");
		rc = MDB_PANIC;
	} else if (env->me_maxpg < txn->mt_next_pgno) {
		rc = MDB_MAP_RESIZED;
	} else {
		return MDB_SUCCESS;
	}
	mdb_txn_end(txn, new_notls /*0 or MDB_END_SLOT*/ | MDB_END_FAIL_BEGIN);
	return rc;
}

int
mdb_txn_renew(MDB_txn *txn)
{
	int rc;

	if (!txn || !F_ISSET(txn->mt_flags, MDB_TXN_RDONLY|MDB_TXN_FINISHED))
		return EINVAL;

	rc = mdb_txn_renew0(txn);
	if (rc == MDB_SUCCESS) {
		DPRINTF(("renew txn %"Yu"%c %p on mdbenv %p, root page %"Yu,
			txn->mt_txnid, (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
			(void *)txn, (void *)txn->mt_env, txn->mt_dbs[MAIN_DBI].md_root));
	}
	return rc;
}

int
mdb_txn_begin(MDB_env *env, MDB_txn *parent, unsigned int flags, MDB_txn **ret)
{
	MDB_txn *txn;
	MDB_ntxn *ntxn;
	int rc, size, tsize;

	flags &= MDB_TXN_BEGIN_FLAGS;
	flags |= env->me_flags & MDB_WRITEMAP;

	if (env->me_flags & MDB_RDONLY & ~flags) /* write txn in RDONLY env */
		return EACCES;

	if (parent) {
		/* Nested transactions: Max 1 child, write txns only, no writemap */
		flags |= parent->mt_flags;
		if (flags & (MDB_RDONLY|MDB_WRITEMAP|MDB_TXN_BLOCKED)) {
			return (parent->mt_flags & MDB_TXN_RDONLY) ? EINVAL : MDB_BAD_TXN;
		}
		/* Child txns save MDB_pgstate and use own copy of cursors */
		size = env->me_maxdbs * (sizeof(MDB_db)+sizeof(MDB_cursor *)+1);
		size += tsize = sizeof(MDB_ntxn);
	} else if (flags & MDB_RDONLY) {
		size = env->me_maxdbs * (sizeof(MDB_db)+1);
		size += tsize = sizeof(MDB_txn);
	} else {
		/* Reuse preallocated write txn. However, do not touch it until
		 * mdb_txn_renew0() succeeds, since it currently may be active.
		 */
		txn = env->me_txn0;
		goto renew;
	}
	if ((txn = calloc(1, size)) == NULL) {
		DPRINTF(("calloc: %s", strerror(errno)));
		return ENOMEM;
	}
#ifdef MDB_VL32
	if (!parent) {
		txn->mt_rpages = malloc(MDB_TRPAGE_SIZE * sizeof(MDB_ID3));
		if (!txn->mt_rpages) {
			free(txn);
			return ENOMEM;
		}
		txn->mt_rpages[0].mid = 0;
		txn->mt_rpcheck = MDB_TRPAGE_SIZE/2;
	}
#endif
	txn->mt_dbxs = env->me_dbxs;	/* static */
	txn->mt_dbs = (MDB_db *) ((char *)txn + tsize);
	txn->mt_dbflags = (unsigned char *)txn + size - env->me_maxdbs;
	txn->mt_flags = flags;
	txn->mt_env = env;
	txn->mt_last_err = MDB_SUCCESS;

	if (parent) {
		unsigned int i;
		txn->mt_cursors = (MDB_cursor **)(txn->mt_dbs + env->me_maxdbs);
		txn->mt_dbiseqs = parent->mt_dbiseqs;
		txn->mt_u.dirty_list = malloc(sizeof(MDB_ID2)*MDB_IDL_UM_SIZE);
		if (!txn->mt_u.dirty_list ||
			!(txn->mt_free_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX)))
		{
			free(txn->mt_u.dirty_list);
			free(txn);
			return ENOMEM;
		}
		txn->mt_txnid = parent->mt_txnid;
		txn->mt_dirty_room = parent->mt_dirty_room;
		txn->mt_u.dirty_list[0].mid = 0;
		txn->mt_spill_pgs = NULL;
		txn->mt_next_pgno = parent->mt_next_pgno;
		parent->mt_flags |= MDB_TXN_HAS_CHILD;
		parent->mt_child = txn;
		txn->mt_parent = parent;
		txn->mt_numdbs = parent->mt_numdbs;
#ifdef MDB_VL32
		txn->mt_rpages = parent->mt_rpages;
#endif
		memcpy(txn->mt_dbs, parent->mt_dbs, txn->mt_numdbs * sizeof(MDB_db));
		/* Copy parent's mt_dbflags, but clear DB_NEW */
		for (i=0; i<txn->mt_numdbs; i++)
			txn->mt_dbflags[i] = parent->mt_dbflags[i] & ~DB_NEW;
		rc = 0;
		ntxn = (MDB_ntxn *)txn;
		ntxn->mnt_pgstate = env->me_pgstate; /* save parent me_pghead & co */
		if (env->me_pghead) {
			size = MDB_IDL_SIZEOF(env->me_pghead);
			env->me_pghead = mdb_midl_alloc(env->me_pghead[0]);
			if (env->me_pghead)
				memcpy(env->me_pghead, ntxn->mnt_pgstate.mf_pghead, size);
			else
				rc = ENOMEM;
		}
		if (!rc)
			rc = mdb_cursor_shadow(parent, txn);
		if (rc)
			mdb_txn_end(txn, MDB_END_FAIL_BEGINCHILD);
	} else { /* MDB_RDONLY */
		txn->mt_dbiseqs = env->me_dbiseqs;
renew:
		rc = mdb_txn_renew0(txn);
	}
	if (rc) {
		if (txn != env->me_txn0) {
			/* mt_rpages is owned by parent */
			free(txn->mt_u.dirty_list);
			free(txn);
		}
	} else {
		txn->mt_flags |= flags;	/* could not change txn=me_txn0 earlier */
		*ret = txn;
		DPRINTF(("begin txn %"Yu"%c %p on mdbenv %p, root page %"Yu,
			txn->mt_txnid, (flags & MDB_RDONLY) ? 'r' : 'w',
			(void *) txn, (void *) env, txn->mt_dbs[MAIN_DBI].md_root));
	}
	MDB_TRACE(("%p, %p, %u = %p", env, parent, flags, txn));

	return rc;
}

MDB_env *
mdb_txn_env(MDB_txn *txn)
{
	if (!txn) return NULL;
	return txn->mt_env;
}

mdb_size_t
mdb_txn_id(MDB_txn *txn)
{
	if (!txn) return 0;
	return txn->mt_txnid;
}

/** Export or close DBI handles opened in this txn. */
static void
mdb_dbis_update(MDB_txn *txn, int keep)
{
	int i;
	MDB_dbi n = txn->mt_numdbs;
	MDB_env *env = txn->mt_env;
	unsigned char *tdbflags = txn->mt_dbflags;

	for (i = n; --i >= CORE_DBS;) {
		if (tdbflags[i] & DB_NEW) {
			if (keep) {
				env->me_dbflags[i] = txn->mt_dbs[i].md_flags | MDB_VALID;
			} else {
				char *ptr = env->me_dbxs[i].md_name.mv_data;
				if (ptr) {
					env->me_dbxs[i].md_name.mv_data = NULL;
					env->me_dbxs[i].md_name.mv_size = 0;
					env->me_dbflags[i] = 0;
					env->me_dbiseqs[i]++;
					free(ptr);
				}
			}
		}
	}
	if (keep && env->me_numdbs < n)
		env->me_numdbs = n;
}

/** End a transaction, except successful commit of a nested transaction.
 * May be called twice for readonly txns: First reset it, then abort.
 * @param[in] txn the transaction handle to end
 * @param[in] mode why and how to end the transaction
 */
static void
mdb_txn_end(MDB_txn *txn, unsigned mode)
{
	MDB_env	*env = txn->mt_env;
#if MDB_DEBUG
	static const char *const names[] = MDB_END_NAMES;
#endif

	/* Export or close DBI handles opened in this txn */
	mdb_dbis_update(txn, mode & MDB_END_UPDATE);

	DPRINTF(("%s txn %"Yu"%c %p on mdbenv %p, root page %"Yu,
		names[mode & MDB_END_OPMASK],
		txn->mt_txnid, (txn->mt_flags & MDB_TXN_RDONLY) ? 'r' : 'w',
		(void *) txn, (void *)env, txn->mt_dbs[MAIN_DBI].md_root));

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		if (txn->mt_u.reader) {
			txn->mt_u.reader->mr_txnid = (txnid_t)-1;
			if (!(env->me_flags & MDB_NOTLS)) {
				txn->mt_u.reader = NULL; /* txn does not own reader */
			} else if (mode & MDB_END_SLOT) {
				txn->mt_u.reader->mr_pid = 0;
				txn->mt_u.reader = NULL;
			} /* else txn owns the slot until it does MDB_END_SLOT */
		}
		txn->mt_numdbs = 0;		/* prevent further DBI activity */
		txn->mt_flags |= MDB_TXN_FINISHED;
		if (mode & MDB_END_RESET)
			mdb_prefix_scratch_reset(&txn->mt_prefix);

	} else if (!F_ISSET(txn->mt_flags, MDB_TXN_FINISHED)) {
		pgno_t *pghead = env->me_pghead;

		if (!(mode & MDB_END_UPDATE)) /* !(already closed cursors) */
			mdb_cursors_close(txn, 0);
		if (!(env->me_flags & MDB_WRITEMAP)) {
			mdb_dlist_free(txn);
		}

		txn->mt_numdbs = 0;
		txn->mt_flags = MDB_TXN_FINISHED;
		mdb_midl_free(txn->mt_spill_pgs);

		if (!txn->mt_parent) {
			mdb_midl_shrink(&txn->mt_free_pgs);
			env->me_free_pgs = txn->mt_free_pgs;
			/* me_pgstate: */
			env->me_pghead = NULL;
			env->me_pglast = 0;

			env->me_txn = NULL;
			/* env->me_txn0 is reused across write txns; invalidate
			 * txn-local prefix cache metadata while keeping scratch
			 * allocations to amortize future insert/update cost.
			 */
			mdb_prefix_scratch_reset(&txn->mt_prefix);
			mode = 0;	/* txn == env->me_txn0, do not free() it */

			/* The writer mutex was locked in mdb_txn_begin. */
			if (env->me_txns)
				UNLOCK_MUTEX(env->me_wmutex);
		} else {
			txn->mt_parent->mt_child = NULL;
			txn->mt_parent->mt_flags &= ~MDB_TXN_HAS_CHILD;
			env->me_pgstate = ((MDB_ntxn *)txn)->mnt_pgstate;
			mdb_midl_free(txn->mt_free_pgs);
			free(txn->mt_u.dirty_list);
		}

		mdb_midl_free(pghead);
	}
#ifdef MDB_VL32
	if (!txn->mt_parent) {
		MDB_ID3L el = env->me_rpages, tl = txn->mt_rpages;
		unsigned i, x, n = tl[0].mid;
		pthread_mutex_lock(&env->me_rpmutex);
		for (i = 1; i <= n; i++) {
			if (tl[i].mid & (MDB_RPAGE_CHUNK-1)) {
				/* tmp overflow pages that we didn't share in env */
				munmap(tl[i].mptr, tl[i].mcnt * env->me_psize);
			} else {
				x = mdb_mid3l_search(el, tl[i].mid);
				if (tl[i].mptr == el[x].mptr) {
					el[x].mref--;
				} else {
					/* another tmp overflow page */
					munmap(tl[i].mptr, tl[i].mcnt * env->me_psize);
				}
			}
		}
		pthread_mutex_unlock(&env->me_rpmutex);
		tl[0].mid = 0;
		if (mode & MDB_END_FREE)
			free(tl);
	}
#endif
	if (mode & MDB_END_FREE) {
		mdb_prefix_scratch_clear(&txn->mt_prefix);
		free(txn);
	}
}

void
mdb_txn_reset(MDB_txn *txn)
{
	if (txn == NULL)
		return;

	/* This call is only valid for read-only txns */
	if (!(txn->mt_flags & MDB_TXN_RDONLY))
		return;

	mdb_txn_end(txn, MDB_END_RESET);
}

static void
_mdb_txn_abort(MDB_txn *txn)
{
	if (txn == NULL)
		return;

	if (txn->mt_child)
		_mdb_txn_abort(txn->mt_child);

	mdb_txn_end(txn, MDB_END_ABORT|MDB_END_SLOT|MDB_END_FREE);
}

void
mdb_txn_abort(MDB_txn *txn)
{
	MDB_TRACE(("%p", txn));
	_mdb_txn_abort(txn);
}

/** Save the freelist as of this transaction to the freeDB.
 * This changes the freelist. Keep trying until it stabilizes.
 *
 * When (MDB_DEVEL) & 2, the changes do not affect #mdb_page_alloc(),
 * it then uses the transaction's original snapshot of the freeDB.
 */
static int
mdb_freelist_save(MDB_txn *txn)
{
	/* env->me_pghead[] can grow and shrink during this call.
	 * env->me_pglast and txn->mt_free_pgs[] can only grow.
	 * Page numbers cannot disappear from txn->mt_free_pgs[].
	 */
	MDB_cursor mc = {0};
	MDB_xcursor mx = {0};
	MDB_env	*env = txn->mt_env;
	int rc, maxfree_1pg = env->me_maxfree_1pg, more = 1;
	txnid_t	pglast = 0, head_id = 0;
	pgno_t	freecnt = 0, *free_pgs, *mop;
	ssize_t	head_room = 0, total_room = 0, mop_len, clean_limit;

	mdb_cursor_init(&mc, txn, FREE_DBI, &mx);

	if (env->me_pghead) {
		/* Make sure first page of freeDB is touched and on freelist */
		rc = mdb_page_search(&mc, NULL, MDB_PS_FIRST|MDB_PS_MODIFY);
		if (rc && rc != MDB_NOTFOUND)
			return rc;
	}

	if (!env->me_pghead && txn->mt_loose_pgs) {
		/* Put loose page numbers in mt_free_pgs, since
		 * we may be unable to return them to me_pghead.
		 */
		MDB_page *mp = txn->mt_loose_pgs;
		MDB_ID2 *dl = txn->mt_u.dirty_list;
		unsigned x;
		if ((rc = mdb_midl_need(&txn->mt_free_pgs, txn->mt_loose_count)) != 0)
			return rc;
		for (; mp; mp = NEXT_LOOSE_PAGE(mp)) {
			mdb_midl_xappend(txn->mt_free_pgs, mp->mp_pgno);
			/* must also remove from dirty list */
			if (txn->mt_flags & MDB_TXN_WRITEMAP) {
				for (x=1; x<=dl[0].mid; x++)
					if (dl[x].mid == mp->mp_pgno)
						break;
				mdb_tassert(txn, x <= dl[0].mid);
			} else {
				x = mdb_mid2l_search(dl, mp->mp_pgno);
				mdb_tassert(txn, dl[x].mid == mp->mp_pgno);
				mdb_dpage_free(env, mp);
			}
			dl[x].mptr = NULL;
		}
		{
			/* squash freed slots out of the dirty list */
			unsigned y;
			for (y=1; dl[y].mptr && y <= dl[0].mid; y++);
			if (y <= dl[0].mid) {
				for(x=y, y++;;) {
					while (!dl[y].mptr && y <= dl[0].mid) y++;
					if (y > dl[0].mid) break;
					dl[x++] = dl[y++];
				}
				dl[0].mid = x-1;
			} else {
				/* all slots freed */
				dl[0].mid = 0;
			}
		}
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
	}

	/* MDB_RESERVE cancels meminit in ovpage malloc (when no WRITEMAP) */
	clean_limit = (env->me_flags & (MDB_NOMEMINIT|MDB_WRITEMAP))
		? SSIZE_MAX : maxfree_1pg;

	for (;;) {
		/* Come back here after each Put() in case freelist changed */
		MDB_val key, data;
		pgno_t *pgs;
		ssize_t j;

		/* If using records from freeDB which we have not yet
		 * deleted, delete them and any we reserved for me_pghead.
		 */
		while (pglast < env->me_pglast) {
			rc = mdb_cursor_first(&mc, &key, NULL);
			if (rc)
				return rc;
			pglast = head_id = *(txnid_t *)key.mv_data;
			total_room = head_room = 0;
			mdb_tassert(txn, pglast <= env->me_pglast);
			rc = _mdb_cursor_del(&mc, 0);
			if (rc)
				return rc;
		}

		/* Save the IDL of pages freed by this txn, to a single record */
		if (freecnt < txn->mt_free_pgs[0]) {
			if (!freecnt) {
				/* Make sure last page of freeDB is touched and on freelist */
				rc = mdb_page_search(&mc, NULL, MDB_PS_LAST|MDB_PS_MODIFY);
				if (rc && rc != MDB_NOTFOUND)
					return rc;
			}
			free_pgs = txn->mt_free_pgs;
			/* Write to last page of freeDB */
			key.mv_size = sizeof(txn->mt_txnid);
			key.mv_data = &txn->mt_txnid;
			do {
				freecnt = free_pgs[0];
				data.mv_size = MDB_IDL_SIZEOF(free_pgs);
				rc = _mdb_cursor_put(&mc, &key, &data, MDB_RESERVE);
				if (rc)
					return rc;
				/* Retry if mt_free_pgs[] grew during the Put() */
				free_pgs = txn->mt_free_pgs;
			} while (freecnt < free_pgs[0]);
			mdb_midl_sort(free_pgs);
			memcpy(data.mv_data, free_pgs, data.mv_size);
#if (MDB_DEBUG) > 1
			{
				unsigned int i = free_pgs[0];
				DPRINTF(("IDL write txn %"Yu" root %"Yu" num %u",
					txn->mt_txnid, txn->mt_dbs[FREE_DBI].md_root, i));
				for (; i; i--)
					DPRINTF(("IDL %"Yu, free_pgs[i]));
			}
#endif
			continue;
		}

		mop = env->me_pghead;
		mop_len = (mop ? mop[0] : 0) + txn->mt_loose_count;

		/* Reserve records for me_pghead[]. Split it if multi-page,
		 * to avoid searching freeDB for a page range. Use keys in
		 * range [1,me_pglast]: Smaller than txnid of oldest reader.
		 */
		if (total_room >= mop_len) {
			if (total_room == mop_len || --more < 0)
				break;
		} else if (head_room >= maxfree_1pg && head_id > 1) {
			/* Keep current record (overflow page), add a new one */
			head_id--;
			head_room = 0;
		}
		/* (Re)write {key = head_id, IDL length = head_room} */
		total_room -= head_room;
		head_room = mop_len - total_room;
		if (head_room > maxfree_1pg && head_id > 1) {
			/* Overflow multi-page for part of me_pghead */
			head_room /= head_id; /* amortize page sizes */
			head_room += maxfree_1pg - head_room % (maxfree_1pg + 1);
		} else if (head_room < 0) {
			/* Rare case, not bothering to delete this record */
			head_room = 0;
		}
		key.mv_size = sizeof(head_id);
		key.mv_data = &head_id;
		data.mv_size = (head_room + 1) * sizeof(pgno_t);
		rc = _mdb_cursor_put(&mc, &key, &data, MDB_RESERVE);
		if (rc)
			return rc;
		/* IDL is initially empty, zero out at least the length */
		pgs = (pgno_t *)data.mv_data;
		j = head_room > clean_limit ? head_room : 0;
		do {
			pgs[j] = 0;
		} while (--j >= 0);
		total_room += head_room;
	}

	/* Return loose page numbers to me_pghead, though usually none are
	 * left at this point.  The pages themselves remain in dirty_list.
	 */
	if (txn->mt_loose_pgs) {
		MDB_page *mp = txn->mt_loose_pgs;
		unsigned count = txn->mt_loose_count;
		MDB_IDL loose;
		/* Room for loose pages + temp IDL with same */
		if ((rc = mdb_midl_need(&env->me_pghead, 2*count+1)) != 0)
			return rc;
		mop = env->me_pghead;
		loose = mop + MDB_IDL_ALLOCLEN(mop) - count;
		for (count = 0; mp; mp = NEXT_LOOSE_PAGE(mp))
			loose[ ++count ] = mp->mp_pgno;
		loose[0] = count;
		mdb_midl_sort(loose);
		mdb_midl_xmerge(mop, loose);
		txn->mt_loose_pgs = NULL;
		txn->mt_loose_count = 0;
		mop_len = mop[0];
	}

	/* Fill in the reserved me_pghead records */
	rc = MDB_SUCCESS;
	if (mop_len) {
		MDB_val key, data;

		mop += mop_len;
		rc = mdb_cursor_first(&mc, &key, &data);
		for (; !rc; rc = mdb_cursor_next(&mc, &key, &data, MDB_NEXT)) {
			txnid_t id = *(txnid_t *)key.mv_data;
			ssize_t	len = (ssize_t)(data.mv_size / sizeof(MDB_ID)) - 1;
			MDB_ID save;

			mdb_tassert(txn, len >= 0 && id <= env->me_pglast);
			key.mv_data = &id;
			if (len > mop_len) {
				len = mop_len;
				data.mv_size = (len + 1) * sizeof(MDB_ID);
			}
			data.mv_data = mop -= len;
			save = mop[0];
			mop[0] = len;
			rc = _mdb_cursor_put(&mc, &key, &data, MDB_CURRENT);
			mop[0] = save;
			if (rc || !(mop_len -= len))
				break;
		}
	}
	return rc;
}

/** Flush (some) dirty pages to the map, after clearing their dirty flag.
 * @param[in] txn the transaction that's being committed
 * @param[in] keep number of initial pages in dirty_list to keep dirty.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_flush(MDB_txn *txn, int keep)
{
	MDB_env		*env = txn->mt_env;
	MDB_ID2L	dl = txn->mt_u.dirty_list;
	unsigned	psize = env->me_psize, j;
	int			i, pagecount = dl[0].mid, rc;
	size_t		size = 0;
	MDB_OFF_T	pos = 0;
	pgno_t		pgno = 0;
	MDB_page	*dp = NULL;
#ifdef _WIN32
	OVERLAPPED	*ov = env->ov;
	MDB_page	*wdp;
	int async_i = 0;
	HANDLE fd = (env->me_flags & MDB_NOSYNC) ? env->me_fd : env->me_ovfd;
#else
	struct iovec iov[MDB_COMMIT_PAGES];
	HANDLE fd = env->me_fd;
#endif
	ssize_t		wsize = 0, wres;
	MDB_OFF_T	wpos = 0, next_pos = 1; /* impossible pos, so pos != next_pos */
	int			n = 0;

	j = i = keep;
	if (env->me_flags & MDB_WRITEMAP
#ifdef _WIN32
		/* In windows, we still do writes to the file (with write-through enabled in sync mode),
		 * as this is faster than FlushViewOfFile/FlushFileBuffers */
		&& ((env->me_flags & MDB_NOSYNC) || (env->me_flags & MDB_INMEMORY))
#endif
		) {
		/* Clear dirty flags */
		while (++i <= pagecount) {
			dp = dl[i].mptr;
			/* Don't flush this page yet */
			if (dp->mp_flags & (P_LOOSE|P_KEEP)) {
				dp->mp_flags &= ~P_KEEP;
				dl[++j] = dl[i];
				continue;
			}
			dp->mp_flags &= ~P_DIRTY;
		}
		goto done;
	}

#ifdef _WIN32
	if (pagecount - keep >= env->ovs) {
		/* ran out of room in ov array, and re-malloc, copy handles and free previous */
		int ovs = (pagecount - keep) * 1.5; /* provide extra padding to reduce number of re-allocations */
		int new_size = ovs * sizeof(OVERLAPPED);
		ov = malloc(new_size);
		if (ov == NULL)
			return ENOMEM;
		int previous_size = env->ovs * sizeof(OVERLAPPED);
		memcpy(ov, env->ov, previous_size); /* Copy previous OVERLAPPED data to retain event handles */
		/* And clear rest of memory */
		memset(&ov[env->ovs], 0, new_size - previous_size);
		if (env->ovs > 0) {
			free(env->ov); /* release previous allocation */
		}

		env->ov = ov;
		env->ovs = ovs;
	}
#endif

	/* Write the pages */
	for (;;) {
		if (++i <= pagecount) {
			dp = dl[i].mptr;
			/* Don't flush this page yet */
			if (dp->mp_flags & (P_LOOSE|P_KEEP)) {
				dp->mp_flags &= ~P_KEEP;
				dl[i].mid = 0;
				continue;
			}
			pgno = dl[i].mid;
			/* clear dirty flag */
			dp->mp_flags &= ~P_DIRTY;
			pos = pgno * psize;
			size = psize;
			if (IS_OVERFLOW(dp)) size *= dp->mp_pages;
		}
		/* Write up to MDB_COMMIT_PAGES dirty pages at a time. */
		if (pos!=next_pos || n==MDB_COMMIT_PAGES || wsize+size>MAX_WRITE
#ifdef _WIN32
			/* If writemap is enabled, consecutive page positions infer
			 * contiguous (mapped) memory.
			 * Otherwise force write pages one at a time.
			 * Windows actually supports scatter/gather I/O, but only on
			 * unbuffered file handles. Since we're relying on the OS page
			 * cache for all our data, that's self-defeating. So we just
			 * write pages one at a time. We use the ov structure to set
			 * the write offset, to at least save the overhead of a Seek
			 * system call.
			 */
			|| !(env->me_flags & MDB_WRITEMAP)
#endif
			) {
			if (n) {
retry_write:
				/* Write previous page(s) */
				DPRINTF(("committing page %"Z"u", pgno));
#ifdef _WIN32
				OVERLAPPED *this_ov = &ov[async_i];
				/* Clear status, and keep hEvent, we reuse that */
				this_ov->Internal = 0;
				this_ov->Offset = wpos & 0xffffffff;
				this_ov->OffsetHigh = wpos >> 16 >> 16;
				if (!F_ISSET(env->me_flags, MDB_NOSYNC) && !this_ov->hEvent) {
					HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
					if (!event) {
						rc = ErrCode();
						DPRINTF(("CreateEvent: %s", strerror(rc)));
						return rc;
					}
					this_ov->hEvent = event;
				}
				if (!WriteFile(fd, wdp, wsize, NULL, this_ov)) {
					rc = ErrCode();
					if (rc != ERROR_IO_PENDING) {
						DPRINTF(("WriteFile: %d", rc));
						return rc;
					}
				}
				async_i++;
#else
#ifdef MDB_USE_PWRITEV
				wres = pwritev(fd, iov, n, wpos);
#else
				if (n == 1) {
					wres = pwrite(fd, iov[0].iov_base, wsize, wpos);
				} else {
retry_seek:
					if (lseek(fd, wpos, SEEK_SET) == -1) {
						rc = ErrCode();
						if (rc == EINTR)
							goto retry_seek;
						DPRINTF(("lseek: %s", strerror(rc)));
						return rc;
					}
					wres = writev(fd, iov, n);
				}
#endif
				if (wres != wsize) {
					if (wres < 0) {
						rc = ErrCode();
						if (rc == EINTR)
							goto retry_write;
						DPRINTF(("Write error: %s", strerror(rc)));
					} else {
						rc = EIO; /* TODO: Use which error code? */
						DPUTS("short write, filesystem full?");
					}
					return rc;
				}
#endif /* _WIN32 */
				n = 0;
			}
			if (i > pagecount)
				break;
			wpos = pos;
			wsize = 0;
#ifdef _WIN32
			wdp = dp;
		}
#else
		}
		iov[n].iov_len = size;
		iov[n].iov_base = (char *)dp;
#endif	/* _WIN32 */
		DPRINTF(("committing page %"Yu, pgno));
		next_pos = pos + size;
		wsize += size;
		n++;
	}
#ifdef MDB_VL32
	if (pgno > txn->mt_last_pgno)
		txn->mt_last_pgno = pgno;
#endif

#ifdef _WIN32
	if (!F_ISSET(env->me_flags, MDB_NOSYNC)) {
		/* Now wait for all the asynchronous/overlapped sync/write-through writes to complete.
		* We start with the last one so that all the others should already be complete and
		* we reduce thread suspend/resuming (in practice, typically about 99.5% of writes are
		* done after the last write is done) */
		rc = 0;
		while (--async_i >= 0) {
			if (ov[async_i].hEvent) {
				DWORD temp_wres;
				if (!GetOverlappedResult(fd, &ov[async_i], &temp_wres, TRUE)) {
					rc = ErrCode(); /* Continue on so that all the event signals are reset */
				}
				wres = temp_wres;
			}
		}
		if (rc) { /* any error on GetOverlappedResult, exit now */
			return rc;
		}
	}
#endif	/* _WIN32 */

	if (!(env->me_flags & MDB_WRITEMAP)) {
		/* Don't free pages when using writemap (can only get here in NOSYNC mode in Windows)
		 * MIPS has cache coherency issues, this is a no-op everywhere else
		 * Note: for any size >= on-chip cache size, entire on-chip cache is
		 * flushed.
		 */
		CACHEFLUSH(env->me_map, txn->mt_next_pgno * env->me_psize, DCACHE);

		for (i = keep; ++i <= pagecount; ) {
			dp = dl[i].mptr;
			/* This is a page we skipped above */
			if (!dl[i].mid) {
				dl[++j] = dl[i];
				dl[j].mid = dp->mp_pgno;
				continue;
			}
			mdb_dpage_free(env, dp);
		}
	}

done:
	i--;
	txn->mt_dirty_room += i - j;
	dl[0].mid = j;
	return MDB_SUCCESS;
}

static int ESECT mdb_env_share_locks(MDB_env *env, int *excl);
static int ESECT mdb_env_setup_inmem_locks(MDB_env *env);

static int
_mdb_txn_commit(MDB_txn *txn)
{
	int		rc;
	unsigned int i, end_mode;
	MDB_env	*env;

	if (txn == NULL)
		return EINVAL;

	/* mdb_txn_end() mode for a commit which writes nothing */
	end_mode = MDB_END_EMPTY_COMMIT|MDB_END_UPDATE|MDB_END_SLOT|MDB_END_FREE;

	if (txn->mt_child) {
		rc = _mdb_txn_commit(txn->mt_child);
		if (rc)
			goto fail;
	}

	env = txn->mt_env;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY)) {
		goto done;
	}

	if (txn->mt_flags & (MDB_TXN_FINISHED|MDB_TXN_ERROR)) {
		DPUTS("txn has failed/finished, can't commit");
		if (txn->mt_parent)
			mdb_txn_mark_error(txn->mt_parent, MDB_BAD_TXN);
		rc = MDB_BAD_TXN;
		goto fail;
	}

	if (txn->mt_parent) {
		MDB_txn *parent = txn->mt_parent;
		MDB_page **lp;
		MDB_ID2L dst, src;
		MDB_IDL pspill;
		unsigned x, y, len, ps_len;

		/* Append our free list to parent's */
		rc = mdb_midl_append_list(&parent->mt_free_pgs, txn->mt_free_pgs);
		if (rc)
			goto fail;
		mdb_midl_free(txn->mt_free_pgs);
		/* Failures after this must either undo the changes
		 * to the parent or set MDB_TXN_ERROR in the parent.
		 */

		parent->mt_next_pgno = txn->mt_next_pgno;
		parent->mt_flags = txn->mt_flags;

		/* Merge our cursors into parent's and close them */
		mdb_cursors_close(txn, 1);

		/* Update parent's DB table. */
		memcpy(parent->mt_dbs, txn->mt_dbs, txn->mt_numdbs * sizeof(MDB_db));
		parent->mt_numdbs = txn->mt_numdbs;
		parent->mt_dbflags[FREE_DBI] = txn->mt_dbflags[FREE_DBI];
		parent->mt_dbflags[MAIN_DBI] = txn->mt_dbflags[MAIN_DBI];
		for (i=CORE_DBS; i<txn->mt_numdbs; i++) {
			/* preserve parent's DB_NEW status */
			x = parent->mt_dbflags[i] & DB_NEW;
			parent->mt_dbflags[i] = txn->mt_dbflags[i] | x;
		}

		dst = parent->mt_u.dirty_list;
		src = txn->mt_u.dirty_list;
		/* Remove anything in our dirty list from parent's spill list */
		if ((pspill = parent->mt_spill_pgs) && (ps_len = pspill[0])) {
			x = y = ps_len;
			pspill[0] = (pgno_t)-1;
			/* Mark our dirty pages as deleted in parent spill list */
			for (i=0, len=src[0].mid; ++i <= len; ) {
				MDB_ID pn = src[i].mid << 1;
				while (pn > pspill[x])
					x--;
				if (pn == pspill[x]) {
					pspill[x] = 1;
					y = --x;
				}
			}
			/* Squash deleted pagenums if we deleted any */
			for (x=y; ++x <= ps_len; )
				if (!(pspill[x] & 1))
					pspill[++y] = pspill[x];
			pspill[0] = y;
		}

		/* Remove anything in our spill list from parent's dirty list */
		if (txn->mt_spill_pgs && txn->mt_spill_pgs[0]) {
			for (i=1; i<=txn->mt_spill_pgs[0]; i++) {
				MDB_ID pn = txn->mt_spill_pgs[i];
				if (pn & 1)
					continue;	/* deleted spillpg */
				pn >>= 1;
				y = mdb_mid2l_search(dst, pn);
				if (y <= dst[0].mid && dst[y].mid == pn) {
					free(dst[y].mptr);
					while (y < dst[0].mid) {
						dst[y] = dst[y+1];
						y++;
					}
					dst[0].mid--;
				}
			}
		}

		/* Find len = length of merging our dirty list with parent's */
		x = dst[0].mid;
		dst[0].mid = 0;		/* simplify loops */
		if (parent->mt_parent) {
			len = x + src[0].mid;
			y = mdb_mid2l_search(src, dst[x].mid + 1) - 1;
			for (i = x; y && i; y--) {
				pgno_t yp = src[y].mid;
				while (yp < dst[i].mid)
					i--;
				if (yp == dst[i].mid) {
					i--;
					len--;
				}
			}
		} else { /* Simplify the above for single-ancestor case */
			len = MDB_IDL_UM_MAX - txn->mt_dirty_room;
		}
		/* Merge our dirty list with parent's */
		y = src[0].mid;
		for (i = len; y; dst[i--] = src[y--]) {
			pgno_t yp = src[y].mid;
			while (yp < dst[x].mid)
				dst[i--] = dst[x--];
			if (yp == dst[x].mid)
				free(dst[x--].mptr);
		}
		mdb_tassert(txn, i == x);
		dst[0].mid = len;
		free(txn->mt_u.dirty_list);
		parent->mt_dirty_room = txn->mt_dirty_room;
		if (txn->mt_spill_pgs) {
			if (parent->mt_spill_pgs) {
				/* TODO: Prevent failure here, so parent does not fail */
				rc = mdb_midl_append_list(&parent->mt_spill_pgs, txn->mt_spill_pgs);
				if (rc)
					mdb_txn_mark_error(parent, rc);
				mdb_midl_free(txn->mt_spill_pgs);
				mdb_midl_sort(parent->mt_spill_pgs);
			} else {
				parent->mt_spill_pgs = txn->mt_spill_pgs;
			}
		}

		/* Append our loose page list to parent's */
		for (lp = &parent->mt_loose_pgs; *lp; lp = &NEXT_LOOSE_PAGE(*lp))
			;
		*lp = txn->mt_loose_pgs;
		parent->mt_loose_count += txn->mt_loose_count;

		parent->mt_child = NULL;
		mdb_midl_free(((MDB_ntxn *)txn)->mnt_pgstate.mf_pghead);
		free(txn);
		return rc;
	}

	if (txn != env->me_txn) {
		DPUTS("attempt to commit unknown transaction");
		rc = EINVAL;
		goto fail;
	}

	mdb_cursors_close(txn, 0);

	if (!txn->mt_u.dirty_list[0].mid &&
		!(txn->mt_flags & (MDB_TXN_DIRTY|MDB_TXN_SPILLS)))
		goto done;

	DPRINTF(("committing txn %"Yu" %p on mdbenv %p, root page %"Yu,
	    txn->mt_txnid, (void*)txn, (void*)env, txn->mt_dbs[MAIN_DBI].md_root));

	/* Update DB root pointers */
		if (txn->mt_numdbs > CORE_DBS) {
			MDB_cursor mc = (MDB_cursor){0};
		MDB_dbi i;
		MDB_val data;
		data.mv_size = sizeof(MDB_db);

		mdb_cursor_init(&mc, txn, MAIN_DBI, NULL);
		for (i = CORE_DBS; i < txn->mt_numdbs; i++) {
			if (txn->mt_dbflags[i] & DB_DIRTY) {
				if (TXN_DBI_CHANGED(txn, i)) {
					rc = MDB_BAD_DBI;
					goto fail;
				}
				data.mv_data = &txn->mt_dbs[i];
				rc = _mdb_cursor_put(&mc, &txn->mt_dbxs[i].md_name, &data,
					F_SUBDATA);
				if (rc)
					goto fail;
			}
		}
	}

	rc = mdb_freelist_save(txn);
	if (rc)
		goto fail;

	mdb_midl_free(env->me_pghead);
	env->me_pghead = NULL;
	mdb_midl_shrink(&txn->mt_free_pgs);

#if (MDB_DEBUG) > 2
	mdb_audit(txn);
#endif

	if ((rc = mdb_page_flush(txn, 0)))
		goto fail;
	if (!F_ISSET(txn->mt_flags, MDB_TXN_NOSYNC) &&
		(rc = mdb_env_sync0(env, 0, txn->mt_next_pgno)))
		goto fail;
	if ((rc = mdb_env_write_meta(txn)))
		goto fail;
	end_mode = MDB_END_COMMITTED|MDB_END_UPDATE;
	if (env->me_flags & MDB_PREVSNAPSHOT) {
		if (!(env->me_flags & MDB_NOLOCK)) {
			int excl;
			rc = mdb_env_share_locks(env, &excl);
			if (rc)
				goto fail;
		}
		env->me_flags ^= MDB_PREVSNAPSHOT;
	}

done:
	mdb_txn_end(txn, end_mode);
	return MDB_SUCCESS;

fail:
	_mdb_txn_abort(txn);
	return rc;
}

int
mdb_txn_commit(MDB_txn *txn)
{
	MDB_TRACE(("%p", txn));
	return _mdb_txn_commit(txn);
}

/** Read the environment parameters of a DB environment before
 * mapping it into memory.
 * @param[in] env the environment handle
 * @param[in] prev whether to read the backup meta page
 * @param[out] meta address of where to store the meta information
 * @return 0 on success, non-zero on failure.
 */
static int ESECT
mdb_env_read_header(MDB_env *env, int prev, MDB_meta *meta)
{
	MDB_metabuf	pbuf;
	MDB_page	*p;
	MDB_meta	*m;
	int			i, rc, off;
	enum { Size = sizeof(pbuf) };

	/* We don't know the page size yet, so use a minimum value.
	 * Read both meta pages so we can use the latest one.
	 */

	for (i=off=0; i<NUM_METAS; i++, off += meta->mm_psize) {
#ifdef _WIN32
		DWORD len;
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		ov.Offset = off;
		rc = ReadFile(env->me_fd, &pbuf, Size, &len, &ov) ? (int)len : -1;
		if (rc == -1 && ErrCode() == ERROR_HANDLE_EOF)
			rc = 0;
#else
		rc = pread(env->me_fd, &pbuf, Size, off);
#endif
		if (rc != Size) {
			if (rc == 0 && off == 0)
				return ENOENT;
			rc = rc < 0 ? (int) ErrCode() : MDB_INVALID;
			DPRINTF(("read: %s", mdb_strerror(rc)));
			return rc;
		}

		p = (MDB_page *)&pbuf;

		if (!F_ISSET(p->mp_flags, P_META)) {
			DPRINTF(("page %"Yu" not a meta page", p->mp_pgno));
			return MDB_INVALID;
		}

		m = METADATA(p);
		if (m->mm_magic != MDB_MAGIC) {
			DPUTS("meta has invalid magic");
			return MDB_INVALID;
		}

		if (m->mm_version != MDB_DATA_VERSION) {
			DPRINTF(("database is version %u, expected version %u",
				m->mm_version, MDB_DATA_VERSION));
			return MDB_VERSION_MISMATCH;
		}

		if (off == 0 || (prev ? m->mm_txnid < meta->mm_txnid : m->mm_txnid > meta->mm_txnid))
			*meta = *m;
	}
	return 0;
}

/** Fill in most of the zeroed #MDB_meta for an empty database environment */
static void ESECT
mdb_env_init_meta0(MDB_env *env, MDB_meta *meta)
{
	meta->mm_magic = MDB_MAGIC;
	meta->mm_version = MDB_DATA_VERSION;
	meta->mm_mapsize = env->me_mapsize;
	meta->mm_psize = env->me_psize;
	meta->mm_last_pg = NUM_METAS-1;
	meta->mm_flags = env->me_flags & 0xffff;
	meta->mm_flags |= MDB_INTEGERKEY; /* this is mm_dbs[FREE_DBI].md_flags */
	meta->mm_dbs[FREE_DBI].md_root = P_INVALID;
	meta->mm_dbs[MAIN_DBI].md_root = P_INVALID;
}

/** Write the environment parameters of a freshly created DB environment.
 * @param[in] env the environment handle
 * @param[in] meta the #MDB_meta to write
 * @return 0 on success, non-zero on failure.
 */
static int ESECT
mdb_env_init_meta(MDB_env *env, MDB_meta *meta)
{
	MDB_page *p, *q;
	int rc;
	unsigned int	 psize;
#ifdef _WIN32
	DWORD len;
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
#define DO_PWRITE(rc, fd, ptr, size, len, pos)	do { \
	ov.Offset = pos;	\
	rc = WriteFile(fd, ptr, size, &len, &ov);	} while(0)
#else
	int len;
#define DO_PWRITE(rc, fd, ptr, size, len, pos)	do { \
	len = pwrite(fd, ptr, size, pos);	\
	if (len == -1 && ErrCode() == EINTR) continue; \
	rc = (len >= 0); break; } while(1)
#endif

	if (env->me_flags & MDB_INMEMORY) {
		p = (MDB_page *)env->me_map;
		if (!p)
			return EINVAL;
		psize = env->me_psize;
		memset(p, 0, psize * NUM_METAS);
		p->mp_pgno = 0;
		p->mp_flags = P_META;
		*(MDB_meta *)METADATA(p) = *meta;

		q = (MDB_page *)((char *)p + psize);
		q->mp_pgno = 1;
		q->mp_flags = P_META;
		*(MDB_meta *)METADATA(q) = *meta;
		return MDB_SUCCESS;
	}

	DPUTS("writing new meta page");

	psize = env->me_psize;

	p = calloc(NUM_METAS, psize);
	if (!p)
		return ENOMEM;
	p->mp_pgno = 0;
	p->mp_flags = P_META;
	*(MDB_meta *)METADATA(p) = *meta;

	q = (MDB_page *)((char *)p + psize);
	q->mp_pgno = 1;
	q->mp_flags = P_META;
	*(MDB_meta *)METADATA(q) = *meta;

	DO_PWRITE(rc, env->me_fd, p, psize * NUM_METAS, len, 0);
	if (!rc)
		rc = ErrCode();
	else if ((unsigned) len == psize * NUM_METAS)
		rc = MDB_SUCCESS;
	else
		rc = ENOSPC;
	free(p);
	return rc;
}

/** Update the environment info to commit a transaction.
 * @param[in] txn the transaction that's being committed
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_env_write_meta(MDB_txn *txn)
{
	MDB_env *env;
	MDB_meta	meta, metab, *mp;
	unsigned flags;
	mdb_size_t mapsize;
	MDB_OFF_T off;
	int rc, len, toggle;
	char *ptr;
	HANDLE mfd;
#ifdef _WIN32
	OVERLAPPED ov;
#else
	int r2;
#endif

	toggle = txn->mt_txnid & 1;
	DPRINTF(("writing meta page %d for root page %"Yu,
		toggle, txn->mt_dbs[MAIN_DBI].md_root));

	env = txn->mt_env;
	flags = txn->mt_flags | env->me_flags;
	mp = env->me_metas[toggle];
	mapsize = env->me_metas[toggle ^ 1]->mm_mapsize;
	/* Persist any increases of mapsize config */
	if (mapsize < env->me_mapsize)
		mapsize = env->me_mapsize;

	if (flags & MDB_INMEMORY) {
		mp->mm_mapsize = mapsize;
		mp->mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
		mp->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
		mp->mm_last_pg = txn->mt_next_pgno - 1;
#if (__GNUC__ * 100 + __GNUC_MINOR__ >= 404) && /* TODO: portability */	\
	!(defined(__i386__) || defined(__x86_64__))
		/* LY: issue a memory barrier, if not x86. ITS#7969 */
		__sync_synchronize();
#endif
		mp->mm_txnid = txn->mt_txnid;
		goto done;
	}

#ifndef _WIN32 /* We don't want to ever use MSYNC/FlushViewOfFile in Windows */
	if (flags & MDB_WRITEMAP) {
		mp->mm_mapsize = mapsize;
		mp->mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
		mp->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
		mp->mm_last_pg = txn->mt_next_pgno - 1;
#if (__GNUC__ * 100 + __GNUC_MINOR__ >= 404) && /* TODO: portability */	\
	!(defined(__i386__) || defined(__x86_64__))
		/* LY: issue a memory barrier, if not x86. ITS#7969 */
		__sync_synchronize();
#endif
		mp->mm_txnid = txn->mt_txnid;
		if (!(flags & MDB_INMEMORY) &&
			!(flags & (MDB_NOMETASYNC|MDB_NOSYNC))) {
			unsigned meta_size = env->me_psize;
			rc = (env->me_flags & MDB_MAPASYNC) ? MS_ASYNC : MS_SYNC;
			ptr = (char *)mp - PAGEHDRSZ;
			/* POSIX msync() requires ptr = start of OS page */
			r2 = (ptr - env->me_map) & (env->me_os_psize - 1);
			ptr -= r2;
			meta_size += r2;
			if (MDB_MSYNC(ptr, meta_size, rc)) {
				rc = ErrCode();
				goto fail;
			}
		}
		goto done;
	}
#endif
	metab.mm_txnid = mp->mm_txnid;
	metab.mm_last_pg = mp->mm_last_pg;

	meta.mm_mapsize = mapsize;
	meta.mm_dbs[FREE_DBI] = txn->mt_dbs[FREE_DBI];
	meta.mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
	meta.mm_last_pg = txn->mt_next_pgno - 1;
	meta.mm_txnid = txn->mt_txnid;

	off = offsetof(MDB_meta, mm_mapsize);
	ptr = (char *)&meta + off;
	len = sizeof(MDB_meta) - off;
	off += (char *)mp - env->me_map;

	/* Write to the SYNC fd unless MDB_NOSYNC/MDB_NOMETASYNC.
	 * (me_mfd goes to the same file as me_fd, but writing to it
	 * also syncs to disk.  Avoids a separate fdatasync() call.)
	 */
	mfd = (flags & (MDB_NOSYNC|MDB_NOMETASYNC)) ? env->me_fd : env->me_mfd;
#ifdef _WIN32
	{
		memset(&ov, 0, sizeof(ov));
		ov.Offset = off;
		if (!WriteFile(mfd, ptr, len, (DWORD *)&rc, &ov))
			rc = -1;
	}
#else
retry_write:
	rc = pwrite(mfd, ptr, len, off);
#endif
	if (rc != len) {
		rc = rc < 0 ? ErrCode() : EIO;
#ifndef _WIN32
		if (rc == EINTR)
			goto retry_write;
#endif
		DPUTS("write failed, disk error?");
		/* On a failure, the pagecache still contains the new data.
		 * Write some old data back, to prevent it from being used.
		 * Use the non-SYNC fd; we know it will fail anyway.
		 */
		meta.mm_last_pg = metab.mm_last_pg;
		meta.mm_txnid = metab.mm_txnid;
#ifdef _WIN32
		memset(&ov, 0, sizeof(ov));
		ov.Offset = off;
		WriteFile(env->me_fd, ptr, len, NULL, &ov);
#else
		r2 = pwrite(env->me_fd, ptr, len, off);
		(void)r2;	/* Silence warnings. We don't care about pwrite's return value */
#endif
fail:
		env->me_flags |= MDB_FATAL_ERROR;
		return rc;
	}
	/* MIPS has cache coherency issues, this is a no-op everywhere else */
	CACHEFLUSH(env->me_map + off, len, DCACHE);
done:
	/* Memory ordering issues are irrelevant; since the entire writer
	 * is wrapped by wmutex, all of these changes will become visible
	 * after the wmutex is unlocked. Since the DB is multi-version,
	 * readers will get consistent data regardless of how fresh or
	 * how stale their view of these values is.
	 */
	if (env->me_txns)
		env->me_txns->mti_txnid = txn->mt_txnid;

	return MDB_SUCCESS;
}

/** Check both meta pages to see which one is newer.
 * @param[in] env the environment handle
 * @return newest #MDB_meta.
 */
static MDB_meta *
mdb_env_pick_meta(const MDB_env *env)
{
	MDB_meta *const *metas = env->me_metas;
	return metas[ (metas[0]->mm_txnid < metas[1]->mm_txnid) ^
		((env->me_flags & MDB_PREVSNAPSHOT) != 0) ];
}

int ESECT
mdb_env_create(MDB_env **env)
{
	MDB_env *e;

	e = calloc(1, sizeof(MDB_env));
	if (!e)
		return ENOMEM;

	e->me_maxreaders = DEFAULT_READERS;
	e->me_maxdbs = e->me_numdbs = CORE_DBS;
	e->me_fd = INVALID_HANDLE_VALUE;
	e->me_lfd = INVALID_HANDLE_VALUE;
	e->me_mfd = INVALID_HANDLE_VALUE;
#ifdef MDB_USE_POSIX_SEM
	e->me_rmutex = SEM_FAILED;
	e->me_wmutex = SEM_FAILED;
#elif defined MDB_USE_SYSV_SEM
	e->me_rmutex->semid = -1;
	e->me_wmutex->semid = -1;
#endif
	e->me_pid = getpid();
	GET_PAGESIZE(e->me_os_psize);
	VGMEMP_CREATE(e,0,0);
	*env = e;
	MDB_TRACE(("%p", e));
	return MDB_SUCCESS;
}

#ifdef _WIN32
/** @brief Map a result from an NTAPI call to WIN32. */
static DWORD
mdb_nt2win32(NTSTATUS st)
{
	OVERLAPPED o = {0};
	DWORD br;
	o.Internal = st;
	GetOverlappedResult(NULL, &o, &br, FALSE);
	return GetLastError();
}
#endif

static int ESECT
mdb_env_map(MDB_env *env, void *addr)
{
	MDB_page *p;
	unsigned int flags = env->me_flags;
#ifdef _WIN32
	int rc;
	int access = SECTION_MAP_READ;
	HANDLE mh;
	void *map;
	SIZE_T msize;
	ULONG pageprot = PAGE_READONLY, secprot, alloctype;

	if (flags & MDB_INMEMORY) {
		DWORD mlo, mhi;
		if (flags & MDB_RDONLY)
			return EINVAL;
		mlo = (DWORD)(env->me_mapsize & 0xffffffff);
		mhi = (DWORD)(env->me_mapsize >> 16 >> 16);
		mh = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
			mhi, mlo, NULL);
		if (!mh)
			return ErrCode();
		map = MapViewOfFileEx(mh, FILE_MAP_WRITE, 0, 0, env->me_mapsize, addr);
		CloseHandle(mh);
		if (!map)
			return ErrCode();
	} else {
		if (flags & MDB_WRITEMAP) {
			access |= SECTION_MAP_WRITE;
			pageprot = PAGE_READWRITE;
		}
		if (flags & MDB_RDONLY) {
			secprot = PAGE_READONLY;
			msize = 0;
			alloctype = 0;
		} else {
			secprot = PAGE_READWRITE;
			msize = env->me_mapsize;
			alloctype = MEM_RESERVE;
		}

		/** Some users are afraid of seeing their disk space getting used
		 * all at once, so the default is now to do incremental file growth.
		 * But that has a large performance impact, so give the option of
		 * allocating the file up front.
		 */
#ifdef MDB_FIXEDSIZE
		LARGE_INTEGER fsize;
		fsize.LowPart = msize & 0xffffffff;
		fsize.HighPart = msize >> 16 >> 16;
		rc = NtCreateSection(&mh, access, NULL, &fsize, secprot, SEC_RESERVE, env->me_fd);
#else
		rc = NtCreateSection(&mh, access, NULL, NULL, secprot, SEC_RESERVE, env->me_fd);
#endif
		if (rc)
			return mdb_nt2win32(rc);
		map = addr;
#ifdef MDB_VL32
		msize = NUM_METAS * env->me_psize;
#endif
		rc = NtMapViewOfSection(mh, GetCurrentProcess(), &map, 0, 0, NULL, &msize, ViewUnmap, alloctype, pageprot);
#ifdef MDB_VL32
		env->me_fmh = mh;
#else
		NtClose(mh);
#endif
		if (rc)
			return mdb_nt2win32(rc);
	}
	env->me_map = map;
#else
	int mmap_flags = MAP_SHARED;
	int prot = PROT_READ;
#ifdef MAP_NOSYNC	/* Used on FreeBSD */
	if (flags & MDB_NOSYNC)
		mmap_flags |= MAP_NOSYNC;
#endif
#ifdef MDB_VL32
	if (flags & MDB_INMEMORY)
		return ENOSYS;
	(void) flags;
	env->me_map = mmap(addr, NUM_METAS * env->me_psize, prot, mmap_flags,
		env->me_fd, 0);
	if (env->me_map == MAP_FAILED) {
		env->me_map = NULL;
		return ErrCode();
	}
#else
	if (flags & MDB_INMEMORY) {
#ifdef MDB_MAP_ANON
		prot |= PROT_WRITE;
		mmap_flags |= MDB_MAP_ANON;
		env->me_map = mmap(addr, env->me_mapsize, prot, mmap_flags, -1, 0);
#else
		return ENOSYS;
#endif
	} else {
		if (flags & MDB_WRITEMAP) {
			prot |= PROT_WRITE;
			if (ftruncate(env->me_fd, env->me_mapsize) < 0)
				return ErrCode();
		}
		env->me_map = mmap(addr, env->me_mapsize, prot, mmap_flags,
			env->me_fd, 0);
	}
	if (env->me_map == MAP_FAILED) {
		env->me_map = NULL;
		return ErrCode();
	}

	if (flags & MDB_NORDAHEAD) {
		/* Turn off readahead. It's harmful when the DB is larger than RAM. */
#ifdef MADV_RANDOM
		madvise(env->me_map, env->me_mapsize, MADV_RANDOM);
#else
#ifdef POSIX_MADV_RANDOM
		posix_madvise(env->me_map, env->me_mapsize, POSIX_MADV_RANDOM);
#endif /* POSIX_MADV_RANDOM */
#endif /* MADV_RANDOM */
	}
#endif /* _WIN32 */

	/* Can happen because the address argument to mmap() is just a
	 * hint.  mmap() can pick another, e.g. if the range is in use.
	 * The MAP_FIXED flag would prevent that, but then mmap could
	 * instead unmap existing pages to make room for the new map.
	 */
	if (addr && env->me_map != addr)
		return EBUSY;	/* TODO: Make a new MDB_* error code? */
#endif

	p = (MDB_page *)env->me_map;
	env->me_metas[0] = METADATA(p);
	env->me_metas[1] = (MDB_meta *)((char *)env->me_metas[0] + env->me_psize);

	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_mapsize(MDB_env *env, mdb_size_t size)
{
	/* If env is already open, caller is responsible for making
	 * sure there are no active txns.
	 */
	if (env->me_map) {
		MDB_meta *meta;
#ifndef MDB_VL32
		void *old;
		int rc;
#endif
		if (env->me_txn)
			return EINVAL;
		meta = mdb_env_pick_meta(env);
		if (!size)
			size = meta->mm_mapsize;
		{
			/* Silently round up to minimum if the size is too small */
			mdb_size_t minsize = (meta->mm_last_pg + 1) * env->me_psize;
			if (size < minsize)
				size = minsize;
		}
#ifndef MDB_VL32
		/* For MDB_VL32 this bit is a noop since we dynamically remap
		 * chunks of the DB anyway.
		 */
		munmap(env->me_map, env->me_mapsize);
		env->me_mapsize = size;
		old = (env->me_flags & MDB_FIXEDMAP) ? env->me_map : NULL;
		rc = mdb_env_map(env, old);
		if (rc)
			return rc;
#endif /* !MDB_VL32 */
	}
	env->me_mapsize = size;
	if (env->me_psize)
		env->me_maxpg = env->me_mapsize / env->me_psize;
	MDB_TRACE(("%p, %"Yu"", env, size));
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_maxdbs(MDB_env *env, MDB_dbi dbs)
{
	if (env->me_map)
		return EINVAL;
	env->me_maxdbs = dbs + CORE_DBS;
	MDB_TRACE(("%p, %u", env, dbs));
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_maxreaders(MDB_env *env, unsigned int readers)
{
	if (env->me_map || readers < 1)
		return EINVAL;
	env->me_maxreaders = readers;
	MDB_TRACE(("%p, %u", env, readers));
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_maxreaders(MDB_env *env, unsigned int *readers)
{
	if (!env || !readers)
		return EINVAL;
	*readers = env->me_maxreaders;
	return MDB_SUCCESS;
}

static int ESECT
mdb_fsize(HANDLE fd, mdb_size_t *size)
{
#ifdef _WIN32
	LARGE_INTEGER fsize;

	if (!GetFileSizeEx(fd, &fsize))
		return ErrCode();

	*size = fsize.QuadPart;
#else
	struct stat st;

	if (fstat(fd, &st))
		return ErrCode();

	*size = st.st_size;
#endif
	return MDB_SUCCESS;
}


#ifdef _WIN32
typedef wchar_t	mdb_nchar_t;
# define MDB_NAME(str)	L##str
# define mdb_name_cpy	wcscpy
#else
/** Character type for file names: char on Unix, wchar_t on Windows */
typedef char	mdb_nchar_t;
# define MDB_NAME(str)	str		/**< #mdb_nchar_t[] string literal */
# define mdb_name_cpy	strcpy	/**< Copy name (#mdb_nchar_t string) */
#endif

/** Filename - string of #mdb_nchar_t[] */
typedef struct MDB_name {
	int mn_len;					/**< Length  */
	int mn_alloced;				/**< True if #mn_val was malloced */
	mdb_nchar_t	*mn_val;		/**< Contents */
} MDB_name;

/** Filename suffixes [datafile,lockfile][without,with MDB_NOSUBDIR] */
static const mdb_nchar_t *const mdb_suffixes[2][2] = {
	{ MDB_NAME("/data.mdb"), MDB_NAME("")      },
	{ MDB_NAME("/lock.mdb"), MDB_NAME("-lock") }
};

#define MDB_SUFFLEN 9	/**< Max string length in #mdb_suffixes[] */

/** Set up filename + scratch area for filename suffix, for opening files.
 * It should be freed with #mdb_fname_destroy().
 * On Windows, paths are converted from char *UTF-8 to wchar_t *UTF-16.
 *
 * @param[in] path Pathname for #mdb_env_open().
 * @param[in] envflags Whether a subdir and/or lockfile will be used.
 * @param[out] fname Resulting filename, with room for a suffix if necessary.
 */
static int ESECT
mdb_fname_init(const char *path, unsigned envflags, MDB_name *fname)
{
	int no_suffix = F_ISSET(envflags, MDB_NOSUBDIR|MDB_NOLOCK);
	fname->mn_alloced = 0;
#ifdef _WIN32
	return utf8_to_utf16(path, fname, no_suffix ? 0 : MDB_SUFFLEN);
#else
	fname->mn_len = strlen(path);
	if (no_suffix)
		fname->mn_val = (char *) path;
	else if ((fname->mn_val = malloc(fname->mn_len + MDB_SUFFLEN+1)) != NULL) {
		fname->mn_alloced = 1;
		strcpy(fname->mn_val, path);
	}
	else
		return ENOMEM;
	return MDB_SUCCESS;
#endif
}

/** Destroy \b fname from #mdb_fname_init() */
#define mdb_fname_destroy(fname) \
	do { if ((fname).mn_alloced) free((fname).mn_val); } while (0)

#ifdef O_CLOEXEC /* POSIX.1-2008: Set FD_CLOEXEC atomically at open() */
# define MDB_CLOEXEC		O_CLOEXEC
#else
# define MDB_CLOEXEC		0
#endif

/** File type, access mode etc. for #mdb_fopen() */
enum mdb_fopen_type {
#ifdef _WIN32
	MDB_O_RDONLY, MDB_O_RDWR, MDB_O_OVERLAPPED, MDB_O_META, MDB_O_COPY, MDB_O_LOCKS
#else
	/* A comment in mdb_fopen() explains some O_* flag choices. */
	MDB_O_RDONLY= O_RDONLY,                            /**< for RDONLY me_fd */
	MDB_O_RDWR  = O_RDWR  |O_CREAT,                    /**< for me_fd */
	MDB_O_META  = O_WRONLY|MDB_DSYNC     |MDB_CLOEXEC, /**< for me_mfd */
	MDB_O_COPY  = O_WRONLY|O_CREAT|O_EXCL|MDB_CLOEXEC, /**< for #mdb_env_copy() */
	/** Bitmask for open() flags in enum #mdb_fopen_type.  The other bits
	 * distinguish otherwise-equal MDB_O_* constants from each other.
	 */
	MDB_O_MASK  = MDB_O_RDWR|MDB_CLOEXEC | MDB_O_RDONLY|MDB_O_META|MDB_O_COPY,
	MDB_O_LOCKS = MDB_O_RDWR|MDB_CLOEXEC | ((MDB_O_MASK+1) & ~MDB_O_MASK) /**< for me_lfd */
#endif
};

/** Open an LMDB file.
 * @param[in] env	The LMDB environment.
 * @param[in,out] fname	Path from from #mdb_fname_init().  A suffix is
 * appended if necessary to create the filename, without changing mn_len.
 * @param[in] which	Determines file type, access mode, etc.
 * @param[in] mode	The Unix permissions for the file, if we create it.
 * @param[out] res	Resulting file handle.
 * @return 0 on success, non-zero on failure.
 */
static int ESECT
mdb_fopen(const MDB_env *env, MDB_name *fname,
	enum mdb_fopen_type which, mdb_mode_t mode,
	HANDLE *res)
{
	int rc = MDB_SUCCESS;
	HANDLE fd;
#ifdef _WIN32
	DWORD acc, share, disp, attrs;
#else
	int flags;
#endif

	if (fname->mn_alloced)		/* modifiable copy */
		mdb_name_cpy(fname->mn_val + fname->mn_len,
			mdb_suffixes[which==MDB_O_LOCKS][F_ISSET(env->me_flags, MDB_NOSUBDIR)]);

	/* The directory must already exist.  Usually the file need not.
	 * MDB_O_META requires the file because we already created it using
	 * MDB_O_RDWR.  MDB_O_COPY must not overwrite an existing file.
	 *
	 * With MDB_O_COPY we do not want the OS to cache the writes, since
	 * the source data is already in the OS cache.
	 *
	 * The lockfile needs FD_CLOEXEC (close file descriptor on exec*())
	 * to avoid the flock() issues noted under Caveats in dlmdb.h.
	 * Also set it for other filehandles which the user cannot get at
	 * and close himself, which he may need after fork().  I.e. all but
	 * me_fd, which programs do use via mdb_env_get_fd().
	 */

#ifdef _WIN32
	acc = GENERIC_READ|GENERIC_WRITE;
	share = FILE_SHARE_READ|FILE_SHARE_WRITE;
	disp = OPEN_ALWAYS;
	attrs = FILE_ATTRIBUTE_NORMAL;
	switch (which) {
	case MDB_O_OVERLAPPED: 	/* for unbuffered asynchronous writes (write-through mode)*/
		acc = GENERIC_WRITE;
		disp = OPEN_EXISTING;
		attrs = FILE_FLAG_OVERLAPPED|FILE_FLAG_WRITE_THROUGH;
		break;
	case MDB_O_RDONLY:			/* read-only datafile */
		acc = GENERIC_READ;
		disp = OPEN_EXISTING;
		break;
	case MDB_O_META:			/* for writing metapages */
		acc = GENERIC_WRITE;
		disp = OPEN_EXISTING;
		attrs = FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH;
		break;
	case MDB_O_COPY:			/* mdb_env_copy() & co */
		acc = GENERIC_WRITE;
		share = 0;
		disp = CREATE_NEW;
		attrs = FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH;
		break;
	default: break;	/* silence gcc -Wswitch (not all enum values handled) */
	}
	fd = CreateFileW(fname->mn_val, acc, share, NULL, disp, attrs, NULL);
#else
	fd = open(fname->mn_val, which & MDB_O_MASK, mode);
#endif

	if (fd == INVALID_HANDLE_VALUE)
		rc = ErrCode();
#ifndef _WIN32
	else {
		if (which != MDB_O_RDONLY && which != MDB_O_RDWR) {
			/* Set CLOEXEC if we could not pass it to open() */
			if (!MDB_CLOEXEC && (flags = fcntl(fd, F_GETFD)) != -1)
				(void) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		}
		if (which == MDB_O_COPY && env->me_psize >= env->me_os_psize) {
			/* This may require buffer alignment.  There is no portable
			 * way to ask how much, so we require OS pagesize alignment.
			 */
# ifdef F_NOCACHE	/* __APPLE__ */
			(void) fcntl(fd, F_NOCACHE, 1);
# elif defined O_DIRECT
			/* open(...O_DIRECT...) would break on filesystems without
			 * O_DIRECT support (ITS#7682). Try to set it here instead.
			 */
			if ((flags = fcntl(fd, F_GETFL)) != -1)
				(void) fcntl(fd, F_SETFL, flags | O_DIRECT);
# endif
		}
	}
#endif	/* !_WIN32 */

	*res = fd;
	return rc;
}


#ifdef BROKEN_FDATASYNC
#include <sys/utsname.h>
#include <sys/vfs.h>
#endif

/** Further setup required for opening an LMDB environment
 */
static int ESECT
mdb_env_open2(MDB_env *env, int prev)
{
	unsigned int flags = env->me_flags;
	int i, newenv = 0, rc;
	MDB_meta meta;

#ifdef _WIN32
	/* See if we should use QueryLimited */
	rc = GetVersion();
	if ((rc & 0xff) > 5)
		env->me_pidquery = MDB_PROCESS_QUERY_LIMITED_INFORMATION;
	else
		env->me_pidquery = PROCESS_QUERY_INFORMATION;
	/* Grab functions we need from NTDLL */
	if (!NtCreateSection) {
		HMODULE h = GetModuleHandleW(L"NTDLL.DLL");
		if (!h)
			return MDB_PROBLEM;
		NtClose = (NtCloseFunc *)GetProcAddress(h, "NtClose");
		if (!NtClose)
			return MDB_PROBLEM;
		NtMapViewOfSection = (NtMapViewOfSectionFunc *)GetProcAddress(h, "NtMapViewOfSection");
		if (!NtMapViewOfSection)
			return MDB_PROBLEM;
		NtCreateSection = (NtCreateSectionFunc *)GetProcAddress(h, "NtCreateSection");
		if (!NtCreateSection)
			return MDB_PROBLEM;
	}
	env->ovs = 0;
#endif /* _WIN32 */

#ifdef BROKEN_FDATASYNC
	/* ext3/ext4 fdatasync is broken on some older Linux kernels.
	 * https://lkml.org/lkml/2012/9/3/83
	 * Kernels after 3.6-rc6 are known good.
	 * https://lkml.org/lkml/2012/9/10/556
	 * See if the DB is on ext3/ext4, then check for new enough kernel
	 * Kernels 2.6.32.60, 2.6.34.15, 3.2.30, and 3.5.4 are also known
	 * to be patched.
	 */
	if (!(flags & MDB_INMEMORY)) {
		struct statfs st;
		fstatfs(env->me_fd, &st);
		while (st.f_type == 0xEF53) {
			struct utsname uts;
			int i;
			uname(&uts);
			if (uts.release[0] < '3') {
				if (!strncmp(uts.release, "2.6.32.", 7)) {
					i = atoi(uts.release+7);
					if (i >= 60)
						break;	/* 2.6.32.60 and newer is OK */
				} else if (!strncmp(uts.release, "2.6.34.", 7)) {
					i = atoi(uts.release+7);
					if (i >= 15)
						break;	/* 2.6.34.15 and newer is OK */
				}
			} else if (uts.release[0] == '3') {
				i = atoi(uts.release+2);
				if (i > 5)
					break;	/* 3.6 and newer is OK */
				if (i == 5) {
					i = atoi(uts.release+4);
					if (i >= 4)
						break;	/* 3.5.4 and newer is OK */
				} else if (i == 2) {
					i = atoi(uts.release+4);
					if (i >= 30)
						break;	/* 3.2.30 and newer is OK */
				}
			} else {	/* 4.x and newer is OK */
				break;
			}
			env->me_flags |= MDB_FSYNCONLY;
			break;
		}
	}
#endif

	if (flags & MDB_INMEMORY) {
		DPUTS("new mdbenv");
		newenv = 1;
		env->me_psize = env->me_os_psize;
		if (env->me_psize > MAX_PAGESIZE)
			env->me_psize = MAX_PAGESIZE;
		memset(&meta, 0, sizeof(meta));
		mdb_env_init_meta0(env, &meta);
		meta.mm_mapsize = DEFAULT_MAPSIZE;
	} else {
		if ((i = mdb_env_read_header(env, prev, &meta)) != 0) {
			if (i != ENOENT)
				return i;
			DPUTS("new mdbenv");
			newenv = 1;
			env->me_psize = env->me_os_psize;
			if (env->me_psize > MAX_PAGESIZE)
				env->me_psize = MAX_PAGESIZE;
			memset(&meta, 0, sizeof(meta));
			mdb_env_init_meta0(env, &meta);
			meta.mm_mapsize = DEFAULT_MAPSIZE;
		} else {
			env->me_psize = meta.mm_psize;
		}
	}

	/* Was a mapsize configured? */
	if (!env->me_mapsize) {
		env->me_mapsize = meta.mm_mapsize;
	}
	{
		/* Make sure mapsize >= committed data size.  Even when using
		 * mm_mapsize, which could be broken in old files (ITS#7789).
		 */
		mdb_size_t minsize = (meta.mm_last_pg + 1) * meta.mm_psize;
		if (env->me_mapsize < minsize)
			env->me_mapsize = minsize;
	}
	meta.mm_mapsize = env->me_mapsize;

	if (newenv && !(flags & MDB_FIXEDMAP) && !(flags & MDB_INMEMORY)) {
		/* mdb_env_map() may grow the datafile.  Write the metapages
		 * first, so the file will be valid if initialization fails.
		 * Except with FIXEDMAP, since we do not yet know mm_address.
		 * We could fill in mm_address later, but then a different
		 * program might end up doing that - one with a memory layout
		 * and map address which does not suit the main program.
		 */
		rc = mdb_env_init_meta(env, &meta);
		if (rc)
			return rc;
		newenv = 0;
	}
#ifdef _WIN32
	/* For FIXEDMAP, make sure the file is non-empty before we attempt to map it */
	if (newenv && !(flags & MDB_INMEMORY)) {
		char dummy = 0;
		DWORD len;
		rc = WriteFile(env->me_fd, &dummy, 1, &len, NULL);
		if (!rc) {
			rc = ErrCode();
			return rc;
		}
	}
#endif

	rc = mdb_env_map(env, (flags & MDB_FIXEDMAP) ? meta.mm_address : NULL);
	if (rc)
		return rc;

	if (newenv) {
		if (flags & MDB_FIXEDMAP)
			meta.mm_address = env->me_map;
		i = mdb_env_init_meta(env, &meta);
		if (i != MDB_SUCCESS) {
			return i;
		}
	}

	env->me_maxfree_1pg = (env->me_psize - PAGEHDRSZ) / sizeof(pgno_t) - 1;
	env->me_nodemax = (((env->me_psize - PAGEHDRSZ) / MDB_MINKEYS) & -2)
		- sizeof(indx_t);
#if !(MDB_MAXKEYSIZE)
	env->me_maxkey = env->me_nodemax - (NODESIZE + sizeof(MDB_db));
#endif
	env->me_maxpg = env->me_mapsize / env->me_psize;

	if (prev && env->me_txns)
		env->me_txns->mti_txnid = meta.mm_txnid;

#if MDB_DEBUG
	{
		MDB_meta *meta = mdb_env_pick_meta(env);
		MDB_db *db = &meta->mm_dbs[MAIN_DBI];

		DPRINTF(("opened database version %u, pagesize %u",
			meta->mm_version, env->me_psize));
		DPRINTF(("using meta page %d",  (int) (meta->mm_txnid & 1)));
		DPRINTF(("depth: %u",           db->md_depth));
		DPRINTF(("entries: %"Yu,        db->md_entries));
		DPRINTF(("branch pages: %"Yu,   db->md_branch_pages));
		DPRINTF(("leaf pages: %"Yu,     db->md_leaf_pages));
		DPRINTF(("overflow pages: %"Yu, db->md_overflow_pages));
		DPRINTF(("root: %"Yu,           db->md_root));
	}
#endif

	return MDB_SUCCESS;
}


/** Release a reader thread's slot in the reader lock table.
 *	This function is called automatically when a thread exits.
 * @param[in] ptr This points to the slot in the reader lock table.
 */
static void
mdb_env_reader_dest(void *ptr)
{
	MDB_reader *reader = ptr;

#ifndef _WIN32
	if (reader->mr_pid == getpid()) /* catch pthread_exit() in child process */
#endif
		/* We omit the mutex, so do this atomically (i.e. skip mr_txnid) */
		reader->mr_pid = 0;
}

#ifdef _WIN32
/** Junk for arranging thread-specific callbacks on Windows. This is
 *	necessarily platform and compiler-specific. Windows supports up
 *	to 1088 keys. Let's assume nobody opens more than 64 environments
 *	in a single process, for now. They can override this if needed.
 */
#ifndef MAX_TLS_KEYS
#define MAX_TLS_KEYS	64
#endif
static pthread_key_t mdb_tls_keys[MAX_TLS_KEYS];
static int mdb_tls_nkeys;

static void NTAPI mdb_tls_callback(PVOID module, DWORD reason, PVOID ptr)
{
	int i;
	switch(reason) {
	case DLL_PROCESS_ATTACH: break;
	case DLL_THREAD_ATTACH: break;
	case DLL_THREAD_DETACH:
		for (i=0; i<mdb_tls_nkeys; i++) {
			MDB_reader *r = pthread_getspecific(mdb_tls_keys[i]);
			if (r) {
				mdb_env_reader_dest(r);
			}
		}
		break;
	case DLL_PROCESS_DETACH: break;
	}
}
#ifdef __GNUC__
#ifdef _WIN64
const PIMAGE_TLS_CALLBACK mdb_tls_cbp __attribute__((section (".CRT$XLB"))) = mdb_tls_callback;
#else
PIMAGE_TLS_CALLBACK mdb_tls_cbp __attribute__((section (".CRT$XLB"))) = mdb_tls_callback;
#endif
#else
#ifdef _WIN64
/* Force some symbol references.
 *	_tls_used forces the linker to create the TLS directory if not already done
 *	mdb_tls_cbp prevents whole-program-optimizer from dropping the symbol.
 */
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:mdb_tls_cbp")
#pragma const_seg(".CRT$XLB")
extern const PIMAGE_TLS_CALLBACK mdb_tls_cbp;
const PIMAGE_TLS_CALLBACK mdb_tls_cbp = mdb_tls_callback;
#pragma const_seg()
#else	/* _WIN32 */
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_mdb_tls_cbp")
#pragma data_seg(".CRT$XLB")
PIMAGE_TLS_CALLBACK mdb_tls_cbp = mdb_tls_callback;
#pragma data_seg()
#endif	/* WIN 32/64 */
#endif	/* !__GNUC__ */
#endif

/** Setup process-local reader/writer lock state for #MDB_INMEMORY. */
static int ESECT
mdb_env_setup_inmem_locks(MDB_env *env)
{
	int rc;
	MDB_OFF_T rsize;

	if (!(env->me_flags & MDB_NOTLS)) {
		rc = pthread_key_create(&env->me_txkey, mdb_env_reader_dest);
		if (rc)
			return rc;
		env->me_flags |= MDB_ENV_TXKEY;
#ifdef _WIN32
		/* Windows TLS callbacks need help finding their TLS info. */
		if (mdb_tls_nkeys >= MAX_TLS_KEYS)
			return MDB_TLS_FULL;
		mdb_tls_keys[mdb_tls_nkeys++] = env->me_txkey;
#endif
	}

#ifdef _WIN32
	rsize = (env->me_maxreaders-1) * sizeof(MDB_reader) + sizeof(MDB_txninfo);
	{
		DWORD rlo = (DWORD)(rsize & 0xffffffff);
		DWORD rhi = (DWORD)(rsize >> 16 >> 16);
		HANDLE mh = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
			rhi, rlo, NULL);
		if (!mh)
			return ErrCode();
		env->me_txns = MapViewOfFileEx(mh, FILE_MAP_WRITE, 0, 0, rsize, NULL);
		CloseHandle(mh);
		if (!env->me_txns)
			return ErrCode();
	}
#else
#ifdef MDB_MAP_ANON
	rsize = (env->me_maxreaders-1) * sizeof(MDB_reader) + sizeof(MDB_txninfo);
	env->me_txns = mmap(NULL, rsize, PROT_READ|PROT_WRITE,
		MAP_SHARED|MDB_MAP_ANON, -1, 0);
	if (env->me_txns == MAP_FAILED) {
		env->me_txns = NULL;
		return ErrCode();
	}
#else
	return ENOSYS;
#endif
#endif
	env->me_flags |= MDB_ENV_INMEM_LOCKS;
	memset(env->me_txns, 0, rsize);

#ifdef _WIN32
	env->me_rmutex = CreateMutex(NULL, FALSE, NULL);
	if (!env->me_rmutex)
		return ErrCode();
	env->me_wmutex = CreateMutex(NULL, FALSE, NULL);
	if (!env->me_wmutex)
		return ErrCode();
#elif defined(MDB_USE_POSIX_SEM)
	{
		static unsigned long mdb_inmem_semseq;
		unsigned long seq = ++mdb_inmem_semseq;
		char rname[64], wname[64];

		snprintf(rname, sizeof(rname), "/dlmdb-ir-%ld-%lu",
			(long)getpid(), seq);
		snprintf(wname, sizeof(wname), "/dlmdb-iw-%ld-%lu",
			(long)getpid(), seq);

		env->me_rmutex = sem_open(rname, O_CREAT|O_EXCL, 0600, 1);
		if (env->me_rmutex == SEM_FAILED)
			return ErrCode();
		(void)sem_unlink(rname);

		env->me_wmutex = sem_open(wname, O_CREAT|O_EXCL, 0600, 1);
		if (env->me_wmutex == SEM_FAILED)
			return ErrCode();
		(void)sem_unlink(wname);
	}
#elif defined(MDB_USE_SYSV_SEM)
	{
		env->me_rmutex->semid = -2;
		env->me_wmutex->semid = -2;
		env->me_rmutex->semnum = 0;
		env->me_wmutex->semnum = 1;
		env->me_rmutex->locked = NULL;
		env->me_wmutex->locked = NULL;
		rc = pthread_mutex_init(&env->me_rmutex->pmutex, NULL);
		if (rc)
			return rc;
		rc = pthread_mutex_init(&env->me_wmutex->pmutex, NULL);
		if (rc) {
			pthread_mutex_destroy(&env->me_rmutex->pmutex);
			return rc;
		}
		env->me_rmutex->semid = -1;
		env->me_wmutex->semid = -1;
		env->me_txns->mti_semid = -1;
		env->me_txns->mti_rlocked = 0;
		env->me_txns->mti_wlocked = 0;
	}
#else	/* MDB_USE_POSIX_MUTEX */
	{
		pthread_mutexattr_t mattr;

		memset(env->me_txns->mti_rmutex, 0, sizeof(*env->me_txns->mti_rmutex));
		memset(env->me_txns->mti_wmutex, 0, sizeof(*env->me_txns->mti_wmutex));
		rc = pthread_mutexattr_init(&mattr);
		if (rc)
			return rc;
		rc = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_PRIVATE);
#ifdef MDB_ROBUST_SUPPORTED
		if (!rc) rc = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
#endif
		if (!rc) rc = pthread_mutex_init(env->me_txns->mti_rmutex, &mattr);
		if (!rc) rc = pthread_mutex_init(env->me_txns->mti_wmutex, &mattr);
		pthread_mutexattr_destroy(&mattr);
		if (rc)
			return rc;
	}
#endif

	env->me_txns->mti_magic = MDB_MAGIC;
	env->me_txns->mti_format = MDB_LOCK_FORMAT;
	env->me_txns->mti_txnid = 0;
	env->me_txns->mti_numreaders = 0;
	env->me_live_reader = 1;
	env->me_close_readers = env->me_maxreaders;

	return MDB_SUCCESS;
}

/** Downgrade the exclusive lock on the region back to shared */
static int ESECT
mdb_env_share_locks(MDB_env *env, int *excl)
{
	int rc = 0;
	MDB_meta *meta = mdb_env_pick_meta(env);

	env->me_txns->mti_txnid = meta->mm_txnid;

#ifdef _WIN32
	{
		OVERLAPPED ov;
		/* First acquire a shared lock. The Unlock will
		 * then release the existing exclusive lock.
		 */
		memset(&ov, 0, sizeof(ov));
		if (!LockFileEx(env->me_lfd, 0, 0, 1, 0, &ov)) {
			rc = ErrCode();
		} else {
			UnlockFile(env->me_lfd, 0, 0, 1, 0);
			*excl = 0;
		}
	}
#else
	{
		struct flock lock_info;
		/* The shared lock replaces the existing lock */
		memset((void *)&lock_info, 0, sizeof(lock_info));
		lock_info.l_type = F_RDLCK;
		lock_info.l_whence = SEEK_SET;
		lock_info.l_start = 0;
		lock_info.l_len = 1;
		while ((rc = fcntl(env->me_lfd, F_SETLK, &lock_info)) &&
				(rc = ErrCode()) == EINTR) ;
		*excl = rc ? -1 : 0;	/* error may mean we lost the lock */
	}
#endif

	return rc;
}

/** Try to get exclusive lock, otherwise shared.
 *	Maintain *excl = -1: no/unknown lock, 0: shared, 1: exclusive.
 */
static int ESECT
mdb_env_excl_lock(MDB_env *env, int *excl)
{
	int rc = 0;
#ifdef _WIN32
	if (LockFile(env->me_lfd, 0, 0, 1, 0)) {
		*excl = 1;
	} else {
		OVERLAPPED ov;
		memset(&ov, 0, sizeof(ov));
		if (LockFileEx(env->me_lfd, 0, 0, 1, 0, &ov)) {
			*excl = 0;
		} else {
			rc = ErrCode();
		}
	}
#else
	struct flock lock_info;
	memset((void *)&lock_info, 0, sizeof(lock_info));
	lock_info.l_type = F_WRLCK;
	lock_info.l_whence = SEEK_SET;
	lock_info.l_start = 0;
	lock_info.l_len = 1;
	while ((rc = fcntl(env->me_lfd, F_SETLK, &lock_info)) &&
			(rc = ErrCode()) == EINTR) ;
	if (!rc) {
		*excl = 1;
	} else
# ifndef MDB_USE_POSIX_MUTEX
	if (*excl < 0) /* always true when MDB_USE_POSIX_MUTEX */
# endif
	{
		lock_info.l_type = F_RDLCK;
		while ((rc = fcntl(env->me_lfd, F_SETLKW, &lock_info)) &&
				(rc = ErrCode()) == EINTR) ;
		if (rc == 0)
			*excl = 0;
	}
#endif
	return rc;
}

#ifdef MDB_USE_HASH
/*
 * hash_64 - 64 bit Fowler/Noll/Vo-0 FNV-1a hash code
 *
 * @(#) $Revision: 5.1 $
 * @(#) $Id: hash_64a.c,v 5.1 2009/06/30 09:01:38 chongo Exp $
 * @(#) $Source: /usr/local/src/cmd/fnv/RCS/hash_64a.c,v $
 *
 *	  http://www.isthe.com/chongo/tech/comp/fnv/index.html
 *
 ***
 *
 * Please do not copyright this code.  This code is in the public domain.
 *
 * LANDON CURT NOLL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO
 * EVENT SHALL LANDON CURT NOLL BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * By:
 *	chongo <Landon Curt Noll> /\oo/\
 *	  http://www.isthe.com/chongo/
 *
 * Share and Enjoy!	:-)
 */

/** perform a 64 bit Fowler/Noll/Vo FNV-1a hash on a buffer
 * @param[in] val	value to hash
 * @param[in] len	length of value
 * @return 64 bit hash
 */
static mdb_hash_t
mdb_hash(const void *val, size_t len)
{
	const unsigned char *s = (const unsigned char *) val, *end = s + len;
	mdb_hash_t hval = 0xcbf29ce484222325ULL;
	/*
	 * FNV-1a hash each octet of the buffer
	 */
	while (s < end) {
		hval = (hval ^ *s++) * 0x100000001b3ULL;
	}
	/* return our new hash value */
	return hval;
}

/** Hash the string and output the encoded hash.
 * This uses modified RFC1924 Ascii85 encoding to accommodate systems with
 * very short name limits. We don't care about the encoding being reversible,
 * we just want to preserve as many bits of the input as possible in a
 * small printable string.
 * @param[in] str string to hash
 * @param[out] encbuf an array of 11 chars to hold the hash
 */
static const char mdb_a85[]= "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

static void ESECT
mdb_pack85(unsigned long long l, char *out)
{
	int i;

	for (i=0; i<10 && l; i++) {
		*out++ = mdb_a85[l % 85];
		l /= 85;
	}
	*out = '\0';
}

/** Init #MDB_env.me_mutexname[] except the char which #MUTEXNAME() will set.
 *	Changes to this code must be reflected in #MDB_LOCK_FORMAT.
 */
static void ESECT
mdb_env_mname_init(MDB_env *env)
{
	char *nm = env->me_mutexname;
	strcpy(nm, MUTEXNAME_PREFIX);
	mdb_pack85(env->me_txns->mti_mutexid, nm + sizeof(MUTEXNAME_PREFIX));
}

/** Return env->me_mutexname after filling in ch ('r'/'w') for convenience */
#define MUTEXNAME(env, ch) ( \
		(void) ((env)->me_mutexname[sizeof(MUTEXNAME_PREFIX)-1] = (ch)), \
		(env)->me_mutexname)

#endif

/** Open and/or initialize the lock region for the environment.
 * @param[in] env The LMDB environment.
 * @param[in] fname Filename + scratch area, from #mdb_fname_init().
 * @param[in] mode The Unix permissions for the file, if we create it.
 * @param[in,out] excl In -1, out lock type: -1 none, 0 shared, 1 exclusive
 * @return 0 on success, non-zero on failure.
 */
static int ESECT
mdb_env_setup_locks(MDB_env *env, MDB_name *fname, int mode, int *excl)
{
#ifdef _WIN32
#	define MDB_ERRCODE_ROFS	ERROR_WRITE_PROTECT
#else
#	define MDB_ERRCODE_ROFS	EROFS
#endif
#ifdef MDB_USE_SYSV_SEM
	int semid;
	union semun semu;
#endif
	int rc;
	MDB_OFF_T size, rsize;

	rc = mdb_fopen(env, fname, MDB_O_LOCKS, mode, &env->me_lfd);
	if (rc) {
		/* Omit lockfile if read-only env on read-only filesystem */
		if (rc == MDB_ERRCODE_ROFS && (env->me_flags & MDB_RDONLY)) {
			return MDB_SUCCESS;
		}
		goto fail;
	}

	if (!(env->me_flags & MDB_NOTLS)) {
		rc = pthread_key_create(&env->me_txkey, mdb_env_reader_dest);
		if (rc)
			goto fail;
		env->me_flags |= MDB_ENV_TXKEY;
#ifdef _WIN32
		/* Windows TLS callbacks need help finding their TLS info. */
		if (mdb_tls_nkeys >= MAX_TLS_KEYS) {
			rc = MDB_TLS_FULL;
			goto fail;
		}
		mdb_tls_keys[mdb_tls_nkeys++] = env->me_txkey;
#endif
	}

	/* Try to get exclusive lock. If we succeed, then
	 * nobody is using the lock region and we should initialize it.
	 */
	if ((rc = mdb_env_excl_lock(env, excl))) goto fail;

#ifdef _WIN32
	size = GetFileSize(env->me_lfd, NULL);
#else
	size = lseek(env->me_lfd, 0, SEEK_END);
	if (size == -1) goto fail_errno;
#endif
	rsize = (env->me_maxreaders-1) * sizeof(MDB_reader) + sizeof(MDB_txninfo);
	if (size < rsize && *excl > 0) {
#ifdef _WIN32
		if (SetFilePointer(env->me_lfd, rsize, NULL, FILE_BEGIN) != (DWORD)rsize
			|| !SetEndOfFile(env->me_lfd))
			goto fail_errno;
#else
		if (ftruncate(env->me_lfd, rsize) != 0) goto fail_errno;
#endif
	} else {
		rsize = size;
		size = rsize - sizeof(MDB_txninfo);
		env->me_maxreaders = size/sizeof(MDB_reader) + 1;
	}
	{
#ifdef _WIN32
		HANDLE mh;
		mh = CreateFileMapping(env->me_lfd, NULL, PAGE_READWRITE,
			0, 0, NULL);
		if (!mh) goto fail_errno;
		env->me_txns = MapViewOfFileEx(mh, FILE_MAP_WRITE, 0, 0, rsize, NULL);
		CloseHandle(mh);
		if (!env->me_txns) goto fail_errno;
#else
		void *m = mmap(NULL, rsize, PROT_READ|PROT_WRITE, MAP_SHARED,
			env->me_lfd, 0);
		if (m == MAP_FAILED) goto fail_errno;
		env->me_txns = m;
#endif
	}
	if (*excl > 0) {
#ifdef _WIN32
		BY_HANDLE_FILE_INFORMATION stbuf;
		struct {
			DWORD volume;
			DWORD nhigh;
			DWORD nlow;
		} idbuf;

		if (!mdb_sec_inited) {
			InitializeSecurityDescriptor(&mdb_null_sd,
				SECURITY_DESCRIPTOR_REVISION);
			SetSecurityDescriptorDacl(&mdb_null_sd, TRUE, 0, FALSE);
			mdb_all_sa.nLength = sizeof(SECURITY_ATTRIBUTES);
			mdb_all_sa.bInheritHandle = FALSE;
			mdb_all_sa.lpSecurityDescriptor = &mdb_null_sd;
			mdb_sec_inited = 1;
		}
		if (!GetFileInformationByHandle(env->me_lfd, &stbuf)) goto fail_errno;
		idbuf.volume = stbuf.dwVolumeSerialNumber;
		idbuf.nhigh  = stbuf.nFileIndexHigh;
		idbuf.nlow   = stbuf.nFileIndexLow;
		env->me_txns->mti_mutexid = mdb_hash(&idbuf, sizeof(idbuf));
		mdb_env_mname_init(env);
		env->me_rmutex = CreateMutexA(&mdb_all_sa, FALSE, MUTEXNAME(env, 'r'));
		if (!env->me_rmutex) goto fail_errno;
		env->me_wmutex = CreateMutexA(&mdb_all_sa, FALSE, MUTEXNAME(env, 'w'));
		if (!env->me_wmutex) goto fail_errno;
#elif defined(MDB_USE_POSIX_SEM)
		struct stat stbuf;
		struct {
			dev_t dev;
			ino_t ino;
		} idbuf;

#if defined(__NetBSD__)
#define	MDB_SHORT_SEMNAMES	1	/* limited to 14 chars */
#endif
		if (fstat(env->me_lfd, &stbuf)) goto fail_errno;
		memset(&idbuf, 0, sizeof(idbuf));
		idbuf.dev = stbuf.st_dev;
		idbuf.ino = stbuf.st_ino;
		env->me_txns->mti_mutexid = mdb_hash(&idbuf, sizeof(idbuf))
#ifdef MDB_SHORT_SEMNAMES
			/* Max 9 base85-digits.  We truncate here instead of in
			 * mdb_env_mname_init() to keep the latter portable.
			 */
			% ((mdb_hash_t)85*85*85*85*85*85*85*85*85)
#endif
			;
		mdb_env_mname_init(env);
		/* Clean up after a previous run, if needed:  Try to
		 * remove both semaphores before doing anything else.
		 */
		sem_unlink(MUTEXNAME(env, 'r'));
		sem_unlink(MUTEXNAME(env, 'w'));
		env->me_rmutex = sem_open(MUTEXNAME(env, 'r'), O_CREAT|O_EXCL, mode, 1);
		if (env->me_rmutex == SEM_FAILED) goto fail_errno;
		env->me_wmutex = sem_open(MUTEXNAME(env, 'w'), O_CREAT|O_EXCL, mode, 1);
		if (env->me_wmutex == SEM_FAILED) goto fail_errno;
#elif defined(MDB_USE_SYSV_SEM)
		unsigned short vals[2] = {1, 1};
		key_t key = ftok(fname->mn_val, 'M'); /* fname is lockfile path now */
		if (key == -1)
			goto fail_errno;
		semid = semget(key, 2, (mode & 0777) | IPC_CREAT);
		if (semid < 0)
			goto fail_errno;
		semu.array = vals;
		if (semctl(semid, 0, SETALL, semu) < 0)
			goto fail_errno;
		env->me_txns->mti_semid = semid;
		env->me_txns->mti_rlocked = 0;
		env->me_txns->mti_wlocked = 0;
#else	/* MDB_USE_POSIX_MUTEX: */
		pthread_mutexattr_t mattr;

		/* Solaris needs this before initing a robust mutex.  Otherwise
		 * it may skip the init and return EBUSY "seems someone already
		 * inited" or EINVAL "it was inited differently".
		 */
		memset(env->me_txns->mti_rmutex, 0, sizeof(*env->me_txns->mti_rmutex));
		memset(env->me_txns->mti_wmutex, 0, sizeof(*env->me_txns->mti_wmutex));

		if ((rc = pthread_mutexattr_init(&mattr)) != 0)
			goto fail;
		rc = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
#ifdef MDB_ROBUST_SUPPORTED
		if (!rc) rc = pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
#endif
		if (!rc) rc = pthread_mutex_init(env->me_txns->mti_rmutex, &mattr);
		if (!rc) rc = pthread_mutex_init(env->me_txns->mti_wmutex, &mattr);
		pthread_mutexattr_destroy(&mattr);
		if (rc)
			goto fail;
#endif	/* _WIN32 || ... */

		env->me_txns->mti_magic = MDB_MAGIC;
		env->me_txns->mti_format = MDB_LOCK_FORMAT;
		env->me_txns->mti_txnid = 0;
		env->me_txns->mti_numreaders = 0;

	} else {
#ifdef MDB_USE_SYSV_SEM
		struct semid_ds buf;
#endif
		if (env->me_txns->mti_magic != MDB_MAGIC) {
			DPUTS("lock region has invalid magic");
			rc = MDB_INVALID;
			goto fail;
		}
		if (env->me_txns->mti_format != MDB_LOCK_FORMAT) {
			DPRINTF(("lock region has format+version 0x%x, expected 0x%x",
				env->me_txns->mti_format, MDB_LOCK_FORMAT));
			rc = MDB_VERSION_MISMATCH;
			goto fail;
		}
		rc = ErrCode();
		if (rc && rc != EACCES && rc != EAGAIN) {
			goto fail;
		}
#ifdef _WIN32
		mdb_env_mname_init(env);
		env->me_rmutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEXNAME(env, 'r'));
		if (!env->me_rmutex) goto fail_errno;
		env->me_wmutex = OpenMutexA(SYNCHRONIZE, FALSE, MUTEXNAME(env, 'w'));
		if (!env->me_wmutex) goto fail_errno;
#elif defined(MDB_USE_POSIX_SEM)
		mdb_env_mname_init(env);
		env->me_rmutex = sem_open(MUTEXNAME(env, 'r'), 0);
		if (env->me_rmutex == SEM_FAILED) goto fail_errno;
		env->me_wmutex = sem_open(MUTEXNAME(env, 'w'), 0);
		if (env->me_wmutex == SEM_FAILED) goto fail_errno;
#elif defined(MDB_USE_SYSV_SEM)
		semid = env->me_txns->mti_semid;
		semu.buf = &buf;
		/* check for read access */
		if (semctl(semid, 0, IPC_STAT, semu) < 0)
			goto fail_errno;
		/* check for write access */
		if (semctl(semid, 0, IPC_SET, semu) < 0)
			goto fail_errno;
#endif
	}
#ifdef MDB_USE_SYSV_SEM
	env->me_rmutex->semid = semid;
	env->me_wmutex->semid = semid;
	env->me_rmutex->semnum = 0;
	env->me_wmutex->semnum = 1;
	env->me_rmutex->locked = &env->me_txns->mti_rlocked;
	env->me_wmutex->locked = &env->me_txns->mti_wlocked;
#endif

	return MDB_SUCCESS;

fail_errno:
	rc = ErrCode();
fail:
	return rc;
}

	/** Only a subset of the @ref mdb_env flags can be changed
	 *	at runtime. Changing other flags requires closing the
	 *	environment and re-opening it with the new flags.
	 */
#define	CHANGEABLE	(MDB_NOSYNC|MDB_NOMETASYNC|MDB_MAPASYNC|MDB_NOMEMINIT)
#define	CHANGELESS	(MDB_FIXEDMAP|MDB_NOSUBDIR|MDB_RDONLY| \
	MDB_WRITEMAP|MDB_NOTLS|MDB_NOLOCK|MDB_NORDAHEAD|MDB_PREVSNAPSHOT| \
	MDB_INMEMORY)

#if VALID_FLAGS & PERSISTENT_FLAGS & (CHANGEABLE|CHANGELESS)
# error "Persistent DB flags & env flags overlap, but both go in mm_flags"
#endif

int ESECT
mdb_env_open(MDB_env *env, const char *path, unsigned int flags, mdb_mode_t mode)
{
	int rc = MDB_SUCCESS, excl = -1;
	MDB_name fname = {0};
	const char *trace_path = path ? path : ":memory:";

	if (env->me_fd!=INVALID_HANDLE_VALUE || (flags & ~(CHANGEABLE|CHANGELESS)))
		return EINVAL;

#ifdef MDB_VL32
	if (flags & MDB_WRITEMAP) {
		/* silently ignore WRITEMAP in 32 bit mode */
		flags ^= MDB_WRITEMAP;
	}
	if (flags & MDB_FIXEDMAP) {
		/* cannot support FIXEDMAP */
		return EINVAL;
	}
#endif
	flags |= env->me_flags;

	if (flags & MDB_INMEMORY) {
#ifdef MDB_VL32
		return ENOSYS;
#endif
		trace_path = ":memory:";
		if (flags & (MDB_RDONLY|MDB_FIXEDMAP|MDB_PREVSNAPSHOT))
			return EINVAL;
		flags |= MDB_WRITEMAP|MDB_NOSYNC|MDB_NOMETASYNC;
	} else {
		if (!path)
			return EINVAL;
		rc = mdb_fname_init(path, flags, &fname);
		if (rc)
			return rc;
	}

#ifdef MDB_VL32
#ifdef _WIN32
	env->me_rpmutex = CreateMutex(NULL, FALSE, NULL);
	if (!env->me_rpmutex) {
		rc = ErrCode();
		goto leave;
	}
#else
	rc = pthread_mutex_init(&env->me_rpmutex, NULL);
	if (rc)
		goto leave;
#endif
#endif
	flags |= MDB_ENV_ACTIVE;	/* tell mdb_env_close0() to clean up */

	if (flags & MDB_RDONLY) {
		/* silently ignore WRITEMAP when we're only getting read access */
		flags &= ~MDB_WRITEMAP;
	} else {
		if (!((env->me_free_pgs = mdb_midl_alloc(MDB_IDL_UM_MAX)) &&
			  (env->me_dirty_list = calloc(MDB_IDL_UM_SIZE, sizeof(MDB_ID2)))))
			rc = ENOMEM;
	}

	env->me_flags = flags;
	if (rc)
		goto leave;

#ifdef MDB_VL32
	{
		env->me_rpages = malloc(MDB_ERPAGE_SIZE * sizeof(MDB_ID3));
		if (!env->me_rpages) {
			rc = ENOMEM;
			goto leave;
		}
		env->me_rpages[0].mid = 0;
		env->me_rpcheck = MDB_ERPAGE_SIZE/2;
	}
#endif

	env->me_path = strdup(trace_path);
	env->me_dbxs = calloc(env->me_maxdbs, sizeof(MDB_dbx));
	env->me_dbflags = calloc(env->me_maxdbs, sizeof(uint16_t));
	env->me_dbiseqs = calloc(env->me_maxdbs, sizeof(unsigned int));
	if (!(env->me_dbxs && env->me_path && env->me_dbflags && env->me_dbiseqs)) {
		rc = ENOMEM;
		goto leave;
	}
	env->me_dbxs[FREE_DBI].md_cmp = mdb_cmp_long; /* aligned MDB_INTEGERKEY */

	if (flags & MDB_INMEMORY) {
		if (!(flags & MDB_NOLOCK)) {
			rc = mdb_env_setup_inmem_locks(env);
			if (rc)
				goto leave;
		}
	} else {
		/* For RDONLY, get lockfile after we know datafile exists */
		if (!(flags & (MDB_RDONLY|MDB_NOLOCK))) {
			rc = mdb_env_setup_locks(env, &fname, mode, &excl);
			if (rc)
				goto leave;
			if ((flags & MDB_PREVSNAPSHOT) && !excl) {
				rc = EAGAIN;
				goto leave;
			}
		}

		rc = mdb_fopen(env, &fname,
			(flags & MDB_RDONLY) ? MDB_O_RDONLY : MDB_O_RDWR,
			mode, &env->me_fd);
		if (rc)
			goto leave;
#ifdef _WIN32
		rc = mdb_fopen(env, &fname, MDB_O_OVERLAPPED, mode, &env->me_ovfd);
		if (rc)
			goto leave;
#endif

		if ((flags & (MDB_RDONLY|MDB_NOLOCK)) == MDB_RDONLY) {
			rc = mdb_env_setup_locks(env, &fname, mode, &excl);
			if (rc)
				goto leave;
		}
	}

	if ((rc = mdb_env_open2(env, flags & MDB_PREVSNAPSHOT)) == MDB_SUCCESS) {
		/* Synchronous fd for meta writes. Needed even with
		 * MDB_NOSYNC/MDB_NOMETASYNC, in case these get reset.
		 */
		if (!(flags & (MDB_RDONLY|MDB_WRITEMAP))) {
			rc = mdb_fopen(env, &fname, MDB_O_META, mode, &env->me_mfd);
			if (rc)
				goto leave;
		}
		DPRINTF(("opened dbenv %p", (void *) env));
		if (excl > 0 && !(flags & MDB_PREVSNAPSHOT)) {
			rc = mdb_env_share_locks(env, &excl);
			if (rc)
				goto leave;
		}
		if (!(flags & MDB_RDONLY)) {
			MDB_txn *txn;
			int tsize = sizeof(MDB_txn), size = tsize + env->me_maxdbs *
				(sizeof(MDB_db)+sizeof(MDB_cursor *)+sizeof(unsigned int)+1);
			if ((env->me_pbuf = calloc(1, env->me_psize)) &&
				(txn = calloc(1, size)))
			{
				txn->mt_dbs = (MDB_db *)((char *)txn + tsize);
				txn->mt_cursors = (MDB_cursor **)(txn->mt_dbs + env->me_maxdbs);
				txn->mt_dbiseqs = (unsigned int *)(txn->mt_cursors + env->me_maxdbs);
				txn->mt_dbflags = (unsigned char *)(txn->mt_dbiseqs + env->me_maxdbs);
				txn->mt_env = env;
#ifdef MDB_VL32
				txn->mt_rpages = malloc(MDB_TRPAGE_SIZE * sizeof(MDB_ID3));
				if (!txn->mt_rpages) {
					free(txn);
					rc = ENOMEM;
					goto leave;
				}
				txn->mt_rpages[0].mid = 0;
				txn->mt_rpcheck = MDB_TRPAGE_SIZE/2;
#endif
				txn->mt_dbxs = env->me_dbxs;
				txn->mt_flags = MDB_TXN_FINISHED;
				env->me_txn0 = txn;
			} else {
				rc = ENOMEM;
			}
		}
	}

leave:
	MDB_TRACE(("%p, %s, %u, %04o", env, trace_path, flags & (CHANGEABLE|CHANGELESS), mode));
	if (rc) {
		mdb_env_close0(env, excl);
	}
	mdb_fname_destroy(fname);
	return rc;
}

/** Destroy resources from mdb_env_open(), clear our readers & DBIs */
static void ESECT
mdb_env_close0(MDB_env *env, int excl)
{
	int i;

	if (!(env->me_flags & MDB_ENV_ACTIVE))
		return;

	/* Doing this here since me_dbxs may not exist during mdb_env_close */
	if (env->me_dbxs) {
		for (i = env->me_maxdbs; --i >= CORE_DBS; )
			free(env->me_dbxs[i].md_name.mv_data);
		free(env->me_dbxs);
	}

	free(env->me_pbuf);
	free(env->me_dbiseqs);
	free(env->me_dbflags);
	free(env->me_path);
	free(env->me_dirty_list);
#ifdef MDB_VL32
	if (env->me_txn0 && env->me_txn0->mt_rpages)
		free(env->me_txn0->mt_rpages);
#endif
	if (env->me_txn0)
		mdb_prefix_scratch_clear(&env->me_txn0->mt_prefix);
#ifdef MDB_VL32
	if (env->me_rpages) {
		MDB_ID3L el = env->me_rpages;
		unsigned int x;
		for (x=1; x<=el[0].mid; x++)
			munmap(el[x].mptr, el[x].mcnt * env->me_psize);
		free(el);
	}
#endif
	free(env->me_txn0);
	mdb_midl_free(env->me_free_pgs);

	if (env->me_flags & MDB_ENV_TXKEY) {
		pthread_key_delete(env->me_txkey);
#ifdef _WIN32
		/* Delete our key from the global list */
		for (i=0; i<mdb_tls_nkeys; i++)
			if (mdb_tls_keys[i] == env->me_txkey) {
				mdb_tls_keys[i] = mdb_tls_keys[mdb_tls_nkeys-1];
				mdb_tls_nkeys--;
				break;
			}
#endif
	}

	if (env->me_map) {
#ifdef MDB_VL32
		munmap(env->me_map, NUM_METAS*env->me_psize);
#else
		munmap(env->me_map, env->me_mapsize);
#endif
	}
	if (env->me_mfd != INVALID_HANDLE_VALUE)
		(void) close(env->me_mfd);
#ifdef _WIN32
	if (env->ovs > 0) {
		for (i = 0; i < env->ovs; i++) {
			CloseHandle(env->ov[i].hEvent);
		}
		free(env->ov);
	}
	if (env->me_ovfd != INVALID_HANDLE_VALUE)
		(void) close(env->me_ovfd);
#endif
	if (env->me_fd != INVALID_HANDLE_VALUE)
		(void) close(env->me_fd);
	if (env->me_txns) {
		MDB_PID_T pid = getpid();
		/* Clearing readers is done in this function because
		 * me_txkey with its destructor must be disabled first.
		 *
		 * We skip the the reader mutex, so we touch only
		 * data owned by this process (me_close_readers and
		 * our readers), and clear each reader atomically.
		 */
		for (i = env->me_close_readers; --i >= 0; )
			if (env->me_txns->mti_readers[i].mr_pid == pid)
				env->me_txns->mti_readers[i].mr_pid = 0;
		if (env->me_flags & MDB_ENV_INMEM_LOCKS) {
#ifdef _WIN32
			if (env->me_rmutex) {
				CloseHandle(env->me_rmutex);
				if (env->me_wmutex) CloseHandle(env->me_wmutex);
			}
#elif defined(MDB_USE_POSIX_SEM)
			if (env->me_rmutex != SEM_FAILED)
				sem_close(env->me_rmutex);
			if (env->me_wmutex != SEM_FAILED)
				sem_close(env->me_wmutex);
#elif defined(MDB_USE_SYSV_SEM)
			if (env->me_rmutex->semid >= 0)
				semctl(env->me_rmutex->semid, 0, IPC_RMID);
			else if (env->me_rmutex->semid == -1) {
				pthread_mutex_destroy(&env->me_rmutex->pmutex);
				pthread_mutex_destroy(&env->me_wmutex->pmutex);
			}
			env->me_rmutex->semid = -1;
			env->me_wmutex->semid = -1;
#endif
		} else {
#ifdef _WIN32
			if (env->me_rmutex) {
				CloseHandle(env->me_rmutex);
				if (env->me_wmutex) CloseHandle(env->me_wmutex);
			}
			/* Windows automatically destroys the mutexes when
			 * the last handle closes.
			 */
#elif defined(MDB_USE_POSIX_SEM)
			if (env->me_rmutex != SEM_FAILED) {
				sem_close(env->me_rmutex);
				if (env->me_wmutex != SEM_FAILED)
					sem_close(env->me_wmutex);
				/* If we have the filelock:  If we are the
				 * only remaining user, clean up semaphores.
				 */
				if (excl == 0)
					mdb_env_excl_lock(env, &excl);
				if (excl > 0) {
					sem_unlink(MUTEXNAME(env, 'r'));
					sem_unlink(MUTEXNAME(env, 'w'));
				}
			}
#elif defined(MDB_USE_SYSV_SEM)
			if (env->me_rmutex->semid != -1) {
				/* If we have the filelock:  If we are the
				 * only remaining user, clean up semaphores.
				 */
				if (excl == 0)
					mdb_env_excl_lock(env, &excl);
				if (excl > 0)
					semctl(env->me_rmutex->semid, 0, IPC_RMID);
			}
#endif
		}
		munmap((void *)env->me_txns, (env->me_maxreaders-1)*sizeof(MDB_reader)+sizeof(MDB_txninfo));
	}
	if (env->me_lfd != INVALID_HANDLE_VALUE) {
#ifdef _WIN32
		if (excl >= 0) {
			/* Unlock the lockfile.  Windows would have unlocked it
			 * after closing anyway, but not necessarily at once.
			 */
			UnlockFile(env->me_lfd, 0, 0, 1, 0);
		}
#endif
		(void) close(env->me_lfd);
	}
#ifdef MDB_VL32
#ifdef _WIN32
	if (env->me_fmh) CloseHandle(env->me_fmh);
	if (env->me_rpmutex) CloseHandle(env->me_rpmutex);
#else
	pthread_mutex_destroy(&env->me_rpmutex);
#endif
#endif

	env->me_flags &= ~(MDB_ENV_ACTIVE|MDB_ENV_TXKEY|MDB_ENV_INMEM_LOCKS);
}

void ESECT
mdb_env_close(MDB_env *env)
{
	MDB_page *dp;

	if (env == NULL)
		return;

	MDB_TRACE(("%p", env));
	VGMEMP_DESTROY(env);
	while ((dp = env->me_dpages) != NULL) {
		VGMEMP_DEFINED(&dp->mp_next, sizeof(dp->mp_next));
		env->me_dpages = dp->mp_next;
		free(dp);
	}

	mdb_env_close0(env, 0);
	free(env);
}

/** Compare two items pointing at aligned #mdb_size_t's */
static int
mdb_cmp_long(const MDB_val *a, const MDB_val *b)
{
	return (*(mdb_size_t *)a->mv_data < *(mdb_size_t *)b->mv_data) ? -1 :
		*(mdb_size_t *)a->mv_data > *(mdb_size_t *)b->mv_data;
}

/** Compare two items pointing at aligned unsigned int's.
 *
 *	This is also set as #MDB_INTEGERDUP|#MDB_DUPFIXED's #MDB_dbx.%md_dcmp,
 *	but #mdb_cmp_clong() is called instead if the data type is #mdb_size_t.
 */
static int
mdb_cmp_int(const MDB_val *a, const MDB_val *b)
{
	return (*(unsigned int *)a->mv_data < *(unsigned int *)b->mv_data) ? -1 :
		*(unsigned int *)a->mv_data > *(unsigned int *)b->mv_data;
}

/** Compare two items pointing at unsigned ints of unknown alignment.
 *	Nodes and keys are guaranteed to be 2-byte aligned.
 */
static int
mdb_cmp_cint(const MDB_val *a, const MDB_val *b)
{
#if BYTE_ORDER == LITTLE_ENDIAN
	unsigned short *u, *c;
	int x;

	u = (unsigned short *) ((char *) a->mv_data + a->mv_size);
	c = (unsigned short *) ((char *) b->mv_data + a->mv_size);
	do {
		x = *--u - *--c;
	} while(!x && u > (unsigned short *)a->mv_data);
	return x;
#else
	unsigned short *u, *c, *end;
	int x;

	end = (unsigned short *) ((char *) a->mv_data + a->mv_size);
	u = (unsigned short *)a->mv_data;
	c = (unsigned short *)b->mv_data;
	do {
		x = *u++ - *c++;
	} while(!x && u < end);
	return x;
#endif
}

/** Compare two items lexically */
static int
mdb_cmp_memn(const MDB_val *a, const MDB_val *b)
{
	int diff;
	ssize_t len_diff;
	unsigned int len;

	len = a->mv_size;
	len_diff = (ssize_t) a->mv_size - (ssize_t) b->mv_size;
	if (len_diff > 0) {
		len = b->mv_size;
		len_diff = 1;
	}

	diff = memcmp(a->mv_data, b->mv_data, len);
	return diff ? diff : len_diff<0 ? -1 : len_diff;
}

/** Compare two items in reverse byte order */
static int
mdb_cmp_memnr(const MDB_val *a, const MDB_val *b)
{
	const unsigned char	*p1, *p2, *p1_lim;
	ssize_t len_diff;
	int diff;

	p1_lim = (const unsigned char *)a->mv_data;
	p1 = (const unsigned char *)a->mv_data + a->mv_size;
	p2 = (const unsigned char *)b->mv_data + b->mv_size;

	len_diff = (ssize_t) a->mv_size - (ssize_t) b->mv_size;
	if (len_diff > 0) {
		p1_lim += len_diff;
		len_diff = 1;
	}

	while (p1 > p1_lim) {
		diff = *--p1 - *--p2;
		if (diff)
			return diff;
	}
	return len_diff<0 ? -1 : len_diff;
}

/** Search for key within a page, using binary search.
 * Returns the smallest entry larger or equal to the key.
 * If exactp is non-null, stores whether the found entry was an exact match
 * in *exactp (1 or 0).
 * Updates the cursor index with the index of the found entry.
 * If no entry larger or equal to the key is found, returns NULL.
 */
static MDB_node *
mdb_node_search(MDB_cursor *mc, MDB_val *key, int *exactp)
{
	unsigned int	 i = 0, nkeys;
	int		 low, high;
	int		 rc = 0;
	int		 cmp_res = 0;
	MDB_page *mp = mc->mc_pg[mc->mc_top];
	MDB_node	*node = NULL;
	MDB_val	 nodekey;
	MDB_cmp_func *cmp;
	DKBUF;
	MDB_val trunk = {0, NULL};

	mc->mc_seq_cmp_pgno = P_INVALID;
	mc->mc_seq_cmp_keyptr = NULL;
	mc->mc_seq_cmp_keysize = 0;
	mc->mc_seq_cmp_prefix = 0;

	nkeys = NUMKEYS(mp);

	DPRINTF(("searching %u keys in %s %spage %"Yu,
	    nkeys, IS_LEAF(mp) ? "leaf" : "branch", IS_SUBP(mp) ? "sub-" : "",
	    mdb_dbg_pgno(mp)));

	low = IS_LEAF(mp) ? 0 : 1;
	high = nkeys - 1;
	cmp = mc->mc_dbx->md_cmp;
	int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;

	if (IS_LEAF(mp) && prefix_enabled && nkeys > 0) {
		MDB_node *trunk_node = NODEPTR(mp, 0);
		trunk.mv_size = trunk_node->mn_ksize;
		trunk.mv_data = NODEKEY(mp, trunk_node);
	}

	/* Branch pages have no data, so if using integer keys,
	 * alignment is guaranteed. Use faster mdb_cmp_int.
	 */
	if (cmp == mdb_cmp_cint && IS_BRANCH(mp)) {
		if (NODEPTR(mp, 1)->mn_ksize == sizeof(mdb_size_t))
			cmp = mdb_cmp_long;
		else
			cmp = mdb_cmp_int;
	}

	if (IS_LEAF2(mp)) {
		nodekey.mv_size = mc->mc_db->md_pad;
		node = NODEPTR(mp, 0);	/* fake */
		while (low <= high) {
			i = (low + high) >> 1;
			nodekey.mv_data = LEAF2KEY(mp, i, nodekey.mv_size);
			cmp_res = cmp(key, &nodekey);
			DPRINTF(("found leaf index %u [%s], rc = %i",
			    i, DKEY(&nodekey), cmp_res));
			if (cmp_res == 0)
				break;
			if (cmp_res > 0)
				low = i + 1;
			else
				high = i - 1;
		}
	} else {
		while (low <= high) {
			i = (low + high) >> 1;

			node = NODEPTR(mp, i);
			int used_encoded = 0;
			size_t prefix = 0;
			int need_prefix = IS_LEAF(mp) && prefix_enabled &&
				key && key->mv_data && cmp == mdb_cmp_memn;
			if (need_prefix)
				prefix = mdb_cursor_seq_cmp_refresh(mc, mp, key);

			if (need_prefix && i > 0 && trunk.mv_data) {
				int enc_rc = mdb_leaf_cmp_memn_encoded(mp, node,
					&trunk, key, prefix, &cmp_res);
				if (enc_rc == MDB_SUCCESS) {
					used_encoded = 1;
				} else {
					if (enc_rc != MDB_BAD_VALSIZE)
						goto bad;
				}
			}

			if (!used_encoded) {
				if (IS_LEAF(mp) && prefix_enabled) {
					rc = mdb_cursor_read_key_at(mc, mp, i, &nodekey);
					if (rc != MDB_SUCCESS)
						goto bad;
				} else {
					nodekey.mv_size = NODEKSZ(node);
					nodekey.mv_data = NODEKEY(mp, node);
				}

				if (need_prefix) {
					size_t shared = mc->mc_seq_shared;
					size_t skip = shared < prefix ? shared : prefix;
					if (skip)
						cmp_res = mdb_cmp_memn_with_skip(key, &nodekey, skip);
					else
						cmp_res = cmp(key, &nodekey);
				} else {
					cmp_res = cmp(key, &nodekey);
				}
			}
#if MDB_DEBUG
			if (used_encoded) {
				rc = mdb_cursor_read_key_at(mc, mp, i, &nodekey);
				if (rc != MDB_SUCCESS)
					goto bad;
			}
#endif
#if MDB_DEBUG
			if (used_encoded) {
				rc = mdb_cursor_read_key_at(mc, mp, i, &nodekey);
				if (rc != MDB_SUCCESS)
					goto bad;
			}
#endif
#if MDB_DEBUG
			if (IS_LEAF(mp))
				DPRINTF(("found leaf index %u [%s], rc = %i",
				    i, DKEY(&nodekey), cmp_res));
			else
				DPRINTF(("found branch index %u [%s -> %"Yu"], rc = %i",
				    i, DKEY(&nodekey), NODEPGNO(node), cmp_res));
#endif
			if (cmp_res == 0)
				break;
			if (cmp_res > 0)
				low = i + 1;
			else
				high = i - 1;
		}
	}

	if (cmp_res > 0) {	/* Found entry is less than the key. */
		i++;	/* Skip to get the smallest entry larger than key. */
		if (!IS_LEAF2(mp))
			node = NODEPTR(mp, i);
	}
	if (exactp)
		*exactp = (cmp_res == 0 && nkeys > 0);
	/* store the key index */
	mc->mc_ki[mc->mc_top] = i;
	if (i >= nkeys)
		/* There is no entry larger or equal to the key. */
		return NULL;

	/* nodeptr is fake for LEAF2 */
	return node;

bad:
	mdb_txn_mark_error(mc->mc_txn, MDB_CORRUPTED);
	return NULL;
}

#if 0
static void
mdb_cursor_adjust(MDB_cursor *mc, func)
{
	MDB_cursor *m2;

	for (m2 = mc->mc_txn->mt_cursors[mc->mc_dbi]; m2; m2=m2->mc_next) {
		if (m2->mc_pg[m2->mc_top] == mc->mc_pg[mc->mc_top]) {
			func(mc, m2);
		}
	}
}
#endif

/** Pop a page off the top of the cursor's stack. */
static void
mdb_cursor_pop(MDB_cursor *mc)
{
	if (mc->mc_snum) {
		DPRINTF(("popping page %"Yu" off db %d cursor %p",
			mc->mc_pg[mc->mc_top]->mp_pgno, DDBI(mc), (void *) mc));

		mc->mc_snum--;
		if (mc->mc_snum) {
			mc->mc_top--;
		} else {
			mc->mc_flags &= ~C_INITIALIZED;
		}
	}
}

/** Push a page onto the top of the cursor's stack.
 * Set #MDB_TXN_ERROR on failure.
 */
static int
mdb_cursor_push(MDB_cursor *mc, MDB_page *mp)
{
	DPRINTF(("pushing page %"Yu" on db %d cursor %p", mp->mp_pgno,
		DDBI(mc), (void *) mc));

	if (mc->mc_snum >= CURSOR_STACK) {
		mdb_txn_mark_error(mc->mc_txn, MDB_CURSOR_FULL);
		return MDB_CURSOR_FULL;
	}

	mc->mc_top = mc->mc_snum++;
	mc->mc_pg[mc->mc_top] = mp;
	mc->mc_ki[mc->mc_top] = 0;

	return MDB_SUCCESS;
}

#ifdef MDB_VL32
/** Map a read-only page.
 * There are two levels of tracking in use, a per-txn list and a per-env list.
 * ref'ing and unref'ing the per-txn list is faster since it requires no
 * locking. Pages are cached in the per-env list for global reuse, and a lock
 * is required. Pages are not immediately unmapped when their refcnt goes to
 * zero; they hang around in case they will be reused again soon.
 *
 * When the per-txn list gets full, all pages with refcnt=0 are purged from the
 * list and their refcnts in the per-env list are decremented.
 *
 * When the per-env list gets full, all pages with refcnt=0 are purged from the
 * list and their pages are unmapped.
 *
 * @note "full" means the list has reached its respective rpcheck threshold.
 * This threshold slowly raises if no pages could be purged on a given check,
 * and returns to its original value when enough pages were purged.
 *
 * If purging doesn't free any slots, filling the per-txn list will return
 * MDB_TXN_FULL, and filling the per-env list returns MDB_MAP_FULL.
 *
 * Reference tracking in a txn is imperfect, pages can linger with non-zero
 * refcnt even without active references. It was deemed to be too invasive
 * to add unrefs in every required location. However, all pages are unref'd
 * at the end of the transaction. This guarantees that no stale references
 * linger in the per-env list.
 *
 * Usually we map chunks of 16 pages at a time, but if an overflow page begins
 * at the tail of the chunk we extend the chunk to include the entire overflow
 * page. Unfortunately, pages can be turned into overflow pages after their
 * chunk was already mapped. In that case we must remap the chunk if the
 * overflow page is referenced. If the chunk's refcnt is 0 we can just remap
 * it, otherwise we temporarily map a new chunk just for the overflow page.
 *
 * @note this chunk handling means we cannot guarantee that a data item
 * returned from the DB will stay alive for the duration of the transaction:
 *   We unref pages as soon as a cursor moves away from the page
 *   A subsequent op may cause a purge, which may unmap any unref'd chunks
 * The caller must copy the data if it must be used later in the same txn.
 *
 * Also - our reference counting revolves around cursors, but overflow pages
 * aren't pointed to by a cursor's page stack. We have to remember them
 * explicitly, in the added mc_ovpg field. A single cursor can only hold a
 * reference to one overflow page at a time.
 *
 * @param[in] txn the transaction for this access.
 * @param[in] pgno the page number for the page to retrieve.
 * @param[out] ret address of a pointer where the page's address will be stored.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_rpage_get(MDB_txn *txn, pgno_t pg0, MDB_page **ret)
{
	MDB_env *env = txn->mt_env;
	MDB_page *p;
	MDB_ID3L tl = txn->mt_rpages;
	MDB_ID3L el = env->me_rpages;
	MDB_ID3 id3;
	unsigned x, rem;
	pgno_t pgno;
	int rc, retries = 1;
#ifdef _WIN32
	LARGE_INTEGER off;
	SIZE_T len;
#define SET_OFF(off,val)	off.QuadPart = val
#define MAP(rc,env,addr,len,off)	\
	addr = NULL; \
	rc = NtMapViewOfSection(env->me_fmh, GetCurrentProcess(), &addr, 0, \
		len, &off, &len, ViewUnmap, (env->me_flags & MDB_RDONLY) ? 0 : MEM_RESERVE, PAGE_READONLY); \
	if (rc) rc = mdb_nt2win32(rc)
#else
	off_t off;
	size_t len;
#define SET_OFF(off,val)	off = val
#define MAP(rc,env,addr,len,off)	\
	addr = mmap(NULL, len, PROT_READ, MAP_SHARED, env->me_fd, off); \
	rc = (addr == MAP_FAILED) ? errno : 0
#endif

	/* remember the offset of the actual page number, so we can
	 * return the correct pointer at the end.
	 */
	rem = pg0 & (MDB_RPAGE_CHUNK-1);
	pgno = pg0 ^ rem;

	id3.mid = 0;
	x = mdb_mid3l_search(tl, pgno);
	if (x <= tl[0].mid && tl[x].mid == pgno) {
		if (x != tl[0].mid && tl[x+1].mid == pg0)
			x++;
		/* check for overflow size */
		p = (MDB_page *)((char *)tl[x].mptr + rem * env->me_psize);
		if (IS_OVERFLOW(p) && p->mp_pages + rem > tl[x].mcnt) {
			id3.mcnt = p->mp_pages + rem;
			len = id3.mcnt * env->me_psize;
			SET_OFF(off, pgno * env->me_psize);
			MAP(rc, env, id3.mptr, len, off);
			if (rc)
				return rc;
			/* check for local-only page */
			if (rem) {
				mdb_tassert(txn, tl[x].mid != pg0);
				/* hope there's room to insert this locally.
				 * setting mid here tells later code to just insert
				 * this id3 instead of searching for a match.
				 */
				id3.mid = pg0;
				goto notlocal;
			} else {
				/* ignore the mapping we got from env, use new one */
				tl[x].mptr = id3.mptr;
				tl[x].mcnt = id3.mcnt;
				/* if no active ref, see if we can replace in env */
				if (!tl[x].mref) {
					unsigned i;
					pthread_mutex_lock(&env->me_rpmutex);
					i = mdb_mid3l_search(el, tl[x].mid);
					if (el[i].mref == 1) {
						/* just us, replace it */
						munmap(el[i].mptr, el[i].mcnt * env->me_psize);
						el[i].mptr = tl[x].mptr;
						el[i].mcnt = tl[x].mcnt;
					} else {
						/* there are others, remove ourself */
						el[i].mref--;
					}
					pthread_mutex_unlock(&env->me_rpmutex);
				}
			}
		}
		id3.mptr = tl[x].mptr;
		id3.mcnt = tl[x].mcnt;
		tl[x].mref++;
		goto ok;
	}

notlocal:
	if (tl[0].mid >= MDB_TRPAGE_MAX - txn->mt_rpcheck) {
		unsigned i, y;
		/* purge unref'd pages from our list and unref in env */
		pthread_mutex_lock(&env->me_rpmutex);
retry:
		y = 0;
		for (i=1; i<=tl[0].mid; i++) {
			if (!tl[i].mref) {
				if (!y) y = i;
				/* tmp overflow pages don't go to env */
				if (tl[i].mid & (MDB_RPAGE_CHUNK-1)) {
					munmap(tl[i].mptr, tl[i].mcnt * env->me_psize);
					continue;
				}
				x = mdb_mid3l_search(el, tl[i].mid);
				el[x].mref--;
			}
		}
		pthread_mutex_unlock(&env->me_rpmutex);
		if (!y) {
			/* we didn't find any unref'd chunks.
			 * if we're out of room, fail.
			 */
			if (tl[0].mid >= MDB_TRPAGE_MAX)
				return MDB_TXN_FULL;
			/* otherwise, raise threshold for next time around
			 * and let this go.
			 */
			txn->mt_rpcheck /= 2;
		} else {
			/* we found some unused; consolidate the list */
			for (i=y+1; i<= tl[0].mid; i++)
				if (tl[i].mref)
					tl[y++] = tl[i];
			tl[0].mid = y-1;
			/* decrease the check threshold toward its original value */
			if (!txn->mt_rpcheck)
				txn->mt_rpcheck = 1;
			while (txn->mt_rpcheck < tl[0].mid && txn->mt_rpcheck < MDB_TRPAGE_SIZE/2)
				txn->mt_rpcheck *= 2;
		}
	}
	if (tl[0].mid < MDB_TRPAGE_SIZE) {
		id3.mref = 1;
		if (id3.mid)
			goto found;
		/* don't map past last written page in read-only envs */
		if ((env->me_flags & MDB_RDONLY) && pgno + MDB_RPAGE_CHUNK-1 > txn->mt_last_pgno)
			id3.mcnt = txn->mt_last_pgno + 1 - pgno;
		else
			id3.mcnt = MDB_RPAGE_CHUNK;
		len = id3.mcnt * env->me_psize;
		id3.mid = pgno;

		/* search for page in env */
		pthread_mutex_lock(&env->me_rpmutex);
		x = mdb_mid3l_search(el, pgno);
		if (x <= el[0].mid && el[x].mid == pgno) {
			id3.mptr = el[x].mptr;
			id3.mcnt = el[x].mcnt;
			/* check for overflow size */
			p = (MDB_page *)((char *)id3.mptr + rem * env->me_psize);
			if (IS_OVERFLOW(p) && p->mp_pages + rem > id3.mcnt) {
				id3.mcnt = p->mp_pages + rem;
				len = id3.mcnt * env->me_psize;
				SET_OFF(off, pgno * env->me_psize);
				MAP(rc, env, id3.mptr, len, off);
				if (rc)
					goto fail;
				if (!el[x].mref) {
					munmap(el[x].mptr, env->me_psize * el[x].mcnt);
					el[x].mptr = id3.mptr;
					el[x].mcnt = id3.mcnt;
				} else {
					id3.mid = pg0;
					pthread_mutex_unlock(&env->me_rpmutex);
					goto found;
				}
			}
			el[x].mref++;
			pthread_mutex_unlock(&env->me_rpmutex);
			goto found;
		}
		if (el[0].mid >= MDB_ERPAGE_MAX - env->me_rpcheck) {
			/* purge unref'd pages */
			unsigned i, y = 0;
			for (i=1; i<=el[0].mid; i++) {
				if (!el[i].mref) {
					if (!y) y = i;
					munmap(el[i].mptr, env->me_psize * el[i].mcnt);
				}
			}
			if (!y) {
				if (retries) {
					/* see if we can unref some local pages */
					retries--;
					id3.mid = 0;
					goto retry;
				}
				if (el[0].mid >= MDB_ERPAGE_MAX) {
					pthread_mutex_unlock(&env->me_rpmutex);
					return MDB_MAP_FULL;
				}
				env->me_rpcheck /= 2;
			} else {
				for (i=y+1; i<= el[0].mid; i++)
					if (el[i].mref)
						el[y++] = el[i];
				el[0].mid = y-1;
				if (!env->me_rpcheck)
					env->me_rpcheck = 1;
				while (env->me_rpcheck < el[0].mid && env->me_rpcheck < MDB_ERPAGE_SIZE/2)
					env->me_rpcheck *= 2;
			}
		}
		SET_OFF(off, pgno * env->me_psize);
		MAP(rc, env, id3.mptr, len, off);
		if (rc) {
fail:
			pthread_mutex_unlock(&env->me_rpmutex);
			return rc;
		}
		/* check for overflow size */
		p = (MDB_page *)((char *)id3.mptr + rem * env->me_psize);
		if (IS_OVERFLOW(p) && p->mp_pages + rem > id3.mcnt) {
			id3.mcnt = p->mp_pages + rem;
			munmap(id3.mptr, len);
			len = id3.mcnt * env->me_psize;
			MAP(rc, env, id3.mptr, len, off);
			if (rc)
				goto fail;
		}
		mdb_mid3l_insert(el, &id3);
		pthread_mutex_unlock(&env->me_rpmutex);
found:
		mdb_mid3l_insert(tl, &id3);
	} else {
		return MDB_TXN_FULL;
	}
ok:
	p = (MDB_page *)((char *)id3.mptr + rem * env->me_psize);
#if MDB_DEBUG	/* we don't need this check any more */
	if (IS_OVERFLOW(p)) {
		mdb_tassert(txn, p->mp_pages + rem <= id3.mcnt);
	}
#endif
	*ret = p;
	return MDB_SUCCESS;
}
#endif

/** Find the address of the page corresponding to a given page number.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc the cursor accessing the page.
 * @param[in] pgno the page number for the page to retrieve.
 * @param[out] ret address of a pointer where the page's address will be stored.
 * @param[out] lvl dirty_list inheritance level of found page. 1=current txn, 0=mapped page.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_get(MDB_cursor *mc, pgno_t pgno, MDB_page **ret, int *lvl)
{
	MDB_txn *txn = mc->mc_txn;
	MDB_page *p = NULL;
	int level;

	if (! (mc->mc_flags & (C_ORIG_RDONLY|C_WRITEMAP))) {
		MDB_txn *tx2 = txn;
		level = 1;
		do {
			MDB_ID2L dl = tx2->mt_u.dirty_list;
			unsigned x;
			/* Spilled pages were dirtied in this txn and flushed
			 * because the dirty list got full. Bring this page
			 * back in from the map (but don't unspill it here,
			 * leave that unless page_touch happens again).
			 */
			if (tx2->mt_spill_pgs) {
				MDB_ID pn = pgno << 1;
				x = mdb_midl_search(tx2->mt_spill_pgs, pn);
				if (x <= tx2->mt_spill_pgs[0] && tx2->mt_spill_pgs[x] == pn) {
					goto mapped;
				}
			}
			if (dl[0].mid) {
				unsigned x = mdb_mid2l_search(dl, pgno);
				if (x <= dl[0].mid && dl[x].mid == pgno) {
					p = dl[x].mptr;
					goto done;
				}
			}
			level++;
		} while ((tx2 = tx2->mt_parent) != NULL);
	}

	if (pgno >= txn->mt_next_pgno) {
		DPRINTF(("page %"Yu" not found", pgno));
		mdb_txn_mark_error(txn, MDB_PAGE_NOTFOUND);
		return MDB_PAGE_NOTFOUND;
	}

	level = 0;

mapped:
	{
#ifdef MDB_VL32
		int rc = mdb_rpage_get(txn, pgno, &p);
		if (rc) {
			mdb_txn_mark_error(txn, rc);
			return rc;
		}
#else
		MDB_env *env = txn->mt_env;
		p = (MDB_page *)(env->me_map + env->me_psize * pgno);
#endif
	}

done:
	*ret = p;
	if (lvl)
		*lvl = level;
	return MDB_SUCCESS;
}

/** Finish #mdb_page_search() / #mdb_page_search_lowest().
 *	The cursor is at the root page, set up the rest of it.
 */
static int
mdb_page_search_root(MDB_cursor *mc, MDB_val *key, int flags)
{
	MDB_page	*mp;
	int rc;
	DKBUF;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	rc = mdb_txn_check_interrupt(mc->mc_txn);
	if (rc)
		return rc;

	mp = mc->mc_pg[mc->mc_top];
	while (IS_BRANCH(mp)) {
		MDB_node	*node;
		indx_t		i;

		DPRINTF(("branch page %"Yu" has %u keys", mp->mp_pgno, NUMKEYS(mp)));
		/* Don't assert on branch pages in the FreeDB. We can get here
		 * while in the process of rebalancing a FreeDB branch page; we must
		 * let that proceed. ITS#8336
		 */
		mdb_cassert(mc, !mc->mc_dbi || NUMKEYS(mp) > 1);
		DPRINTF(("found index 0 to page %"Yu, NODEPGNO(NODEPTR(mp, 0))));

		if (flags & (MDB_PS_FIRST|MDB_PS_LAST)) {
			i = 0;
			if (flags & MDB_PS_LAST) {
				i = NUMKEYS(mp) - 1;
				/* if already init'd, see if we're already in right place */
				if (mc->mc_flags & C_INITIALIZED) {
					if (mc->mc_ki[mc->mc_top] == i) {
						mc->mc_top = mc->mc_snum++;
						mp = mc->mc_pg[mc->mc_top];
						goto ready;
					}
				}
			}
		} else {
			int	 exact;
			node = mdb_node_search(mc, key, &exact);
			if (node == NULL)
				i = NUMKEYS(mp) - 1;
			else {
				i = mc->mc_ki[mc->mc_top];
				if (!exact) {
					mdb_cassert(mc, i > 0);
					i--;
				}
			}
			DPRINTF(("following index %u for key [%s]", i, DKEY(key)));
		}

		mdb_cassert(mc, i < NUMKEYS(mp));
		node = NODEPTR(mp, i);

	if ((rc = mdb_page_get(mc, NODEPGNO(node), &mp, NULL)) != 0)
		return rc;

		mc->mc_ki[mc->mc_top] = i;
		if ((rc = mdb_cursor_push(mc, mp)))
			return rc;

ready:
		if (flags & MDB_PS_MODIFY) {
			if ((rc = mdb_page_touch(mc)) != 0)
				return rc;
			mp = mc->mc_pg[mc->mc_top];
		}
	}

	if (!IS_LEAF(mp)) {
		mdb_txn_mark_error(mc->mc_txn, MDB_CORRUPTED);
		return MDB_CORRUPTED;
	}

	DPRINTF(("found leaf page %"Yu" for key [%s]", mp->mp_pgno,
	    key ? DKEY(key) : "null"));
	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~C_EOF;

	return MDB_SUCCESS;
}

/** Search for the lowest key under the current branch page.
 * This just bypasses a NUMKEYS check in the current page
 * before calling mdb_page_search_root(), because the callers
 * are all in situations where the current page is known to
 * be underfilled.
 */
static int
mdb_page_search_lowest(MDB_cursor *mc)
{
	MDB_page	*mp;
	MDB_node	*node;
	int rc;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	rc = mdb_txn_check_interrupt(mc->mc_txn);
	if (rc)
		return rc;

	mp = mc->mc_pg[mc->mc_top];
	node = NODEPTR(mp, 0);
	if ((rc = mdb_page_get(mc, NODEPGNO(node), &mp, NULL)) != 0)
		return rc;

	mc->mc_ki[mc->mc_top] = 0;
	if ((rc = mdb_cursor_push(mc, mp)))
		return rc;
	return mdb_page_search_root(mc, NULL, MDB_PS_FIRST);
}

/** Search for the page a given key should be in.
 * Push it and its parent pages on the cursor stack.
 * @param[in,out] mc the cursor for this operation.
 * @param[in] key the key to search for, or NULL for first/last page.
 * @param[in] flags If MDB_PS_MODIFY is set, visited pages in the DB
 *   are touched (updated with new page numbers).
 *   If MDB_PS_FIRST or MDB_PS_LAST is set, find first or last leaf.
 *   This is used by #mdb_cursor_first() and #mdb_cursor_last().
 *   If MDB_PS_ROOTONLY set, just fetch root node, no further lookups.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_search(MDB_cursor *mc, MDB_val *key, int flags)
{
	int		 rc;
	pgno_t		 root;

	/* Make sure the txn is still viable, then find the root from
	 * the txn's db table and set it as the root of the cursor's stack.
	 */
	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED) {
		DPUTS("transaction may not be used now");
		return MDB_BAD_TXN;
	}

	rc = mdb_txn_check_interrupt(mc->mc_txn);
	if (rc)
		return rc;

	/* Make sure we're using an up-to-date root */
	if (*mc->mc_dbflag & DB_STALE) {
				MDB_cursor mc2 = (MDB_cursor){0};
			if (TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi))
				return MDB_BAD_DBI;
			mdb_cursor_init(&mc2, mc->mc_txn, MAIN_DBI, NULL);
			rc = mdb_page_search(&mc2, &mc->mc_dbx->md_name, 0);
			if (rc)
				return rc;
			{
				MDB_val data;
				int exact = 0;
				uint16_t flags;
				MDB_node *leaf = mdb_node_search(&mc2,
					&mc->mc_dbx->md_name, &exact);
				if (!exact)
					return MDB_BAD_DBI;
				if ((leaf->mn_flags & (F_DUPDATA|F_SUBDATA)) != F_SUBDATA)
					return MDB_INCOMPATIBLE; /* not a named DB */
				rc = mdb_node_read(&mc2, leaf, &data);
				if (rc)
					return rc;
				memcpy(&flags, ((char *) data.mv_data + offsetof(MDB_db, md_flags)),
					sizeof(uint16_t));
				/* The txn may not know this DBI, or another process may
				 * have dropped and recreated the DB with other flags.
				 */
				if ((mc->mc_db->md_flags & PERSISTENT_FLAGS) != flags)
					return MDB_INCOMPATIBLE;
				memcpy(mc->mc_db, data.mv_data, sizeof(MDB_db));
			}
			*mc->mc_dbflag &= ~DB_STALE;
	}
	root = mc->mc_db->md_root;

	if (root == P_INVALID) {		/* Tree is empty. */
		DPUTS("tree is empty");
		return MDB_NOTFOUND;
	}

	mdb_cassert(mc, root > 1);
	if (!mc->mc_pg[0] || mc->mc_pg[0]->mp_pgno != root) {
#ifdef MDB_VL32
		if (mc->mc_pg[0])
			MDB_PAGE_UNREF(mc->mc_txn, mc->mc_pg[0]);
#endif
		if ((rc = mdb_page_get(mc, root, &mc->mc_pg[0], NULL)) != 0)
			return rc;
	}

#ifdef MDB_VL32
	{
		int i;
		for (i=1; i<mc->mc_snum; i++)
			MDB_PAGE_UNREF(mc->mc_txn, mc->mc_pg[i]);
	}
#endif
	mc->mc_snum = 1;
	mc->mc_top = 0;

	DPRINTF(("db %d root page %"Yu" has flags 0x%X",
		DDBI(mc), root, mc->mc_pg[0]->mp_flags));

	if (flags & MDB_PS_MODIFY) {
		if ((rc = mdb_page_touch(mc)))
			return rc;
	}

	if (flags & MDB_PS_ROOTONLY)
		return MDB_SUCCESS;

	return mdb_page_search_root(mc, key, flags);
}

static int
mdb_ovpage_free(MDB_cursor *mc, MDB_page *mp)
{
	MDB_txn *txn = mc->mc_txn;
	pgno_t pg = mp->mp_pgno;
	unsigned x = 0, ovpages = mp->mp_pages;
	MDB_env *env = txn->mt_env;
	MDB_IDL sl = txn->mt_spill_pgs;
	MDB_ID pn = pg << 1;
	int rc;

	DPRINTF(("free ov page %"Yu" (%d)", pg, ovpages));
	/* If the page is dirty or on the spill list we just acquired it,
	 * so we should give it back to our current free list, if any.
	 * Otherwise put it onto the list of pages we freed in this txn.
	 *
	 * Won't create me_pghead: me_pglast must be inited along with it.
	 * Unsupported in nested txns: They would need to hide the page
	 * range in ancestor txns' dirty and spilled lists.
	 */
	if (env->me_pghead &&
		!txn->mt_parent &&
		((mp->mp_flags & P_DIRTY) ||
		 (sl && (x = mdb_midl_search(sl, pn)) <= sl[0] && sl[x] == pn)))
	{
		unsigned i, j;
		pgno_t *mop;
		MDB_ID2 *dl, ix, iy;
		rc = mdb_midl_need(&env->me_pghead, ovpages);
		if (rc)
			return rc;
		if (!(mp->mp_flags & P_DIRTY)) {
			/* This page is no longer spilled */
			if (x == sl[0])
				sl[0]--;
			else
				sl[x] |= 1;
			goto release;
		}
		/* Remove from dirty list */
		dl = txn->mt_u.dirty_list;
		x = dl[0].mid--;
		for (ix = dl[x]; ix.mptr != mp; ix = iy) {
			if (x > 1) {
				x--;
				iy = dl[x];
				dl[x] = ix;
			} else {
				mdb_cassert(mc, x > 1);
				j = ++(dl[0].mid);
				dl[j] = ix;		/* Unsorted. OK when MDB_TXN_ERROR. */
				mdb_txn_mark_error(txn, MDB_PROBLEM);
				return MDB_PROBLEM;
			}
		}
		txn->mt_dirty_room++;
		if (!(env->me_flags & MDB_WRITEMAP))
			mdb_dpage_free(env, mp);
release:
		/* Insert in me_pghead */
		mop = env->me_pghead;
		j = mop[0] + ovpages;
		for (i = mop[0]; i && mop[i] < pg; i--)
			mop[j--] = mop[i];
		while (j>i)
			mop[j--] = pg++;
		mop[0] += ovpages;
	} else {
		rc = mdb_midl_append_range(&txn->mt_free_pgs, pg, ovpages);
		if (rc)
			return rc;
	}
#ifdef MDB_VL32
	if (mc->mc_ovpg == mp)
		mc->mc_ovpg = NULL;
#endif
	mc->mc_db->md_overflow_pages -= ovpages;
	return 0;
}

/** Return the data associated with a given node.
 * @param[in] mc The cursor for this operation.
 * @param[in] leaf The node being read.
 * @param[out] data Updated to point to the node's data.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_node_read(MDB_cursor *mc, MDB_node *leaf, MDB_val *data)
{
	MDB_page	*omp;		/* overflow page */
	pgno_t		 pgno;
	int rc;

	if (MC_OVPG(mc)) {
		MDB_PAGE_UNREF(mc->mc_txn, MC_OVPG(mc));
		MC_SET_OVPG(mc, NULL);
	}
	if (!F_ISSET(leaf->mn_flags, F_BIGDATA)) {
		data->mv_size = NODEDSZ(leaf);
		data->mv_data = NODEDATA(leaf);
		return MDB_SUCCESS;
	}

	/* Read overflow data.
	 */
	data->mv_size = NODEDSZ(leaf);
	memcpy(&pgno, NODEDATA(leaf), sizeof(pgno));
	if ((rc = mdb_page_get(mc, pgno, &omp, NULL)) != 0) {
		DPRINTF(("read overflow page %"Yu" failed", pgno));
		return rc;
	}
	data->mv_data = METADATA(omp);
	MC_SET_OVPG(mc, omp);

	return MDB_SUCCESS;
}

int
mdb_get(MDB_txn *txn, MDB_dbi dbi,
	MDB_val *key, MDB_val *data)
{
	MDB_cursor	mc = (MDB_cursor){0};
	MDB_xcursor	mx = (MDB_xcursor){0};
	int exact = 0, rc;
	DKBUF;

	DPRINTF(("===> get db %u key [%s]", dbi, DKEY(key)));

	if (!key || !data || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	mdb_cursor_init(&mc, txn, dbi, &mx);
	rc = mdb_cursor_set(&mc, key, data, MDB_SET, &exact);
	/* unref all the pages when MDB_VL32 - caller must copy the data
	 * before doing anything else
	 */
	MDB_CURSOR_UNREF(&mc, 1);
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	return rc;
}

/** Find a sibling for a page.
 * Replaces the page at the top of the cursor's stack with the
 * specified sibling, if one exists.
 * @param[in] mc The cursor for this operation.
 * @param[in] move_right Non-zero if the right sibling is requested,
 * otherwise the left sibling.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_cursor_sibling(MDB_cursor *mc, int move_right)
{
	int		 rc;
	MDB_node	*indx;
	MDB_page	*mp;
#ifdef MDB_VL32
	MDB_page	*op;
#endif

	if (mc->mc_snum < 2) {
		return MDB_NOTFOUND;		/* root has no siblings */
	}

#ifdef MDB_VL32
	op = mc->mc_pg[mc->mc_top];
#endif
	mdb_cursor_pop(mc);
	DPRINTF(("parent page is page %"Yu", index %u",
		mc->mc_pg[mc->mc_top]->mp_pgno, mc->mc_ki[mc->mc_top]));

	if (move_right ? (mc->mc_ki[mc->mc_top] + 1u >= NUMKEYS(mc->mc_pg[mc->mc_top]))
		       : (mc->mc_ki[mc->mc_top] == 0)) {
		DPRINTF(("no more keys left, moving to %s sibling",
		    move_right ? "right" : "left"));
		if ((rc = mdb_cursor_sibling(mc, move_right)) != MDB_SUCCESS) {
			/* undo cursor_pop before returning */
			mc->mc_top++;
			mc->mc_snum++;
			return rc;
		}
	} else {
		if (move_right)
			mc->mc_ki[mc->mc_top]++;
		else
			mc->mc_ki[mc->mc_top]--;
		DPRINTF(("just moving to %s index key %u",
		    move_right ? "right" : "left", mc->mc_ki[mc->mc_top]));
	}
	mdb_cassert(mc, IS_BRANCH(mc->mc_pg[mc->mc_top]));

	MDB_PAGE_UNREF(mc->mc_txn, op);

	indx = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
	if ((rc = mdb_page_get(mc, NODEPGNO(indx), &mp, NULL)) != 0) {
		/* mc will be inconsistent if caller does mc_snum++ as above */
		mc->mc_flags &= ~(C_INITIALIZED|C_EOF);
		return rc;
	}

	mdb_cursor_push(mc, mp);
	if (!move_right)
		mc->mc_ki[mc->mc_top] = NUMKEYS(mp)-1;

	return MDB_SUCCESS;
}

/** Move the cursor to the next data item. */
static int
mdb_cursor_next(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op)
{
	MDB_page	*mp;
	MDB_node	*leaf;
	int rc;

	if ((mc->mc_flags & C_DEL && op == MDB_NEXT_DUP))
		return MDB_NOTFOUND;

	if (!(mc->mc_flags & C_INITIALIZED))
		return mdb_cursor_first(mc, key, data);

	mp = mc->mc_pg[mc->mc_top];

	if (mc->mc_flags & C_EOF) {
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mp)-1)
			return MDB_NOTFOUND;
		mc->mc_flags ^= C_EOF;
	}

	if (mc->mc_db->md_flags & MDB_DUPSORT) {
		leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
			if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
				if (op == MDB_NEXT || op == MDB_NEXT_DUP) {
					rc = mdb_cursor_next(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_NEXT);
					if (op != MDB_NEXT || rc != MDB_NOTFOUND) {
						if (rc == MDB_SUCCESS) {
							MDB_val *target = key ? key : &mc->mc_key;
							if (mc->mc_key_pgno == mp->mp_pgno &&
							    mc->mc_key_last == mc->mc_ki[mc->mc_top]) {
								if (key)
									*key = mc->mc_key;
							} else {
								int krc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], target);
								if (krc != MDB_SUCCESS)
									return krc;
							}
							mc->mc_seq_pgno = mp->mp_pgno;
							mc->mc_seq_idx = mc->mc_ki[mc->mc_top];
						}
						return rc;
					}
				}
				else {
				MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
			}
		} else {
			mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if (op == MDB_NEXT_DUP)
				return MDB_NOTFOUND;
		}
	}

	DPRINTF(("cursor_next: top page is %"Yu" in cursor %p",
		mdb_dbg_pgno(mp), (void *) mc));
	if (mc->mc_flags & C_DEL) {
		mc->mc_flags ^= C_DEL;
		goto skip;
	}

	if (mc->mc_ki[mc->mc_top] + 1u >= NUMKEYS(mp)) {
		DPUTS("=====> move to next sibling page");
		if ((rc = mdb_cursor_sibling(mc, 1)) != MDB_SUCCESS) {
			mc->mc_flags |= C_EOF;
			return rc;
		}
		mp = mc->mc_pg[mc->mc_top];
		DPRINTF(("next page is %"Yu", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
	} else
		mc->mc_ki[mc->mc_top]++;

skip:
	DPRINTF(("==> cursor points to page %"Yu" with %u keys, key index %u",
	    mdb_dbg_pgno(mp), NUMKEYS(mp), mc->mc_ki[mc->mc_top]));

	if (IS_LEAF2(mp)) {
		key->mv_size = mc->mc_db->md_pad;
		key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		return MDB_SUCCESS;
	}

	mdb_cassert(mc, IS_LEAF(mp));
	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	if (key) {
		if (mc->mc_ki[mc->mc_top] > 0) {
			mc->mc_flags |= C_SEQEXPECT;
			mc->mc_seq_pgno = mp->mp_pgno;
			mc->mc_seq_idx = mc->mc_ki[mc->mc_top] - 1;
		} else {
			mc->mc_flags &= ~C_SEQEXPECT;
			mdb_cursor_seq_invalidate(mc);
		}
		rc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
		mc->mc_flags &= ~C_SEQEXPECT;
		if (rc != MDB_SUCCESS) {
			mdb_cursor_seq_invalidate(mc);
			return rc;
		}
		mc->mc_seq_pgno = mp->mp_pgno;
		mc->mc_seq_idx = mc->mc_ki[mc->mc_top];
	} else {
		mc->mc_flags &= ~C_SEQEXPECT;
		mdb_cursor_seq_invalidate(mc);
	}
	return MDB_SUCCESS;
}

/** Move the cursor to the previous data item. */
static int
mdb_cursor_prev(MDB_cursor *mc, MDB_val *key, MDB_val *data, MDB_cursor_op op)
{
	MDB_page	*mp;
	MDB_node	*leaf;
	int rc;

	if (!(mc->mc_flags & C_INITIALIZED)) {
		rc = mdb_cursor_last(mc, key, data);
		if (rc)
			return rc;
		mc->mc_ki[mc->mc_top]++;
	}

	mp = mc->mc_pg[mc->mc_top];

	if ((mc->mc_db->md_flags & MDB_DUPSORT) &&
		mc->mc_ki[mc->mc_top] < NUMKEYS(mp)) {
		leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
		if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
			if (op == MDB_PREV || op == MDB_PREV_DUP) {
				rc = mdb_cursor_prev(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_PREV);
				if (op != MDB_PREV || rc != MDB_NOTFOUND) {
					if (rc == MDB_SUCCESS && key) {
						int krc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
						if (krc != MDB_SUCCESS)
							return krc;
					}
					if (rc == MDB_SUCCESS)
						mc->mc_flags &= ~C_EOF;
					return rc;
				}
			}
			else {
				MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
			}
		} else {
			mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if (op == MDB_PREV_DUP)
				return MDB_NOTFOUND;
		}
	}

	DPRINTF(("cursor_prev: top page is %"Yu" in cursor %p",
		mdb_dbg_pgno(mp), (void *) mc));

	mc->mc_flags &= ~(C_EOF|C_DEL);

	if (mc->mc_ki[mc->mc_top] == 0)  {
		DPUTS("=====> move to prev sibling page");
		if ((rc = mdb_cursor_sibling(mc, 0)) != MDB_SUCCESS) {
			return rc;
		}
		mp = mc->mc_pg[mc->mc_top];
		mc->mc_ki[mc->mc_top] = NUMKEYS(mp) - 1;
		DPRINTF(("prev page is %"Yu", key index %u", mp->mp_pgno, mc->mc_ki[mc->mc_top]));
	} else
		mc->mc_ki[mc->mc_top]--;

	DPRINTF(("==> cursor points to page %"Yu" with %u keys, key index %u",
	    mdb_dbg_pgno(mp), NUMKEYS(mp), mc->mc_ki[mc->mc_top]));

	if (!IS_LEAF(mp))
		return MDB_CORRUPTED;

	if (IS_LEAF2(mp)) {
		key->mv_size = mc->mc_db->md_pad;
		key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		return MDB_SUCCESS;
	}

	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_last(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	if (key) {
		rc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	return MDB_SUCCESS;
}

/** Set the cursor on a specific data item. */
static int
mdb_cursor_set(MDB_cursor *mc, MDB_val *key, MDB_val *data,
	MDB_cursor_op op, int *exactp)
{
	int		 rc;
	MDB_page	*mp;
	MDB_node	*leaf = NULL;
	MDB_val	 saved_key = *key;
	MDB_val	 saved_data = {0, NULL};
	int	 saved_data_valid = 0;
	unsigned char saved_key_local[MDB_KEYBUF_MAX];
	unsigned char *saved_key_copy = NULL;
	DKBUF;

	if ((mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) &&
	    (op == MDB_GET_BOTH || op == MDB_GET_BOTH_RANGE)) {
		if (data) {
			saved_data = *data;
			saved_data_valid = 1;
		}
		if (key->mv_data && key->mv_size) {
			if (key->mv_size <= MDB_KEYBUF_MAX) {
				memcpy(saved_key_local, key->mv_data, key->mv_size);
				saved_key_copy = saved_key_local;
			} else {
				unsigned char *scratch_copy = NULL;
				int copy_rc = mdb_prefix_ensure_keybuf(mc->mc_txn, key->mv_size, &scratch_copy);
				if (copy_rc != MDB_SUCCESS)
					return copy_rc;
				memcpy(scratch_copy, key->mv_data, key->mv_size);
				saved_key_copy = scratch_copy;
			}
			if (saved_key_copy) {
				key->mv_data = saved_key_copy;
				key->mv_size = saved_key.mv_size;
			}
		}
	}
	if (key->mv_size == 0)
		return MDB_BAD_VALSIZE;


	if (mc->mc_xcursor) {
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
		mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
	}

	if ((mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) &&
	    (op == MDB_GET_BOTH || op == MDB_GET_BOTH_RANGE))
		mc->mc_flags &= ~C_INITIALIZED;

	/* See if we're already on the right page */
	if (mc->mc_flags & C_INITIALIZED) {
		MDB_val nodekey;

		mp = mc->mc_pg[mc->mc_top];
		if (!NUMKEYS(mp)) {
			mc->mc_ki[mc->mc_top] = 0;
			return MDB_NOTFOUND;
		}
		if (MP_FLAGS(mp) & P_LEAF2) {
			nodekey.mv_size = mc->mc_db->md_pad;
			nodekey.mv_data = LEAF2KEY(mp, 0, nodekey.mv_size);
		} else {
			leaf = NODEPTR(mp, 0);
			rc = mdb_cursor_read_key_at(mc, mp, 0, &nodekey);
			if (rc != MDB_SUCCESS)
				return rc;
		}
		rc = mc->mc_dbx->md_cmp(key, &nodekey);
		if (rc == 0) {
			/* Probably happens rarely, but first node on the page
			 * was the one we wanted.
			 */
			mc->mc_ki[mc->mc_top] = 0;
			if (exactp)
				*exactp = 1;
			goto set1;
		}
		if (rc > 0) {
			unsigned int i;
			unsigned int nkeys = NUMKEYS(mp);
			if (nkeys > 1) {
				if (MP_FLAGS(mp) & P_LEAF2) {
					nodekey.mv_data = LEAF2KEY(mp,
						 nkeys-1, nodekey.mv_size);
				} else {
				leaf = NODEPTR(mp, nkeys-1);
				rc = mdb_cursor_read_key_at(mc, mp, nkeys-1, &nodekey);
				if (rc != MDB_SUCCESS)
					return rc;
				}
				rc = mc->mc_dbx->md_cmp(key, &nodekey);
				if (rc == 0) {
					/* last node was the one we wanted */
					mc->mc_ki[mc->mc_top] = nkeys-1;
					if (exactp)
						*exactp = 1;
					goto set1;
				}
				if (rc < 0) {
					if (mc->mc_ki[mc->mc_top] < NUMKEYS(mp)) {
						/* This is definitely the right page, skip search_page */
						if (MP_FLAGS(mp) & P_LEAF2) {
							nodekey.mv_data = LEAF2KEY(mp,
								 mc->mc_ki[mc->mc_top], nodekey.mv_size);
						} else {
					leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
					rc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], &nodekey);
					if (rc != MDB_SUCCESS)
						return rc;
						}
						rc = mc->mc_dbx->md_cmp(key, &nodekey);
						if (rc == 0) {
							/* current node was the one we wanted */
							if (exactp)
								*exactp = 1;
							goto set1;
						}
					}
					rc = 0;
					mc->mc_flags &= ~C_EOF;
					goto set2;
				}
			}
			/* If any parents have right-sibs, search.
			 * Otherwise, there's nothing further.
			 */
			for (i=0; i<mc->mc_top; i++)
				if (mc->mc_ki[i] <
					NUMKEYS(mc->mc_pg[i])-1)
					break;
			if (i == mc->mc_top) {
				if ((mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) &&
				    (op == MDB_GET_BOTH_RANGE || op == MDB_GET_BOTH)) {
					mdb_cursor_seq_invalidate(mc);
					mc->mc_flags &= ~C_INITIALIZED;
					mc->mc_pg[0] = NULL;
					if (saved_key_copy && (op == MDB_GET_BOTH || op == MDB_GET_BOTH_RANGE))
						*key = saved_key;
					if (saved_data_valid && data)
						*data = saved_data;
					return MDB_NOTFOUND;
				}
				/* There are no other pages */
				mc->mc_ki[mc->mc_top] = nkeys;
				return MDB_NOTFOUND;
			}
		}
		if (!mc->mc_top) {
			/* There are no other pages */
			mc->mc_ki[mc->mc_top] = 0;
			if (op == MDB_SET_RANGE && !exactp) {
				rc = 0;
				goto set1;
			} else
				return MDB_NOTFOUND;
		}
	} else {
		mc->mc_pg[0] = 0;
	}

	rc = mdb_page_search(mc, key, 0);
	if (rc != MDB_SUCCESS)
		return rc;

	mp = mc->mc_pg[mc->mc_top];
	mdb_cassert(mc, IS_LEAF(mp));

set2:
	leaf = mdb_node_search(mc, key, exactp);
	if (exactp != NULL && !*exactp) {
		/* MDB_SET specified and not an exact match. */
		return MDB_NOTFOUND;
	}

	if (leaf == NULL) {
		DPUTS("===> inexact leaf not found, goto sibling");
		if ((rc = mdb_cursor_sibling(mc, 1)) != MDB_SUCCESS) {
			mc->mc_flags |= C_EOF;
			return rc;		/* no entries matched */
		}
		mp = mc->mc_pg[mc->mc_top];
		mdb_cassert(mc, IS_LEAF(mp));
		leaf = NODEPTR(mp, 0);
	}

set1:
	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~C_EOF;

	if (IS_LEAF2(mp)) {
		if (op == MDB_SET_RANGE || op == MDB_SET_KEY) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		}
		return MDB_SUCCESS;
	}

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		if (op == MDB_SET || op == MDB_SET_KEY || op == MDB_SET_RANGE) {
			rc = mdb_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
		} else {
			int ex2, *ex2p;
			if (op == MDB_GET_BOTH) {
				ex2p = &ex2;
				ex2 = 0;
			} else {
				ex2p = NULL;
			}
			rc = mdb_cursor_set(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_SET_RANGE, ex2p);
			if (rc != MDB_SUCCESS)
				return rc;
		}
	} else if (data) {
		if (op == MDB_GET_BOTH || op == MDB_GET_BOTH_RANGE) {
			MDB_val olddata;
			MDB_cmp_func *dcmp;
			if ((rc = mdb_node_read(mc, leaf, &olddata)) != MDB_SUCCESS)
				return rc;
			dcmp = mc->mc_dbx->md_dcmp;
			if (NEED_CMP_CLONG(dcmp, olddata.mv_size))
				dcmp = mdb_cmp_clong;
			rc = dcmp(data, &olddata);
			if (rc) {
				if (op == MDB_GET_BOTH || rc > 0)
					return MDB_NOTFOUND;
				rc = 0;
			}
			*data = olddata;

		} else {
			if (mc->mc_xcursor)
				mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
			if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
				return rc;
		}
	}

	/* The key already matches in all other cases */
	if (op == MDB_SET_RANGE || op == MDB_SET_KEY || saved_key_copy) {
		rc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
		if (rc != MDB_SUCCESS)
			return rc;
	}
DPRINTF(("==> cursor placed on key [%s]", DKEY(key)));
	if (saved_key_copy && (op == MDB_GET_BOTH || op == MDB_GET_BOTH_RANGE))
		*key = saved_key;

	return rc;
}

/** Move the cursor to the first item in the database. */
static int
mdb_cursor_first(MDB_cursor *mc, MDB_val *key, MDB_val *data)
{
	int		 rc;
	MDB_node	*leaf;
	MDB_page	*mp;

	if (mc->mc_xcursor) {
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
		mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
	}

	if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
		rc = mdb_page_search(mc, NULL, MDB_PS_FIRST);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	mdb_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));
	mp = mc->mc_pg[mc->mc_top];

	leaf = NODEPTR(mp, 0);
	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~C_EOF;

	mc->mc_ki[mc->mc_top] = 0;

	if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
		if ( key ) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mc->mc_pg[mc->mc_top], 0, key->mv_size);
		}
		return MDB_SUCCESS;
	}

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_first(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	if (key) {
		rc = mdb_cursor_read_key_at(mc, mp, 0, key);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	return MDB_SUCCESS;
}

/** Move the cursor to the last item in the database. */
static int
mdb_cursor_last(MDB_cursor *mc, MDB_val *key, MDB_val *data)
{
	int		 rc;
	MDB_node	*leaf;
	MDB_page	*mp;
	if (mc->mc_xcursor) {
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);
		mc->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
	}

	if (!(mc->mc_flags & C_INITIALIZED) || mc->mc_top) {
		rc = mdb_page_search(mc, NULL, MDB_PS_LAST);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	mdb_cassert(mc, IS_LEAF(mc->mc_pg[mc->mc_top]));
	mp = mc->mc_pg[mc->mc_top];

	mc->mc_ki[mc->mc_top] = NUMKEYS(mp) - 1;
	mc->mc_flags |= C_INITIALIZED|C_EOF;
	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (IS_LEAF2(mp)) {
		if (key) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		}
		return MDB_SUCCESS;
	}

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		mdb_xcursor_init1(mc, leaf);
		rc = mdb_cursor_last(&mc->mc_xcursor->mx_cursor, data, NULL);
		if (rc)
			return rc;
	} else if (data) {
		if ((rc = mdb_node_read(mc, leaf, data)) != MDB_SUCCESS)
			return rc;
	}

	if (key) {
		rc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
		if (rc != MDB_SUCCESS)
			return rc;
	}
	return MDB_SUCCESS;
}

int
mdb_cursor_get(MDB_cursor *mc, MDB_val *key, MDB_val *data,
	MDB_cursor_op op)
{
	int		 rc;
	int		 exact = 0;
	int		 (*mfunc)(MDB_cursor *mc, MDB_val *key, MDB_val *data);

	if (mc == NULL)
		return EINVAL;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	rc = mdb_txn_check_interrupt(mc->mc_txn);
	if (rc)
		return rc;

	if (op != MDB_GET_CURRENT)
		mdb_cursor_rank_invalidate(mc);

	switch (op) {
	case MDB_GET_CURRENT:
		if (!(mc->mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
		} else {
			MDB_page *mp = mc->mc_pg[mc->mc_top];
			int nkeys = NUMKEYS(mp);
			if (!nkeys || mc->mc_ki[mc->mc_top] >= nkeys) {
				mc->mc_ki[mc->mc_top] = nkeys;
				rc = MDB_NOTFOUND;
				break;
			}
			rc = MDB_SUCCESS;
			if (IS_LEAF2(mp)) {
				key->mv_size = mc->mc_db->md_pad;
				key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
			} else {
				MDB_node *leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
				if (key) {
					int krc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
					if (krc != MDB_SUCCESS)
						return krc;
				}
				if (data) {
					if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
						rc = mdb_cursor_get(&mc->mc_xcursor->mx_cursor, data, NULL, MDB_GET_CURRENT);
					} else {
						rc = mdb_node_read(mc, leaf, data);
					}
				}
			}
		}
		break;
	case MDB_GET_BOTH:
	case MDB_GET_BOTH_RANGE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		if (mc->mc_xcursor == NULL) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		/* FALLTHRU */
	case MDB_SET:
	case MDB_SET_KEY:
	case MDB_SET_RANGE:
		if (key == NULL) {
			rc = EINVAL;
		} else {
			rc = mdb_cursor_set(mc, key, data, op,
				op == MDB_SET_RANGE ? NULL : &exact);
		}
		break;
	case MDB_GET_MULTIPLE:
		if (data == NULL || !(mc->mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
			break;
		}
		if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		rc = MDB_SUCCESS;
		if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) ||
			(mc->mc_xcursor->mx_cursor.mc_flags & C_EOF))
			break;
		goto fetchm;
	case MDB_NEXT_MULTIPLE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		rc = mdb_cursor_next(mc, key, data, MDB_NEXT_DUP);
		if (rc == MDB_SUCCESS) {
			if (mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
				MDB_cursor *mx;
fetchm:
				mx = &mc->mc_xcursor->mx_cursor;
				data->mv_size = NUMKEYS(mx->mc_pg[mx->mc_top]) *
					mx->mc_db->md_pad;
				data->mv_data = METADATA(mx->mc_pg[mx->mc_top]);
				mx->mc_ki[mx->mc_top] = NUMKEYS(mx->mc_pg[mx->mc_top])-1;
			} else {
				rc = MDB_NOTFOUND;
			}
		}
		break;
	case MDB_PREV_MULTIPLE:
		if (data == NULL) {
			rc = EINVAL;
			break;
		}
		if (!(mc->mc_db->md_flags & MDB_DUPFIXED)) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		if (!(mc->mc_flags & C_INITIALIZED))
			rc = mdb_cursor_last(mc, key, data);
		else
			rc = MDB_SUCCESS;
		if (rc == MDB_SUCCESS) {
			MDB_cursor *mx = &mc->mc_xcursor->mx_cursor;
			if (mx->mc_flags & C_INITIALIZED) {
				rc = mdb_cursor_sibling(mx, 0);
				if (rc == MDB_SUCCESS)
					goto fetchm;
			} else {
				rc = MDB_NOTFOUND;
			}
		}
		break;
	case MDB_NEXT:
	case MDB_NEXT_DUP:
	case MDB_NEXT_NODUP:
		rc = mdb_cursor_next(mc, key, data, op);
		break;
	case MDB_PREV:
	case MDB_PREV_DUP:
	case MDB_PREV_NODUP:
		rc = mdb_cursor_prev(mc, key, data, op);
		break;
	case MDB_FIRST:
		rc = mdb_cursor_first(mc, key, data);
		break;
case MDB_FIRST_DUP:
	mfunc = mdb_cursor_first;
mmove:
	if (data == NULL || !(mc->mc_flags & C_INITIALIZED)) {
		rc = EINVAL;
			break;
		}
		if (mc->mc_xcursor == NULL) {
			rc = MDB_INCOMPATIBLE;
			break;
		}
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top])) {
			mc->mc_ki[mc->mc_top] = NUMKEYS(mc->mc_pg[mc->mc_top]);
			rc = MDB_NOTFOUND;
			break;
		}
		mc->mc_flags &= ~C_EOF;
		{
			MDB_page *mp = mc->mc_pg[mc->mc_top];
			MDB_node *leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
			if (!F_ISSET(leaf->mn_flags, F_DUPDATA)) {
				if (key) {
					int krc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
					if (krc != MDB_SUCCESS)
						return krc;
				}
				rc = mdb_node_read(mc, leaf, data);
				break;
			}
		}
		if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED)) {
			rc = EINVAL;
			break;
		}
	rc = mfunc(&mc->mc_xcursor->mx_cursor, data, NULL);
	if (rc == MDB_SUCCESS && key) {
		MDB_page *mp = mc->mc_pg[mc->mc_top];
		int krc = mdb_cursor_read_key_at(mc, mp, mc->mc_ki[mc->mc_top], key);
		if (krc != MDB_SUCCESS)
			return krc;
	}
	break;
case MDB_LAST:
	rc = mdb_cursor_last(mc, key, data);
	break;
	case MDB_LAST_DUP:
		mfunc = mdb_cursor_last;
		goto mmove;
	default:
		DPRINTF(("unhandled/unimplemented cursor operation %u", op));
		rc = EINVAL;
		break;
	}

	if (mc->mc_flags & C_DEL)
		mc->mc_flags ^= C_DEL;

	return rc;
}

/** Touch all the pages in the cursor stack. Set mc_top.
 *	Makes sure all the pages are writable, before attempting a write operation.
 * @param[in] mc The cursor to operate on.
 */
static int
mdb_cursor_touch(MDB_cursor *mc)
{
	int rc = MDB_SUCCESS;

	if (mc->mc_dbi >= CORE_DBS && !(*mc->mc_dbflag & (DB_DIRTY|DB_DUPDATA))) {
		/* Touch DB record of named DB */
		MDB_cursor mc2 = (MDB_cursor){0};
		MDB_xcursor mcx = (MDB_xcursor){0};
		if (TXN_DBI_CHANGED(mc->mc_txn, mc->mc_dbi))
			return MDB_BAD_DBI;
		mdb_cursor_init(&mc2, mc->mc_txn, MAIN_DBI, &mcx);
		rc = mdb_page_search(&mc2, &mc->mc_dbx->md_name, MDB_PS_MODIFY);
		if (rc)
			 return rc;
		*mc->mc_dbflag |= DB_DIRTY;
	}
	mc->mc_top = 0;
	if (mc->mc_snum) {
		do {
			rc = mdb_page_touch(mc);
		} while (!rc && ++(mc->mc_top) < mc->mc_snum);
		mc->mc_top = mc->mc_snum-1;
	}
	return rc;
}

/** Do not spill pages to disk if txn is getting full, may fail instead */
#define MDB_NOSPILL	0x8000

static int
_mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data,
	unsigned int flags)
{
	MDB_env		*env;
	MDB_node	*leaf = NULL;
	MDB_page	*fp, *mp, *sub_root = NULL;
	uint16_t	fp_flags;
	MDB_val		xdata, *rdata, dkey, olddata;
	MDB_db dummy;
	int do_sub = 0, insert_key, insert_data;
	int split_performed = 0;
	int inline_pair_ready = 0;
	unsigned int mcount = 0, dcount = 0, nospill;
	size_t nsize;
	int rc, rc2;
	unsigned int nflags;
	DKBUF;

	if (mc == NULL || key == NULL)
		return EINVAL;

	env = mc->mc_txn->mt_env;

	/* Check this first so counter will always be zero on any
	 * early failures.
	 */
	if (flags & MDB_MULTIPLE) {
		dcount = data[1].mv_size;
		data[1].mv_size = 0;
		if (!F_ISSET(mc->mc_db->md_flags, MDB_DUPFIXED))
			return MDB_INCOMPATIBLE;
	}

	nospill = flags & MDB_NOSPILL;
	flags &= ~MDB_NOSPILL;

	if (mc->mc_txn->mt_flags & MDB_TXN_RDONLY)
		return EACCES;
	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED) {
		if ((mc->mc_txn->mt_flags & MDB_TXN_ERROR) && mc->mc_txn->mt_last_err)
			return mc->mc_txn->mt_last_err;
		return MDB_BAD_TXN;
	}

	rc = mdb_txn_check_interrupt(mc->mc_txn);
	if (rc)
		return rc;

	if (key->mv_size-1 >= ENV_MAXKEY(env))
		return MDB_BAD_VALSIZE;

#if SIZE_MAX > MAXDATASIZE
	if (data->mv_size > ((mc->mc_db->md_flags & MDB_DUPSORT) ? ENV_MAXKEY(env) : MAXDATASIZE))
		return MDB_BAD_VALSIZE;
#else
	if ((mc->mc_db->md_flags & MDB_DUPSORT) && data->mv_size > ENV_MAXKEY(env))
		return MDB_BAD_VALSIZE;
#endif

	DPRINTF(("==> put db %d key [%s], size %"Z"u, data size %"Z"u",
		DDBI(mc), DKEY(key), key ? key->mv_size : 0, data->mv_size));

	dkey.mv_size = 0;

	if (flags & MDB_CURRENT) {
		if (!(mc->mc_flags & C_INITIALIZED))
			return EINVAL;
		rc = MDB_SUCCESS;
	} else if (mc->mc_db->md_root == P_INVALID) {
		/* new database, cursor has nothing to point to */
		mc->mc_snum = 0;
		mc->mc_top = 0;
		mc->mc_flags &= ~C_INITIALIZED;
		rc = MDB_NO_ROOT;
	} else {
		int exact = 0;
		MDB_val d2;
		if (flags & MDB_APPEND) {
			MDB_val k2;
			rc = mdb_cursor_last(mc, &k2, &d2);
			if (rc == 0) {
				rc = mc->mc_dbx->md_cmp(key, &k2);
				if (rc > 0) {
					rc = MDB_NOTFOUND;
					mc->mc_ki[mc->mc_top]++;
				} else {
					/* new key is <= last key */
					rc = MDB_KEYEXIST;
				}
			}
		} else {
			rc = mdb_cursor_set(mc, key, &d2, MDB_SET, &exact);
		}
		if ((flags & MDB_NOOVERWRITE) && rc == 0) {
			DPRINTF(("duplicate key [%s]", DKEY(key)));
			*data = d2;
			return MDB_KEYEXIST;
		}
		if (rc && rc != MDB_NOTFOUND)
			return rc;
	}

	if (mc->mc_flags & C_DEL)
		mc->mc_flags ^= C_DEL;

	/* Cursor is positioned, check for room in the dirty list */
	if (!nospill) {
		if (flags & MDB_MULTIPLE) {
			rdata = &xdata;
			xdata.mv_size = data->mv_size * dcount;
		} else {
			rdata = data;
		}
		if ((rc2 = mdb_page_spill(mc, key, rdata)))
			return rc2;
	}

	if (rc == MDB_NO_ROOT) {
		MDB_page *np;
		/* new database, write a root leaf page */
		DPUTS("allocating new root leaf page");
		if ((rc2 = mdb_page_new(mc, P_LEAF, 1, &np))) {
			return rc2;
		}
		mdb_cursor_push(mc, np);
		mc->mc_db->md_root = np->mp_pgno;
		mc->mc_db->md_depth++;
		*mc->mc_dbflag |= DB_DIRTY;
		if ((mc->mc_db->md_flags & (MDB_DUPSORT|MDB_DUPFIXED))
			== MDB_DUPFIXED)
			MP_FLAGS(np) |= P_LEAF2;
		mc->mc_flags |= C_INITIALIZED;
	} else {
		/* make sure all cursor pages are writable */
		rc2 = mdb_cursor_touch(mc);
		if (rc2)
			return rc2;
	}

	insert_key = insert_data = rc;
	if (insert_key) {
		/* The key does not exist */
		DPRINTF(("inserting key at index %i", mc->mc_ki[mc->mc_top]));
		if ((mc->mc_db->md_flags & MDB_DUPSORT) &&
			LEAFSIZE(key, data) > env->me_nodemax)
		{
			/* Too big for a node, insert in sub-DB.  Set up an empty
			 * "old sub-page" for prep_subDB to expand to a full page.
			 */
			fp_flags = P_LEAF|P_DIRTY;
			fp = env->me_pbuf;
			fp->mp_pad = data->mv_size; /* used if MDB_DUPFIXED */
			MP_LOWER(fp) = MP_UPPER(fp) = (PAGEHDRSZ-PAGEBASE);
			olddata.mv_size = PAGEHDRSZ;
			goto prep_subDB;
		}
	} else {
		/* there's only a key anyway, so this is a no-op */
		if (IS_LEAF2(mc->mc_pg[mc->mc_top])) {
			char *ptr;
			unsigned int ksize = mc->mc_db->md_pad;
			if (key->mv_size != ksize)
				return MDB_BAD_VALSIZE;
			ptr = LEAF2KEY(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top], ksize);
			memcpy(ptr, key->mv_data, ksize);
fix_parent:
			/* if overwriting slot 0 of leaf, need to
			 * update branch key if there is a parent page
			 */
			if (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
				unsigned short dtop = 1;
				mc->mc_top--;
				/* slot 0 is always an empty key, find real slot */
				while (mc->mc_top && !mc->mc_ki[mc->mc_top]) {
					mc->mc_top--;
					dtop++;
				}
				if (mc->mc_ki[mc->mc_top])
					rc2 = mdb_update_key(mc, key);
				else
					rc2 = MDB_SUCCESS;
				mc->mc_top += dtop;
				if (rc2)
					return rc2;
			}
			return MDB_SUCCESS;
		}

more:
		leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
		olddata.mv_size = NODEDSZ(leaf);
		olddata.mv_data = NODEDATA(leaf);

		if (F_ISSET(leaf->mn_flags, F_DUPDATA|F_SUBDATA))
			flags |= leaf->mn_flags & (F_DUPDATA|F_SUBDATA);

		/* DB has dups? */
		if (F_ISSET(mc->mc_db->md_flags, MDB_DUPSORT)) {
			/* Prepare (sub-)page/sub-DB to accept the new item,
			 * if needed.  fp: old sub-page or a header faking
			 * it.  mp: new (sub-)page.  offset: growth in page
			 * size.  xdata: node data with new page or DB.
			 */
			unsigned	i, offset = 0;
			int force_subdb = 0;
			MDB_val zerodata = {0, NULL};
			uint16_t parent_flags = mc->mc_txn->mt_dbs[mc->mc_dbi].md_flags;
			int parent_prefix = (parent_flags & MDB_PREFIX_COMPRESSION) != 0;
			mp = fp = xdata.mv_data = env->me_pbuf;
			mp->mp_pgno = mc->mc_pg[mc->mc_top]->mp_pgno;

			/* Was a single item before, must convert now */
			if (!F_ISSET(leaf->mn_flags, F_DUPDATA)) {
				MDB_cmp_func *dcmp;
				int cmp;
				MDB_val original_dup = olddata;
				/* Just overwrite the current item */
				if (flags == MDB_CURRENT)
					goto current;
				dcmp = mc->mc_dbx->md_dcmp;
				if (NEED_CMP_CLONG(dcmp, olddata.mv_size))
					dcmp = mdb_cmp_clong;
				/* does data match? */
				cmp = dcmp(data, &olddata);
				unsigned int insert_index = (cmp < 0) ? 0 : 1;
				if (!cmp) {
					if (flags & (MDB_NODUPDATA|MDB_APPENDDUP))
						return MDB_KEYEXIST;
					/* overwrite it */
					goto current;
				}

				/* Back up original data item */
				dkey.mv_size = olddata.mv_size;
				dkey.mv_data = memcpy(fp+1, olddata.mv_data, olddata.mv_size);

				/* Make sub-page header for the dup items, with dummy body */
				MP_FLAGS(fp) = P_LEAF|P_DIRTY|P_SUBP;
				MP_LOWER(fp) = (PAGEHDRSZ-PAGEBASE);
				xdata.mv_size = PAGEHDRSZ + dkey.mv_size + data->mv_size;
				if (mc->mc_db->md_flags & MDB_DUPFIXED) {
					MP_FLAGS(fp) |= P_LEAF2;
					fp->mp_pad = data->mv_size;
					xdata.mv_size += 2 * data->mv_size;	/* leave space for 2 more */
				} else {
					xdata.mv_size += 2 * (sizeof(indx_t) + NODESIZE) +
						(dkey.mv_size & 1) + (data->mv_size & 1);
				}
				MP_UPPER(fp) = xdata.mv_size - PAGEBASE;
				olddata.mv_size = xdata.mv_size; /* pretend olddata is fp */
				if (parent_prefix && !IS_LEAF2(fp)) {
					MDB_prefix_rebuild_entry calc_entries[2];
					MDB_val measure_items[2];
					size_t required = 0;
					memset(calc_entries, 0, sizeof(calc_entries));
					measure_items[0] = (insert_index == 0) ? *data : original_dup;
					measure_items[1] = (insert_index == 0) ? original_dup : *data;
					for (unsigned int i = 0; i < 2; ++i) {
						calc_entries[i].flags = 0;
						calc_entries[i].data_size = 0;
						calc_entries[i].data_payload = 0;
						calc_entries[i].data_ptr = NULL;
						calc_entries[i].key = measure_items[i];
					}
					rc2 = mdb_leaf_rebuild_measure(env, mc->mc_txn,
					    calc_entries, 2, env->me_psize, &required);
					if (rc2 != MDB_SUCCESS && rc2 != MDB_PAGE_FULL)
						return rc2;
					if (rc2 == MDB_PAGE_FULL || required > env->me_psize) {
						force_subdb = 1;
					} else if (required > olddata.mv_size) {
						size_t target = EVEN(required);
						if (target > env->me_psize)
							force_subdb = 1;
						else if (target > olddata.mv_size) {
							size_t extra = target - olddata.mv_size;
							xdata.mv_size += extra;
							olddata.mv_size += extra;
						}
					}
					if (!force_subdb && mc->mc_xcursor) {
						mc->mc_xcursor->mx_inline_required = required;
						mc->mc_xcursor->mx_inline_measure_ready = 1;
					}
				}
				if (!force_subdb) {
					rc2 = mdb_prefix_inline_build_pair(mc, fp, xdata.mv_size,
					    &original_dup, data);
					if (rc2 != MDB_SUCCESS)
						return rc2;
					inline_pair_ready = 1;
					dkey.mv_size = 0;
					dkey.mv_data = NULL;
				}
			} else if (leaf->mn_flags & F_SUBDATA) {
				/* Data is on sub-DB, just store it */
				flags |= F_DUPDATA|F_SUBDATA;
				goto put_sub;
			} else {
				/* Data is on sub-page */
				int prefix_enabled = parent_prefix;
				MDB_cursor *mx;
				size_t free_bytes, need, grow = 0;
				unsigned int dup_index = 0;
				int exact = 0;

				fp = olddata.mv_data;
				if (flags == MDB_CURRENT) {
					MP_FLAGS(fp) |= P_DIRTY;
					MP_SETPGNO(fp, MP_PGNO(mp));
					mc->mc_xcursor->mx_cursor.mc_pg[0] = fp;
					flags |= F_DUPDATA;
					goto put_sub;
				}

				mdb_xcursor_init1(mc, leaf);
				mx = &mc->mc_xcursor->mx_cursor;

				if (flags & MDB_APPENDDUP) {
					dup_index = NUMKEYS(fp);
					mx->mc_ki[mx->mc_top] = dup_index;
					mx->mc_flags |= C_EOF;
				} else {
					MDB_val dummy = zerodata;
					rc2 = mdb_cursor_set(mx, data, &dummy, MDB_SET, &exact);
					if (rc2 != MDB_SUCCESS && rc2 != MDB_NOTFOUND)
						return rc2;
					if (exact) {
						if (flags & MDB_NODUPDATA)
							return MDB_KEYEXIST;
						dup_index = mx->mc_ki[mx->mc_top];
					} else {
						dup_index = mx->mc_ki[mx->mc_top];
						if (rc2 == MDB_NOTFOUND && (mx->mc_flags & C_EOF))
							dup_index = NUMKEYS(fp);
					}
					mx->mc_flags &= ~C_EOF;
				}

				if (mc->mc_db->md_flags & MDB_DUPFIXED) {
					size_t step = fp->mp_pad;
					if (SIZELEFT(fp) < step)
						grow = step * 4;
				} else {
					need = mdb_leaf_size(env, fp, dup_index, data, &zerodata, prefix_enabled);
					free_bytes = SIZELEFT(fp);
					if (need > free_bytes)
						grow = need - free_bytes;
					if (prefix_enabled && dup_index == 0) {
						size_t required = 0;
						MDB_xcursor *owner = mdb_cursor_container(mc);
						int use_cached = 0;

						if (owner && owner->mx_inline_measure_ready) {
							required = owner->mx_inline_required;
							owner->mx_inline_required = 0;
							owner->mx_inline_measure_ready = 0;
							use_cached = 1;
							rc2 = MDB_SUCCESS;
						} else {
							rc2 = mdb_prefix_inline_measure_after_insert(mc, fp,
							    olddata.mv_size, dup_index, data, &zerodata, 0, &required);
						}
						if (!use_cached && rc2 != MDB_SUCCESS && rc2 != MDB_PAGE_FULL)
							return rc2;
						if (rc2 == MDB_PAGE_FULL || required > env->me_psize) {
							force_subdb = 1;
							grow = 0;
						} else if (required > olddata.mv_size) {
							required = EVEN(required);
							if (required > olddata.mv_size) {
								size_t extra = required - olddata.mv_size;
								if (extra > grow)
									grow = extra;
							}
						}
					}
				}

				if (grow == 0 && !force_subdb) {
					MP_FLAGS(fp) |= P_DIRTY;
					MP_SETPGNO(fp, MP_PGNO(mp));
					mc->mc_xcursor->mx_cursor.mc_pg[0] = fp;
					flags |= F_DUPDATA;
					do_sub = 1;
					rc = MDB_SUCCESS;
					goto finish_put;
				}

				offset = (unsigned)grow;
				xdata.mv_size = olddata.mv_size + offset;
				if (force_subdb) {
					offset = 0;
					xdata.mv_size = olddata.mv_size;
				}
				mdb_xcursor_init1(mc, leaf);
			}

			fp_flags = MP_FLAGS(fp);
			if (force_subdb) {
				fp_flags &= ~P_SUBP;
				xdata.mv_size = env->me_nodemax + 1;
			}
			if (NODESIZE + NODEKSZ(leaf) + xdata.mv_size > env->me_nodemax) {
					/* Too big for a sub-page, convert to sub-DB */
					inline_pair_ready = 0;
					fp_flags &= ~P_SUBP;
prep_subDB:
			if (mc->mc_db->md_flags & MDB_DUPFIXED) {
				fp_flags |= P_LEAF2;
				dummy.md_pad = fp->mp_pad;
				dummy.md_flags = MDB_DUPFIXED;
				if (mc->mc_db->md_flags & MDB_INTEGERDUP)
					dummy.md_flags |= MDB_INTEGERKEY;
			} else {
				dummy.md_pad = 0;
				dummy.md_flags = 0;
			}
			if (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION)
				dummy.md_flags |= MDB_PREFIX_COMPRESSION;
			dummy.md_depth = 1;
				dummy.md_branch_pages = 0;
					dummy.md_leaf_pages = 1;
					dummy.md_overflow_pages = 0;
					dummy.md_entries = NUMKEYS(fp);
					xdata.mv_size = sizeof(MDB_db);
					xdata.mv_data = &dummy;
					if ((rc = mdb_page_alloc(mc, 1, &mp)))
						return rc;
					offset = env->me_psize - olddata.mv_size;
					flags |= F_DUPDATA|F_SUBDATA;
					dummy.md_root = mp->mp_pgno;
					sub_root = mp;
			}
			if (mp != fp) {
				MP_FLAGS(mp) = fp_flags | P_DIRTY;
				MP_PAD(mp)   = MP_PAD(fp);
				MP_LOWER(mp) = MP_LOWER(fp);
				MP_UPPER(mp) = MP_UPPER(fp) + offset;
				if (fp_flags & P_LEAF2) {
					memcpy(METADATA(mp), METADATA(fp), NUMKEYS(fp) * fp->mp_pad);
				} else {
					memcpy((char *)mp + MP_UPPER(mp) + PAGEBASE, (char *)fp + MP_UPPER(fp) + PAGEBASE,
						olddata.mv_size - MP_UPPER(fp) - PAGEBASE);
					memcpy((char *)MP_PTRS(mp), (char *)MP_PTRS(fp), NUMKEYS(fp) * sizeof(mp->mp_ptrs[0]));
					for (i=0; i<NUMKEYS(fp); i++)
						mp->mp_ptrs[i] += offset;
				}
			}

			if (!force_subdb) {
				mc->mc_xcursor->mx_cursor.mc_pg[0] = mp;
				mc->mc_xcursor->mx_inline_bytes = xdata.mv_size;
			}

			rdata = &xdata;
			flags |= F_DUPDATA;
			if (!inline_pair_ready)
				do_sub = 1;
			if (!insert_key)
				mdb_node_del(mc, 0);
			goto new_sub;
		}
current:
		/* LMDB passes F_SUBDATA in 'flags' to write a DB record */
		if ((leaf->mn_flags ^ flags) & F_SUBDATA)
			return MDB_INCOMPATIBLE;
		/* overflow page overwrites need special handling */
		mp = mc->mc_pg[mc->mc_top];
		if (F_ISSET(leaf->mn_flags, F_BIGDATA)) {
			MDB_page *omp;
			pgno_t pg;
			int level, ovpages, dpages = OVPAGES(data->mv_size, env->me_psize);

			memcpy(&pg, olddata.mv_data, sizeof(pg));
			if ((rc2 = mdb_page_get(mc, pg, &omp, &level)) != 0)
				return rc2;
			ovpages = omp->mp_pages;

			/* Is the ov page large enough? */
			if (ovpages >= dpages) {
			  if (!(omp->mp_flags & P_DIRTY) &&
				  (level || (env->me_flags & MDB_WRITEMAP)))
			  {
				rc = mdb_page_unspill(mc->mc_txn, omp, &omp);
				if (rc)
					return rc;
				level = 0;		/* dirty in this txn or clean */
			  }
			  /* Is it dirty? */
			  if (omp->mp_flags & P_DIRTY) {
				/* yes, overwrite it. Note in this case we don't
				 * bother to try shrinking the page if the new data
				 * is smaller than the overflow threshold.
				 */
				if (level > 1) {
					/* It is writable only in a parent txn */
					size_t sz = (size_t) env->me_psize * ovpages, off;
					MDB_page *np = mdb_page_malloc(mc->mc_txn, ovpages);
					MDB_ID2 id2;
					if (!np)
						return ENOMEM;
					id2.mid = pg;
					id2.mptr = np;
					/* Note - this page is already counted in parent's dirty_room */
					rc2 = mdb_mid2l_insert(mc->mc_txn->mt_u.dirty_list, &id2);
					mdb_cassert(mc, rc2 == 0);
					/* Currently we make the page look as with put() in the
					 * parent txn, in case the user peeks at MDB_RESERVEd
					 * or unused parts. Some users treat ovpages specially.
					 */
					if (!(flags & MDB_RESERVE)) {
						/* Skip the part where LMDB will put *data.
						 * Copy end of page, adjusting alignment so
						 * compiler may copy words instead of bytes.
						 */
						off = (PAGEHDRSZ + data->mv_size) & -(int)sizeof(size_t);
						memcpy((size_t *)((char *)np + off),
							(size_t *)((char *)omp + off), sz - off);
						sz = PAGEHDRSZ;
					}
					memcpy(np, omp, sz); /* Copy beginning of page */
					omp = np;
				}
				SETDSZ(leaf, data->mv_size);
				if (F_ISSET(flags, MDB_RESERVE))
					data->mv_data = METADATA(omp);
				else
					memcpy(METADATA(omp), data->mv_data, data->mv_size);
				return MDB_SUCCESS;
			  }
			}
			if ((rc2 = mdb_ovpage_free(mc, omp)) != MDB_SUCCESS)
				return rc2;
		} else if (data->mv_size == olddata.mv_size) {
			/* same size, just replace it. Note that we could
			 * also reuse this node if the new data is smaller,
			 * but instead we opt to shrink the node in that case.
			 */
			if (F_ISSET(flags, MDB_RESERVE))
				data->mv_data = olddata.mv_data;
			else if (!(mc->mc_flags & C_SUB))
				memcpy(olddata.mv_data, data->mv_data, data->mv_size);
			else {
				if (key->mv_size != NODEKSZ(leaf))
					goto new_ksize;
				memcpy(NODEKEY(mp, leaf), key->mv_data, key->mv_size);
				goto fix_parent;
			}
			return MDB_SUCCESS;
		}
new_ksize:
		mdb_node_del(mc, 0);
	}

	rdata = data;

new_sub:
	nflags = flags & NODE_ADD_FLAGS;
		MDB_page *cur_pg = mc->mc_pg[mc->mc_top];
		int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;
		nsize = IS_LEAF2(cur_pg) ? key->mv_size :
		    mdb_leaf_size(env, cur_pg, mc->mc_ki[mc->mc_top], key, rdata, prefix_enabled);
	if (SIZELEFT(mc->mc_pg[mc->mc_top]) < nsize) {
		if (( flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA)
			nflags &= ~MDB_APPEND; /* sub-page may need room to grow */
		if (!insert_key)
			nflags |= MDB_SPLIT_REPLACE;
		rc = mdb_page_split(mc, key, rdata, P_INVALID, nflags);
		if (rc == MDB_SUCCESS)
			split_performed = 1;
	} else {
		/* There is room already in this leaf page. */
		rc = mdb_node_add(mc, mc->mc_ki[mc->mc_top], key, rdata, 0, nflags,
		    NULL, MDB_COUNT_HINT_NONE);
		if (rc == MDB_SUCCESS) {
			/* Adjust other cursors pointing to mp */
			MDB_cursor *m2, *m3;
			MDB_dbi dbi = mc->mc_dbi;
			unsigned i = mc->mc_top;
			MDB_page *mp = mc->mc_pg[i];

			for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
				if (mc->mc_flags & C_SUB)
					m3 = &m2->mc_xcursor->mx_cursor;
				else
					m3 = m2;
				if (m3 == mc || m3->mc_snum < mc->mc_snum || m3->mc_pg[i] != mp)
					continue;
				if (m3->mc_ki[i] >= mc->mc_ki[i] && insert_key)
					m3->mc_ki[i]++;
				XCURSOR_REFRESH(m3, i, mp);
			}
		} else if (rc == MDB_PAGE_FULL) {
			if ((flags & (F_DUPDATA|F_SUBDATA)) == F_DUPDATA)
				nflags &= ~MDB_APPEND;
			if (!insert_key)
				nflags |= MDB_SPLIT_REPLACE;
			rc = mdb_page_split(mc, key, rdata, P_INVALID, nflags);
			if (rc == MDB_SUCCESS)
				split_performed = 1;
		}
	}

finish_put:
	if (rc == MDB_SUCCESS) {
		if (inline_pair_ready && !do_sub && mc->mc_xcursor) {
			leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
				mdb_xcursor_init1(mc, leaf);
			if (mc->mc_db->md_flags & MDB_DUPSORT)
				insert_data = 1;
			{
				MDB_cursor *m2;
				unsigned i = mc->mc_top;
				MDB_page *mp = mc->mc_pg[i];
				MDB_xcursor *mx = mc->mc_xcursor;

				for (m2 = mc->mc_txn->mt_cursors[mc->mc_dbi]; m2; m2=m2->mc_next) {
					if (m2 == mc || m2->mc_snum < mc->mc_snum)
						continue;
					if (!(m2->mc_flags & C_INITIALIZED))
						continue;
					if (m2->mc_pg[i] != mp)
						continue;
					if (m2->mc_ki[i] == mc->mc_ki[i]) {
						mdb_xcursor_init2(m2, mx, 1);
					} else if (!insert_key) {
						XCURSOR_REFRESH(m2, i, mp);
					}
				}
			}
		}
		/* Now store the actual data in the child DB. Note that we're
		 * storing the user data in the keys field, so there are strict
		 * size limits on dupdata. The actual data fields of the child
		 * DB are all zero size.
		 */
		if (do_sub) {
			int xflags, new_dupdata;
			mdb_size_t ecount;
put_sub:
			xdata.mv_size = 0;
			xdata.mv_data = "";
			leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
			MDB_xcursor *mx = mc->mc_xcursor;
			if ((flags & (MDB_CURRENT|MDB_APPENDDUP)) == MDB_CURRENT) {
				xflags = MDB_CURRENT|MDB_NOSPILL;
			} else {
				mdb_xcursor_init1(mc, leaf);
				xflags = (flags & MDB_NODUPDATA) ?
					MDB_NOOVERWRITE|MDB_NOSPILL : MDB_NOSPILL;
			}
			if (sub_root)
				mc->mc_xcursor->mx_cursor.mc_pg[0] = sub_root;
			new_dupdata = (int)dkey.mv_size;
			/* converted, write the original data first */
			if (dkey.mv_size) {
				int skip_seed = mx && mx->mx_inline_measure_ready;
				if (!skip_seed) {
					rc = _mdb_cursor_put(&mc->mc_xcursor->mx_cursor, &dkey, &xdata, xflags);
					if (rc)
						goto bad_sub;
				}
				/* we've done our job */
				dkey.mv_size = 0;
			}
			if (!(leaf->mn_flags & F_SUBDATA) || sub_root) {
				/* Adjust other cursors pointing to mp */
				MDB_cursor *m2;
				unsigned i = mc->mc_top;
				MDB_page *mp = mc->mc_pg[i];

				for (m2 = mc->mc_txn->mt_cursors[mc->mc_dbi]; m2; m2=m2->mc_next) {
					if (m2 == mc || m2->mc_snum < mc->mc_snum) continue;
					if (!(m2->mc_flags & C_INITIALIZED)) continue;
					if (m2->mc_pg[i] == mp) {
						if (m2->mc_ki[i] == mc->mc_ki[i]) {
							mdb_xcursor_init2(m2, mx, new_dupdata);
						} else if (!insert_key) {
							XCURSOR_REFRESH(m2, i, mp);
						}
					}
				}
			}
			ecount = mc->mc_xcursor->mx_db.md_entries;
			if (flags & MDB_APPENDDUP)
				xflags |= MDB_APPEND;
			rc = _mdb_cursor_put(&mc->mc_xcursor->mx_cursor, data, &xdata, xflags);
			if (flags & F_SUBDATA) {
				void *db = NODEDATA(leaf);
				memcpy(db, &mc->mc_xcursor->mx_db, sizeof(MDB_db));
			}
			insert_data = mc->mc_xcursor->mx_db.md_entries - ecount;
		}
		/* Increment count unless we just replaced an existing item. */
		if ((mc->mc_db->md_flags & MDB_DUPSORT) && insert_key && insert_data <= 0)
			/* normalize MDB_NOTFOUND (or similar) from cursor_set */
			insert_data = 1;
		if (insert_data)
			mc->mc_db->md_entries++;
		if (insert_key) {
			/* Invalidate txn if we created an empty sub-DB */
			if (rc)
				goto bad_sub;
			/* If we succeeded and the key didn't exist before,
			 * make sure the cursor is marked valid.
			 */
			mc->mc_flags |= C_INITIALIZED;
		}
		if (flags & MDB_MULTIPLE) {
			if (!rc) {
				mcount++;
				/* let caller know how many succeeded, if any */
				data[1].mv_size = mcount;
				if (mcount < dcount) {
					data[0].mv_data = (char *)data[0].mv_data + data[0].mv_size;
					insert_key = insert_data = 0;
					goto more;
				}
			}
		}
bad_sub:
		if (rc == MDB_KEYEXIST)	/* should not happen, we deleted that item */
			rc = MDB_PROBLEM;
	}
	if (rc == MDB_SUCCESS) {
		if (mc->mc_db->md_flags & MDB_COUNTED) {
			int64_t value_delta;
			if (mc->mc_db->md_flags & MDB_DUPSORT)
				value_delta = (int64_t)insert_data;
			else
				value_delta = insert_key ? 1 : 0;
			if (value_delta || split_performed) {
				MDB_page *leaf_page = mc->mc_pg[mc->mc_top];
				mdb_adjust_counted_parents(mc, leaf_page, split_performed, value_delta);
			}
		}
		return rc;
	}
	mdb_txn_mark_error(mc->mc_txn, rc);
	return rc;
}

static int
mdb_prefix_inline_build_pair(MDB_cursor *mc, MDB_page *mp, size_t capacity,
	const MDB_val *first, const MDB_val *second)
{
	MDB_val items[2];
	MDB_cmp_func *dcmp;
	if (!mc || !mp || !first || !second)
		return EINVAL;
	if (capacity <= PAGEHDRSZ)
		return MDB_PAGE_FULL;

	memset(mp, 0, capacity);
	MP_FLAGS(mp) = P_LEAF|P_DIRTY|P_SUBP;
	MP_LOWER(mp) = (indx_t)(PAGEHDRSZ - PAGEBASE);
	MP_UPPER(mp) = (indx_t)(capacity - PAGEBASE);
	MP_PAD(mp) = 0;
	MP_SETPGNO(mp, MP_PGNO(mc->mc_pg[mc->mc_top]));
	items[0] = *first;
	items[1] = *second;

	if (mc->mc_db->md_flags & MDB_DUPFIXED) {
		if (items[0].mv_size != items[1].mv_size)
			return MDB_BAD_VALSIZE;
		MP_FLAGS(mp) |= P_LEAF2;
		mp->mp_pad = items[0].mv_size;
	}

	dcmp = mc->mc_dbx->md_dcmp;
	if (NEED_CMP_CLONG(dcmp, items[0].mv_size) ||
	    NEED_CMP_CLONG(dcmp, items[1].mv_size))
		dcmp = mdb_cmp_clong;
	if (dcmp(&items[1], &items[0]) < 0) {
		MDB_val swap = items[0];
		items[0] = items[1];
		items[1] = swap;
	}

	if (IS_LEAF2(mp)) {
		size_t ksize = items[0].mv_size;
		char *dest = LEAF2KEY(mp, 0, ksize);

		if (ksize) {
			if (items[0].mv_data)
				memcpy(dest, items[0].mv_data, ksize);
			else
				memset(dest, 0, ksize);
			if (items[1].mv_data)
				memcpy(dest + ksize, items[1].mv_data, ksize);
			else
				memset(dest + ksize, 0, ksize);
		}
		MP_LOWER(mp) = (indx_t)((PAGEHDRSZ - PAGEBASE) + 2 * sizeof(indx_t));
		{
			ssize_t upper = (ssize_t)(capacity - PAGEBASE);
			ssize_t delta = (ssize_t)ksize - (ssize_t)sizeof(indx_t);
			upper -= delta;
			upper -= delta;
			if (upper < 0)
				return MDB_PAGE_FULL;
			MP_UPPER(mp) = (indx_t)upper;
		}
		return MDB_SUCCESS;
	}

	int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;

	if (!prefix_enabled) {
		size_t lower = (PAGEHDRSZ - PAGEBASE) + 2 * sizeof(indx_t);
		size_t upper = capacity > PAGEBASE ? (capacity - PAGEBASE) : 0;

		if (lower > upper)
			return MDB_PAGE_FULL;

		MP_LOWER(mp) = (indx_t)lower;
		MP_UPPER(mp) = (indx_t)upper;

			for (unsigned int i = 0; i < 2; ++i) {
				size_t key_bytes = items[i].mv_size;
				size_t node_bytes = NODESIZE + EVEN(key_bytes);
			MDB_node *node;

			node_bytes = EVEN(node_bytes);
			if (node_bytes > upper)
				return MDB_PAGE_FULL;
			upper -= node_bytes;
			if (upper < (PAGEHDRSZ - PAGEBASE))
				return MDB_PAGE_FULL;

			MP_PTRS(mp)[i] = (indx_t)upper;
			node = NODEPTR(mp, i);
			node->mn_ksize = (unsigned short)key_bytes;
			node->mn_flags = 0;
			mdb_node_set_count(mp, node, 0);
			SETDSZ(node, 0);
			if (key_bytes)
				memcpy(NODEKEY(mp, node), items[i].mv_data, key_bytes);
		}

		MP_UPPER(mp) = (indx_t)upper;
	} else {
		MDB_prefix_rebuild_entry entries[2];
		memset(entries, 0, sizeof(entries));

		for (unsigned int i = 0; i < 2; ++i) {
			entries[i].key = items[i];
			entries[i].flags = 0;
			entries[i].data_size = 0;
			entries[i].data_payload = 0;
			entries[i].data_ptr = NULL;
			entries[i].encoded_ksize = 0;
			entries[i].encoded_key = NULL;
			entries[i].encoded_len = 0;
			entries[i].encoded_used = 0;
			entries[i].shared_prefix = UINT16_MAX;
		}

		int rc = mdb_leaf_rebuild_measure(mc->mc_txn->mt_env, mc->mc_txn,
		    entries, 2, capacity, NULL);
		if (rc != MDB_SUCCESS)
			return rc;

		rc = mdb_leaf_rebuild_apply(mc, mp, entries, 2, capacity);
		if (rc != MDB_SUCCESS)
			return rc;
	}

	return MDB_SUCCESS;
}

int
mdb_cursor_put(MDB_cursor *mc, MDB_val *key, MDB_val *data,
	unsigned int flags)
{
	DKBUF;
	DDBUF;
	int rc = _mdb_cursor_put(mc, key, data, flags);
	MDB_TRACE(("%p, %"Z"u[%s], %"Z"u%s, %u",
		mc, key ? key->mv_size:0, DKEY(key), data ? data->mv_size:0,
			data ? mdb_dval(mc->mc_txn, mc->mc_dbi, data, dbuf):"", flags));
	return rc;
}

static int
_mdb_cursor_del(MDB_cursor *mc, unsigned int flags)
{
	MDB_node	*leaf;
	MDB_page	*mp;
	int rc;

	if (mc->mc_txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (mc->mc_txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	if (!(mc->mc_flags & C_INITIALIZED))
		return EINVAL;

	if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
		return MDB_NOTFOUND;

	if (!(flags & MDB_NOSPILL) && (rc = mdb_page_spill(mc, NULL, NULL)))
		return rc;

	rc = mdb_cursor_touch(mc);
	if (rc)
		return rc;

	mp = mc->mc_pg[mc->mc_top];
	if (!IS_LEAF(mp))
		return MDB_CORRUPTED;
	if (IS_LEAF2(mp))
		goto del_key;
	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);

	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		if (flags & MDB_NODUPDATA) {
			/* mdb_cursor_del0() will subtract the final entry */
			mc->mc_db->md_entries -= mc->mc_xcursor->mx_db.md_entries - 1;
			mc->mc_xcursor->mx_cursor.mc_flags &= ~C_INITIALIZED;
		} else {
			mdb_size_t dup_before = 0;
			mdb_size_t dup_after = 0;
			int track_dup_delta = 0;
			if (mc->mc_db->md_flags & MDB_COUNTED) {
				dup_before = mc->mc_xcursor->mx_db.md_entries;
				track_dup_delta = 1;
			}
			if (!F_ISSET(leaf->mn_flags, F_SUBDATA)) {
				mc->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(leaf);
			}
			rc = _mdb_cursor_del(&mc->mc_xcursor->mx_cursor, MDB_NOSPILL);
			if (rc)
				return rc;
			if (track_dup_delta)
				dup_after = mc->mc_xcursor->mx_db.md_entries;
			/* If sub-DB still has entries, we're done */
			if (mc->mc_xcursor->mx_db.md_entries) {
				if (leaf->mn_flags & F_SUBDATA) {
					/* update subDB info */
					void *db = NODEDATA(leaf);
					memcpy(db, &mc->mc_xcursor->mx_db, sizeof(MDB_db));
				} else {
					MDB_cursor *m2;
					/* shrink fake page */
					if (!(mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION))
						mdb_node_shrink(mp, mc->mc_ki[mc->mc_top]);
					leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
					mc->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(leaf);
					/* fix other sub-DB cursors pointed at fake pages on this page */
					for (m2 = mc->mc_txn->mt_cursors[mc->mc_dbi]; m2; m2=m2->mc_next) {
						if (m2 == mc || m2->mc_snum < mc->mc_snum) continue;
						if (!(m2->mc_flags & C_INITIALIZED)) continue;
						if (m2->mc_pg[mc->mc_top] == mp) {
							XCURSOR_REFRESH(m2, mc->mc_top, mp);
						}
					}
				}
				mdb_size_t removed = 1;
				if (track_dup_delta && dup_before >= dup_after) {
					removed = dup_before - dup_after;
					if (!removed)
						removed = 1;
				}
				mc->mc_db->md_entries -= removed;
				if ((mc->mc_db->md_flags & MDB_COUNTED) && removed) {
					MDB_page *leaf_page = mc->mc_pg[mc->mc_top];
					mdb_adjust_counted_parents(mc, leaf_page, 0, - (int64_t)removed);
				}
				return rc;
			} else {
				mc->mc_xcursor->mx_cursor.mc_flags &= ~C_INITIALIZED;
			}
			/* otherwise fall thru and delete the sub-DB */
		}

		if (leaf->mn_flags & F_SUBDATA) {
			/* add all the child DB's pages to the free list */
			rc = mdb_drop0(&mc->mc_xcursor->mx_cursor, 0);
			if (rc)
				goto fail;
		}
	}
	/* LMDB passes F_SUBDATA in 'flags' to delete a DB record */
	else if ((leaf->mn_flags ^ flags) & F_SUBDATA) {
		rc = MDB_INCOMPATIBLE;
		goto fail;
	}

	/* add overflow pages to free list */
	if (F_ISSET(leaf->mn_flags, F_BIGDATA)) {
		MDB_page *omp;
		pgno_t pg;

		memcpy(&pg, NODEDATA(leaf), sizeof(pg));
		if ((rc = mdb_page_get(mc, pg, &omp, NULL)) ||
			(rc = mdb_ovpage_free(mc, omp)))
			goto fail;
	}

del_key:
	return mdb_cursor_del0(mc);

fail:
	mdb_txn_mark_error(mc->mc_txn, rc);
	return rc;
}

int
mdb_cursor_del(MDB_cursor *mc, unsigned int flags)
{
	MDB_TRACE(("%p, %u",
		mc, flags));
	return _mdb_cursor_del(mc, flags);
}

/** Allocate and initialize new pages for a database.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc a cursor on the database being added to.
 * @param[in] flags flags defining what type of page is being allocated.
 * @param[in] num the number of pages to allocate. This is usually 1,
 * unless allocating overflow pages for a large record.
 * @param[out] mp Address of a page, or NULL on failure.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_new(MDB_cursor *mc, uint32_t flags, int num, MDB_page **mp)
{
	MDB_page	*np;
	int rc;

	if ((rc = mdb_page_alloc(mc, num, &np)))
		return rc;
	DPRINTF(("allocated new mpage %"Yu", page size %u",
	    np->mp_pgno, mc->mc_txn->mt_env->me_psize));
	np->mp_flags = flags | P_DIRTY;
	if ((flags & P_BRANCH) && (mc->mc_db->md_flags & MDB_COUNTED))
		np->mp_flags |= P_COUNTED;
	np->mp_lower = (PAGEHDRSZ-PAGEBASE);
	np->mp_upper = mc->mc_txn->mt_env->me_psize - PAGEBASE;

	if (IS_BRANCH(np))
		mc->mc_db->md_branch_pages++;
	else if (IS_LEAF(np))
		mc->mc_db->md_leaf_pages++;
	else if (IS_OVERFLOW(np)) {
		mc->mc_db->md_overflow_pages += num;
		np->mp_pages = num;
	}
	*mp = np;

	return 0;
}

/** Calculate the size of a leaf node.
 * The size depends on the environment's page size; if a data item
 * is too large it will be put onto an overflow page and the node
 * size will only include the key and not the data. Sizes are always
 * rounded up to an even number of bytes, to guarantee 2-byte alignment
 * of the #MDB_node headers.
 * @param[in] env The environment handle.
 * @param[in] key The key for the node.
 * @param[in] data The data for the node.
 * @return The number of bytes needed to store the node.
 */
static size_t
mdb_leaf_size(MDB_env *env, MDB_page *mp, indx_t indx, MDB_val *key, MDB_val *data, int prefix_enabled)
{
	MDB_val trunk = {0, NULL};
	size_t key_bytes;
	size_t sz;

	if (prefix_enabled && mp && !IS_LEAF2(mp)) {
		indx_t total = NUMKEYS(mp);
		if (total > 0 && indx != 0) {
			MDB_node *tr = NODEPTR(mp, 0);
			trunk.mv_size = tr->mn_ksize;
			trunk.mv_data = NODEKEY(mp, tr);
		}
	}

	key_bytes = mdb_leaf_encoded_size(trunk.mv_data ? &trunk : NULL, key, NULL);
	sz = NODESIZE + EVEN(key_bytes) + data->mv_size;
	if (sz > env->me_nodemax) {
		/* put on overflow page */
		sz -= data->mv_size - sizeof(pgno_t);
	}

	return EVEN(sz + sizeof(indx_t));
}

/** Calculate the size of a branch node.
 * The size should depend on the environment's page size but since
 * we currently don't support spilling large keys onto overflow
 * pages, it's simply the size of the #MDB_node header plus the
 * size of the key. Sizes are always rounded up to an even number
 * of bytes, to guarantee 2-byte alignment of the #MDB_node headers.
 * @param[in] env The environment handle.
 * @param[in] key The key for the node.
 * @return The number of bytes needed to store the node.
 */
static size_t
mdb_branch_size(MDB_env *env, MDB_page *mp, MDB_val *key)
{
	size_t		 sz;

	sz = INDXSIZE(key);
	if (mp && IS_COUNTED(mp))
		sz += sizeof(uint64_t);
	if (sz > env->me_nodemax) {
		/* put on overflow page */
		/* not implemented */
		/* sz -= key->size - sizeof(pgno_t); */
	}

	return sz + sizeof(indx_t);
}

/** Add a node to the page pointed to by the cursor.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc The cursor for this operation.
 * @param[in] indx The index on the page where the new node should be added.
 * @param[in] key The key for the new node.
 * @param[in] data The data for the new node, if any.
 * @param[in] pgno The page number, if adding a branch node.
 * @param[in] flags Flags for the node.
 * @return 0 on success, non-zero on failure. Possible errors are:
 * <ul>
 *	<li>ENOMEM - failed to allocate overflow pages for the node.
 *	<li>MDB_PAGE_FULL - there is insufficient room in the page. This error
 *	should never happen since all callers already calculate the
 *	page's free space before calling this function.
 * </ul>
 */
static int
mdb_node_add(MDB_cursor *mc, indx_t indx,
	MDB_val *key, MDB_val *data, pgno_t pgno, unsigned int flags,
	MDB_page *child_hint, uint64_t count_hint)
{
	size_t		 node_size = NODESIZE;
	ssize_t		 room;
	MDB_node	*node;
	MDB_page	*mp = mc->mc_pg[mc->mc_top];
	MDB_page	*ofp = NULL;		/* overflow page */
	void		*ndata;
	size_t		 count_sz = (IS_BRANCH(mp) && IS_COUNTED(mp)) ? sizeof(uint64_t) : 0;
	MDB_page	*counted_child = child_hint;
	uint64_t	 child_count = count_hint;
	DKBUF;
	MDB_val	trunk = {0, NULL};
	size_t	trunk_shared = 0;
	int	trunk_shared_valid = 0;
	size_t	key_bytes = 0;
	size_t	key_pad = 0;
	unsigned char old_trunk_buf[MDB_KEYBUF_MAX];
	unsigned char decoded_trunk_buf[MDB_KEYBUF_MAX];
	MDB_val	old_trunk = {0, NULL};
	int	need_reencode = 0;
	int	prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;
	MDB_prefix_stride_entry *stride_entry = NULL;
	mdb_cassert(mc, MP_UPPER(mp) >= MP_LOWER(mp));

	DPRINTF(("add to %s %spage %"Yu" index %i, data size %"Z"u key size %"Z"u [%s]",
	    IS_LEAF(mp) ? "leaf" : "branch",
		IS_SUBP(mp) ? "sub-" : "",
		mdb_dbg_pgno(mp), indx, data ? data->mv_size : 0,
		key ? key->mv_size : 0, key ? DKEY(key) : "null"));

	if (IS_LEAF2(mp)) {
		/* Move higher keys up one slot. */
		int ksize = mc->mc_db->md_pad, dif;
		char *ptr = LEAF2KEY(mp, indx, ksize);
		dif = NUMKEYS(mp) - indx;
		if (dif > 0)
			memmove(ptr+ksize, ptr, dif*ksize);
		/* insert new key */
		memcpy(ptr, key->mv_data, ksize);

		/* Just using these for counting */
		MP_LOWER(mp) += sizeof(indx_t);
		MP_UPPER(mp) -= ksize - sizeof(indx_t);
		return MDB_SUCCESS;
	}

	room = (ssize_t)SIZELEFT(mp) - (ssize_t)sizeof(indx_t);
	node_size += count_sz;
	if (key != NULL) {
		if (IS_LEAF(mp)) {
			if (prefix_enabled) {
				indx_t total = NUMKEYS(mp);
				if (total > 0 && indx != 0) {
					MDB_node *tr = NODEPTR(mp, 0);
					trunk.mv_size = tr->mn_ksize;
					trunk.mv_data = NODEKEY(mp, tr);
					trunk_shared_valid = 1;
				} else {
					trunk_shared_valid = 0;
				}
			}
			if (prefix_enabled && trunk.mv_data)
				key_bytes = mdb_leaf_encoded_size(&trunk, key, &trunk_shared);
			else {
				key_bytes = mdb_leaf_encoded_size(NULL, key, NULL);
				trunk_shared_valid = 0;
			}
		} else {
			key_bytes = key->mv_size;
		}
		size_t key_bytes_aligned = EVEN(key_bytes);
		key_pad = key_bytes_aligned - key_bytes;
		node_size += key_bytes_aligned;
	} else {
		key_pad = 0;
	}
	if (prefix_enabled && IS_LEAF(mp) && !IS_LEAF2(mp) && indx == 0 && NUMKEYS(mp) > 0) {
		MDB_node *old = NODEPTR(mp, 0);
		MDB_val current_trunk = { old->mn_ksize, NODEKEY(mp, old) };

		if (key == NULL ||
		    current_trunk.mv_size != key->mv_size ||
		    memcmp(current_trunk.mv_data, key->mv_data, current_trunk.mv_size) != 0) {
			if (old->mn_ksize > MDB_KEYBUF_MAX)
				return MDB_BAD_VALSIZE;
			old_trunk.mv_size = old->mn_ksize;
			old_trunk.mv_data = old_trunk_buf;
			memcpy(old_trunk_buf, NODEKEY(mp, old), old_trunk.mv_size);
			if (old_trunk.mv_size > 0) {
				int trunk_decoded = 0;

				if (!trunk_decoded && (mc->mc_flags & C_SUB) &&
				    mc->mc_top > 0 && mc->mc_pg[mc->mc_top - 1]) {
					MDB_page *parent = mc->mc_pg[mc->mc_top - 1];
					indx_t parent_idx = mc->mc_ki[mc->mc_top - 1];
					if (IS_LEAF(parent) && parent_idx < NUMKEYS(parent)) {
						MDB_val parent_key = {0, NULL};
						int pkrc = mdb_cursor_read_key_at(mc, parent, parent_idx,
						    &parent_key);
						if (pkrc == MDB_SUCCESS && parent_key.mv_data &&
						    parent_key.mv_size <= MDB_KEYBUF_MAX) {
							memcpy(old_trunk_buf, parent_key.mv_data,
							    parent_key.mv_size);
							old_trunk.mv_size = parent_key.mv_size;
							trunk_decoded = 1;
						}
					}
				}

				if (!trunk_decoded) {
					MDB_val decoded_trunk = {0, decoded_trunk_buf};
					int krc = mdb_cursor_read_key_at(mc, mp, 0, &decoded_trunk);
					if (krc == MDB_SUCCESS && decoded_trunk.mv_data &&
					    decoded_trunk.mv_size <= MDB_KEYBUF_MAX) {
						memcpy(old_trunk_buf, decoded_trunk.mv_data,
						    decoded_trunk.mv_size);
						old_trunk.mv_size = decoded_trunk.mv_size;
						trunk_decoded = 1;
					}
				}
			}

			need_reencode = 1;
		}
	}
	if (IS_LEAF(mp)) {
		mdb_cassert(mc, key && data);
		if (F_ISSET(flags, F_BIGDATA)) {
			/* Data already on overflow page. */
			node_size += sizeof(pgno_t);
		} else if (node_size + data->mv_size > mc->mc_txn->mt_env->me_nodemax) {
			int inline_dup = F_ISSET(flags, F_DUPDATA) && !F_ISSET(flags, F_SUBDATA);

			if (inline_dup) {
				/* Inline duplicate sub-pages must never spill to overflow;
				 * the dupsort path will resize or promote them as needed.
				 */
				node_size += data->mv_size;
			} else {
				int ovpages = OVPAGES(data->mv_size, mc->mc_txn->mt_env->me_psize);
				int rc;
				/* Put data on overflow page. */
				DPRINTF(("data size is %"Z"u, node would be %"Z"u, put data on overflow page",
				    data->mv_size, node_size+data->mv_size));
				node_size = EVEN(node_size + sizeof(pgno_t));
				if ((ssize_t)node_size > room)
					goto full;
				if ((rc = mdb_page_new(mc, P_OVERFLOW, ovpages, &ofp)))
					return rc;
				DPRINTF(("allocated overflow page %"Yu, ofp->mp_pgno));
				flags |= F_BIGDATA;
				goto update;
			}
		} else {
			node_size += data->mv_size;
		}
	}
	node_size = EVEN(node_size);
	if (prefix_enabled && need_reencode) {
		int rc;
		MDB_env *env = mc->mc_txn->mt_env;
		MDB_node *replaced = NULL;

		/* Stage inline subpage into scratch so rebuild reads stable bytes. */
		if (F_ISSET(flags, MDB_SPLIT_REPLACE) &&
		    F_ISSET(flags, F_DUPDATA) && !F_ISSET(flags, F_SUBDATA) &&
		    !F_ISSET(flags, F_BIGDATA) && data && data->mv_size) {
			MDB_page *scratch = env->me_pbuf;
			mdb_cassert(mc, scratch != NULL);
			mdb_cassert(mc, data->mv_size <= env->me_psize);
			if (data->mv_data != scratch)
				memcpy(scratch, data->mv_data, data->mv_size);
			MP_SETPGNO(scratch, MP_PGNO(mp));
			MP_FLAGS(scratch) |= P_DIRTY;
			data->mv_data = scratch;
		}

		if (F_ISSET(flags, MDB_SPLIT_REPLACE) && indx < NUMKEYS(mp)) {
			size_t node_bytes;
			replaced = NODEPTR(mp, indx);
			/* Remove the old node so rebuild treats this as a fresh insert. */
			if (F_ISSET(replaced->mn_flags, F_BIGDATA)) {
				MDB_page *omp;
				pgno_t pg;
				memcpy(&pg, NODEDATA(replaced), sizeof(pg));
				rc = mdb_page_get(mc, pg, &omp, NULL);
				if (rc != MDB_SUCCESS)
					return rc;
				rc = mdb_ovpage_free(mc, omp);
				if (rc != MDB_SUCCESS)
					return rc;
			}
			node_bytes = NODESIZE + EVEN(replaced->mn_ksize);
			if (IS_BRANCH(mp) && IS_COUNTED(mp))
				node_bytes += sizeof(uint64_t);
			if (IS_LEAF(mp)) {
				if (F_ISSET(replaced->mn_flags, F_BIGDATA))
					node_bytes += sizeof(pgno_t);
				else
					node_bytes += NODEDSZ(replaced);
			}
			node_bytes = EVEN(node_bytes);
			mdb_page_remove_slot(mc, mp, indx, node_bytes);
			mc->mc_ki[mc->mc_top] = indx;
		}

		if (ofp != NULL) {
			unsigned char *odata = METADATA(ofp);
			if (F_ISSET(flags, MDB_RESERVE))
				data->mv_data = odata;
			else
				memcpy(odata, data->mv_data, data->mv_size);
		}

		rc = mdb_leaf_rebuild_after_trunk_insert(mc, mp, &old_trunk, indx,
		    key, data, flags, ofp);
		if (rc != MDB_SUCCESS) {
			if (ofp != NULL)
				mdb_ovpage_free(mc, ofp);
			return rc;
		}

		if (F_ISSET(flags, MDB_RESERVE)) {
			if (F_ISSET(flags, F_BIGDATA)) {
				if (ofp != NULL)
					data->mv_data = METADATA(ofp);
			} else {
				MDB_node *res = NODEPTR(mp, indx);
				data->mv_data = NODEDATA(res);
			}
		}

		return MDB_SUCCESS;
	}
	if ((ssize_t)node_size > room)
		goto full;

	if (prefix_enabled && !need_reencode && IS_LEAF(mp) && !IS_LEAF2(mp)) {
		int prc = mdb_prefix_stride_prepare(mc, mp, &stride_entry);
		if (prc != MDB_SUCCESS)
			return prc;
	}

update:
	mdb_page_insert_slot(mc, mp, indx, node_size);

	/* Write the node data. */
	node = NODEPTR(mp, indx);
	node->mn_ksize = (key == NULL) ? 0 : key_bytes;
	node->mn_flags = flags;
	if (IS_LEAF(mp))
		SETDSZ(node,data->mv_size);
	else
		SETPGNO(node,pgno);

	mdb_node_set_count(mp, node, 0);
	if (key) {
		if (IS_LEAF(mp)) {
			size_t wrote;
			if (prefix_enabled && trunk_shared_valid)
				wrote = mdb_leaf_encode_key_fast(key, trunk_shared,
				    NODEKEY(mp, node));
			else
				wrote = mdb_leaf_encode_key(prefix_enabled ? &trunk : NULL,
				    key, NODEKEY(mp, node), NULL);
			mdb_cassert(mc, wrote == key_bytes);
		} else {
			memcpy(NODEKEY(mp, node), key->mv_data, key->mv_size);
		}
		if (key_pad) {
			unsigned char *pad = (unsigned char *)NODEKEY(mp, node);
			memset(pad + key_bytes, 0, key_pad);
		}
	}

	if (IS_LEAF(mp)) {
		ndata = NODEDATA(node);
		if (ofp == NULL) {
			if (F_ISSET(flags, F_BIGDATA))
				memcpy(ndata, data->mv_data, sizeof(pgno_t));
			else if (F_ISSET(flags, MDB_RESERVE))
				data->mv_data = ndata;
			else
				memcpy(ndata, data->mv_data, data->mv_size);
		} else {
			memcpy(ndata, &ofp->mp_pgno, sizeof(pgno_t));
			ndata = METADATA(ofp);
			if (F_ISSET(flags, MDB_RESERVE))
				data->mv_data = ndata;
			else
				memcpy(ndata, data->mv_data, data->mv_size);
		}
	}

	if (IS_BRANCH(mp) && IS_COUNTED(mp)) {
		if (child_count == MDB_COUNT_HINT_NONE) {
			if (!counted_child && pgno != 0)
				mdb_page_get(mc, pgno, &counted_child, NULL);
			if (counted_child)
				child_count = mdb_page_subtree_count(counted_child);
			else
				child_count = 0;
		}
		mdb_node_set_count(mp, node, child_count);
	}

	if (IS_LEAF(mp) && !IS_LEAF2(mp)) {
		if (prefix_enabled) {
			if (stride_entry) {
				int irc = mdb_prefix_stride_apply_insert(stride_entry, mc, mp,
				    indx, key ? key->mv_size : 0);
				if (irc != MDB_SUCCESS)
					return irc;
			} else if (key && key->mv_data) {
				size_t cached = MP_PAD(mp);
				if (!cached) {
					if (NUMKEYS(mp) == 1)
						mdb_prefix_leaf_store_stride(mc, mp, key->mv_size);
					else
						mdb_prefix_leaf_refresh_stride(mc, mp);
				} else if (key->mv_size > cached) {
					mdb_prefix_leaf_store_stride(mc, mp, key->mv_size);
				}
			}
		} else {
			mdb_prefix_leaf_store_stride(mc, mp, 0);
		}
	}

	return MDB_SUCCESS;

full:
	DPRINTF(("not enough room in page %"Yu", got %u ptrs",
		mdb_dbg_pgno(mp), NUMKEYS(mp)));
	DPRINTF(("upper-lower = %u - %u = %"Z"d", MP_UPPER(mp),MP_LOWER(mp),room));
	DPRINTF(("node size = %"Z"u", node_size));
	mdb_txn_mark_error(mc->mc_txn, MDB_PAGE_FULL);
	return MDB_PAGE_FULL;
}

/** Delete the specified node from a page.
 * @param[in] mc Cursor pointing to the node to delete.
 * @param[in] ksize The size of a node. Only used if the page is
 * part of a #MDB_DUPFIXED database.
 */
static void
mdb_node_del(MDB_cursor *mc, int ksize)
{
	MDB_page *mp = mc->mc_pg[mc->mc_top];
	indx_t	indx = mc->mc_ki[mc->mc_top];
	unsigned int	 sz;
	indx_t		 numkeys;
	MDB_node	*node;
	unsigned char old_trunk_buf[MDB_KEYBUF_MAX];
	MDB_val old_trunk = {0, NULL};
	int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;
	MDB_prefix_stride_entry *stride_entry = NULL;

	DPRINTF(("delete node %u on %s page %"Yu, indx,
	    IS_LEAF(mp) ? "leaf" : "branch", mdb_dbg_pgno(mp)));
	numkeys = NUMKEYS(mp);
	mdb_cassert(mc, indx < numkeys);
	if (prefix_enabled && IS_LEAF(mp) && !IS_LEAF2(mp) && indx == 0 && numkeys > 0) {
		int rc;
		MDB_node *first = NODEPTR(mp, 0);
		mdb_cassert(mc, first->mn_ksize <= MDB_KEYBUF_MAX);
		old_trunk.mv_size = first->mn_ksize;
		old_trunk.mv_data = old_trunk_buf;
		memcpy(old_trunk_buf, NODEKEY(mp, first), old_trunk.mv_size);
		rc = mdb_leaf_rebuild_after_trunk_delete(mc, mp, indx, &old_trunk);
		if (rc != MDB_SUCCESS)
			mdb_txn_mark_error(mc->mc_txn, rc);
		return;
	}

	if (prefix_enabled && IS_LEAF(mp) && !IS_LEAF2(mp)) {
		if (mdb_prefix_stride_prepare(mc, mp, &stride_entry) != MDB_SUCCESS)
			stride_entry = NULL;
	}

	if (IS_LEAF2(mp)) {
		int x = numkeys - 1 - indx;
		char *base = LEAF2KEY(mp, indx, ksize);
		if (x)
			memmove(base, base + ksize, x * ksize);
		MP_LOWER(mp) -= sizeof(indx_t);
		MP_UPPER(mp) += ksize - sizeof(indx_t);
		return;
	}

	node = NODEPTR(mp, indx);
	sz = NODESIZE + EVEN(node->mn_ksize);
	if (IS_BRANCH(mp) && IS_COUNTED(mp))
		sz += sizeof(uint64_t);
	if (IS_LEAF(mp)) {
		if (F_ISSET(node->mn_flags, F_BIGDATA))
			sz += sizeof(pgno_t);
		else
			sz += NODEDSZ(node);
	}
	sz = EVEN(sz);

	if (stride_entry)
		mdb_prefix_stride_apply_delete(stride_entry, mc, mp, indx);

	mdb_page_remove_slot(mc, mp, indx, sz);

	if (IS_LEAF(mp) && !IS_LEAF2(mp)) {
		if (!prefix_enabled) {
			mdb_prefix_leaf_store_stride(mc, mp, 0);
		} else if (!stride_entry) {
			if (!NUMKEYS(mp))
				mdb_prefix_leaf_store_stride(mc, mp, 0);
			else
				mdb_prefix_leaf_refresh_stride(mc, mp);
		}
	}
}

/** Compact the main page after deleting a node on a subpage.
 * @param[in] mp The main page to operate on.
 * @param[in] indx The index of the subpage on the main page.
 */
static void
mdb_node_shrink(MDB_page *mp, indx_t indx)
{
	MDB_node *node;
	MDB_page *sp, *xp;
	char *base;
	indx_t delta, nsize, len, ptr;
	int i;

	node = NODEPTR(mp, indx);
	sp = (MDB_page *)NODEDATA(node);
	delta = SIZELEFT(sp);
	if (delta >= NODEDSZ(node)) {
		/* nothing to trim or subpage already minimal */
		return;
	}
	nsize = NODEDSZ(node) - delta;
	if (nsize < PAGEHDRSZ) {
		delta = NODEDSZ(node) - PAGEHDRSZ;
		if ((int)delta <= 0)
			return;
		nsize = PAGEHDRSZ;
	}

	/* Prepare to shift upward, set len = length(subpage part to shift) */
	if (IS_LEAF2(sp)) {
		len = nsize;
		if (nsize & 1)
			return;		/* do not make the node uneven-sized */
	} else {
		xp = (MDB_page *)((char *)sp + delta); /* destination subpage */
		for (i = NUMKEYS(sp); --i >= 0; )
			MP_PTRS(xp)[i] = MP_PTRS(sp)[i] - delta;
		len = PAGEHDRSZ;
	}
	MP_UPPER(sp) = MP_LOWER(sp);
	MP_SETPGNO(sp, MP_PGNO(mp));
	SETDSZ(node, nsize);

	/* Shift <lower nodes...initial part of subpage> upward */
	base = (char *)mp + mp->mp_upper + PAGEBASE;
	memmove(base + delta, base, (char *)sp + len - base);

	ptr = mp->mp_ptrs[indx];
	for (i = NUMKEYS(mp); --i >= 0; ) {
		if (mp->mp_ptrs[i] <= ptr)
			mp->mp_ptrs[i] += delta;
	}
	mp->mp_upper += delta;
}

/** Initial setup of a sorted-dups cursor.
 * Sorted duplicates are implemented as a sub-database for the given key.
 * The duplicate data items are actually keys of the sub-database.
 * Operations on the duplicate data items are performed using a sub-cursor
 * initialized when the sub-database is first accessed. This function does
 * the preliminary setup of the sub-cursor, filling in the fields that
 * depend only on the parent DB.
 * @param[in] mc The main cursor whose sorted-dups cursor is to be initialized.
 */
static void
mdb_xcursor_init0(MDB_cursor *mc)
{
	MDB_xcursor *mx = mc->mc_xcursor;

	mx->mx_cursor.mc_xcursor = NULL;
	mx->mx_cursor.mc_txn = mc->mc_txn;
	mx->mx_cursor.mc_db = &mx->mx_db;
	mx->mx_cursor.mc_dbx = &mx->mx_dbx;
	mx->mx_cursor.mc_dbi = mc->mc_dbi;
	mx->mx_cursor.mc_dbflag = &mx->mx_dbflag;
	mx->mx_cursor.mc_snum = 0;
	mx->mx_cursor.mc_top = 0;
	mx->mx_cursor.mc_key.mv_data = mx->mx_cursor.mc_keybuf;
	mx->mx_cursor.mc_key.mv_size = 0;
	mx->mx_cursor.mc_key_pgno = P_INVALID;
	mx->mx_cursor.mc_key_last = (indx_t)~0;
	mdb_cursor_leaf_cache_reset(&mx->mx_cursor.mc_leaf_cache);
	MC_SET_OVPG(&mx->mx_cursor, NULL);
	mx->mx_cursor.mc_flags = C_SUB | (mc->mc_flags & (C_ORIG_RDONLY|C_WRITEMAP|C_LEAFCACHE));
	mx->mx_cursor.mc_flags |= C_LEAFCACHE;
	mx->mx_inline_bytes = 0;
	mx->mx_inline_required = 0;
	mx->mx_inline_measure_ready = 0;
	mx->mx_dbx.md_name.mv_size = 0;
	mx->mx_dbx.md_name.mv_data = NULL;
	mx->mx_dbx.md_cmp = mc->mc_dbx->md_dcmp;
	mx->mx_dbx.md_dcmp = NULL;
	mx->mx_dbx.md_rel = mc->mc_dbx->md_rel;
}

/** Final setup of a sorted-dups cursor.
 *	Sets up the fields that depend on the data from the main cursor.
 * @param[in] mc The main cursor whose sorted-dups cursor is to be initialized.
 * @param[in] node The data containing the #MDB_db record for the
 * sorted-dup database.
 */
static void
mdb_xcursor_init1(MDB_cursor *mc, MDB_node *node)
{
	MDB_xcursor *mx = mc->mc_xcursor;
	MDB_cursor_leaf_cache *cache = &mx->mx_cursor.mc_leaf_cache;
	int keep_inline_cache = 0;

	if (!(node->mn_flags & F_SUBDATA) &&
	    (mx->mx_cursor.mc_flags & C_INITIALIZED) &&
	    mx->mx_cursor.mc_pg[0]) {
		MDB_page *new_fp = NODEDATA(node);
		if (mx->mx_cursor.mc_pg[0] == new_fp &&
		    cache->decoded_pgno == new_fp->mp_pgno &&
		    cache->decoded_gen == mc->mc_txn->mt_txnid &&
		    cache->decoded_count == NUMKEYS(new_fp)) {
			keep_inline_cache = 1;
		}
	}

	if (!keep_inline_cache)
		mdb_cursor_leaf_cache_reset(cache);

	mx->mx_cursor.mc_flags &= C_SUB|C_ORIG_RDONLY|C_WRITEMAP|C_LEAFCACHE;
	if (node->mn_flags & F_SUBDATA) {
		memcpy(&mx->mx_db, NODEDATA(node), sizeof(MDB_db));
		mx->mx_cursor.mc_pg[0] = 0;
		mx->mx_cursor.mc_snum = 0;
		mx->mx_cursor.mc_top = 0;
		mx->mx_inline_bytes = 0;
		mx->mx_inline_required = 0;
		mx->mx_inline_measure_ready = 0;
		if (mc->mc_db->md_flags & MDB_COUNTED)
			mx->mx_db.md_flags |= MDB_COUNTED;
	} else {
		MDB_page *fp = NODEDATA(node);
		mx->mx_inline_bytes = NODEDSZ(node);
		mx->mx_db.md_pad = 0;
		mx->mx_db.md_flags = 0;
		mx->mx_db.md_depth = 1;
		mx->mx_db.md_branch_pages = 0;
		mx->mx_db.md_leaf_pages = 1;
		mx->mx_db.md_overflow_pages = 0;
		mx->mx_db.md_entries = NUMKEYS(fp);
			mx->mx_db.md_root = MP_PGNO(fp);
		mx->mx_cursor.mc_snum = 1;
		mx->mx_cursor.mc_top = 0;
		mx->mx_cursor.mc_flags |= C_INITIALIZED;
		mx->mx_cursor.mc_pg[0] = fp;
		mx->mx_cursor.mc_ki[0] = 0;
		if (mc->mc_db->md_flags & MDB_DUPFIXED) {
			mx->mx_db.md_flags = MDB_DUPFIXED;
			mx->mx_db.md_pad = fp->mp_pad;
			if (mc->mc_db->md_flags & MDB_INTEGERDUP)
				mx->mx_db.md_flags |= MDB_INTEGERKEY;
		}
		if (mc->mc_db->md_flags & MDB_COUNTED)
			mx->mx_db.md_flags |= MDB_COUNTED;
	}
	if (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION)
		mx->mx_db.md_flags |= MDB_PREFIX_COMPRESSION;
	DPRINTF(("Sub-db -%u root page %"Yu, mx->mx_cursor.mc_dbi,
		mx->mx_db.md_root));
	mx->mx_dbflag = DB_VALID|DB_USRVALID|DB_DUPDATA;
	if (NEED_CMP_CLONG(mx->mx_dbx.md_cmp, mx->mx_db.md_pad))
		mx->mx_dbx.md_cmp = mdb_cmp_clong;
}


/** Fixup a sorted-dups cursor due to underlying update.
 *	Sets up some fields that depend on the data from the main cursor.
 *	Almost the same as init1, but skips initialization steps if the
 *	xcursor had already been used.
 * @param[in] mc The main cursor whose sorted-dups cursor is to be fixed up.
 * @param[in] src_mx The xcursor of an up-to-date cursor.
 * @param[in] new_dupdata True if converting from a non-#F_DUPDATA item.
 */
static void
mdb_xcursor_init2(MDB_cursor *mc, MDB_xcursor *src_mx, int new_dupdata)
{
	MDB_xcursor *mx = mc->mc_xcursor;
	MDB_cursor_leaf_cache *dst_cache = &mx->mx_cursor.mc_leaf_cache;
	int clone_ok = 0;

	if (!new_dupdata && src_mx &&
	    (src_mx->mx_cursor.mc_flags & C_INITIALIZED)) {
		const MDB_cursor_leaf_cache *src_cache =
			&src_mx->mx_cursor.mc_leaf_cache;
		if (mdb_cursor_leaf_cache_clone(dst_cache, src_cache, mc->mc_txn) ==
		    MDB_SUCCESS)
			clone_ok = 1;
	}

	if (!clone_ok)
		mdb_cursor_leaf_cache_reset(dst_cache);
	if (new_dupdata) {
		mx->mx_cursor.mc_snum = 1;
		mx->mx_cursor.mc_top = 0;
		mx->mx_cursor.mc_flags |= C_INITIALIZED;
		mx->mx_cursor.mc_ki[0] = 0;
		mx->mx_dbflag = DB_VALID|DB_USRVALID|DB_DUPDATA;
#if UINT_MAX < MDB_SIZE_MAX	/* matches mdb_xcursor_init1:NEED_CMP_CLONG() */
		mx->mx_dbx.md_cmp = src_mx->mx_dbx.md_cmp;
#endif
	} else if (!(mx->mx_cursor.mc_flags & C_INITIALIZED)) {
		return;
	}
	mx->mx_db = src_mx->mx_db;
	mx->mx_cursor.mc_pg[0] = src_mx->mx_cursor.mc_pg[0];
	mx->mx_inline_bytes = src_mx->mx_inline_bytes;
	mx->mx_inline_required = 0;
	mx->mx_inline_measure_ready = 0;
	DPRINTF(("Sub-db -%u root page %"Yu, mx->mx_cursor.mc_dbi,
		mx->mx_db.md_root));
}

/** Initialize a cursor for a given transaction and database. */
static void
mdb_cursor_init(MDB_cursor *mc, MDB_txn *txn, MDB_dbi dbi, MDB_xcursor *mx)
{
	unsigned int keep = mc->mc_flags & C_LEAFCACHE;
	mc->mc_next = NULL;
	mc->mc_backup = NULL;
	mc->mc_dbi = dbi;
	mc->mc_txn = txn;
	mc->mc_db = &txn->mt_dbs[dbi];
	mc->mc_dbx = &txn->mt_dbxs[dbi];
	mc->mc_dbflag = &txn->mt_dbflags[dbi];
	mc->mc_snum = 0;
	mc->mc_top = 0;
	mc->mc_pg[0] = 0;
	mc->mc_ki[0] = 0;
	MC_SET_OVPG(mc, NULL);
	mc->mc_flags = (txn->mt_flags & (C_ORIG_RDONLY|C_WRITEMAP)) | keep;
	mc->mc_key.mv_data = mc->mc_keybuf;
	mc->mc_key.mv_size = 0;
	mc->mc_key_pgno = P_INVALID;
	mc->mc_key_last = (indx_t)~0;
	mdb_cursor_seq_invalidate(mc);
	memset(mc->mc_rank_base, 0, sizeof(mc->mc_rank_base));
	mdb_cursor_leaf_cache_reset(&mc->mc_leaf_cache);
	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT) {
		mdb_tassert(txn, mx != NULL);
		mc->mc_xcursor = mx;
		mdb_xcursor_init0(mc);
	} else if (mx) {
		mc->mc_xcursor = mx;
		mdb_xcursor_init0(mc);
	} else {
		mc->mc_xcursor = NULL;
	}
	if (*mc->mc_dbflag & DB_STALE) {
		mdb_page_search(mc, NULL, MDB_PS_ROOTONLY);
	}
}

int
mdb_cursor_open(MDB_txn *txn, MDB_dbi dbi, MDB_cursor **ret)
{
	MDB_cursor	*mc;
	size_t size = sizeof(MDB_cursor) + sizeof(MDB_xcursor);

	if (!ret || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (dbi == FREE_DBI && !F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
		return EINVAL;

	if ((mc = calloc(1, size)) != NULL) {
		mdb_cursor_init(mc, txn, dbi, (MDB_xcursor *)(mc + 1));
		mc->mc_flags |= C_LEAFCACHE;
		if (txn->mt_cursors) {
			mc->mc_next = txn->mt_cursors[dbi];
			txn->mt_cursors[dbi] = mc;
			mc->mc_flags |= C_UNTRACK;
		}
	} else {
		return ENOMEM;
	}

	MDB_TRACE(("%p, %u = %p", txn, dbi, mc));
	*ret = mc;

	return MDB_SUCCESS;
}

int
mdb_cursor_renew(MDB_txn *txn, MDB_cursor *mc)
{
	if (!mc || !TXN_DBI_EXIST(txn, mc->mc_dbi, DB_VALID))
		return EINVAL;

	if ((mc->mc_flags & C_UNTRACK) || txn->mt_cursors)
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	mdb_cursor_init(mc, txn, mc->mc_dbi, mc->mc_xcursor);
	return MDB_SUCCESS;
}

/* Return the count of duplicate data items for the current key */
int
mdb_cursor_count(MDB_cursor *mc, mdb_size_t *countp)
{
	MDB_node	*leaf;

	if (mc == NULL || countp == NULL)
		return EINVAL;

	if (mc->mc_xcursor == NULL)
		return MDB_INCOMPATIBLE;

	if (!(mc->mc_db->md_flags & MDB_DUPSORT))
		return MDB_INCOMPATIBLE;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (!(mc->mc_flags & C_INITIALIZED))
		return EINVAL;

	if (!mc->mc_snum)
		return MDB_NOTFOUND;

	if (mc->mc_flags & C_EOF) {
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
			return MDB_NOTFOUND;
		mc->mc_flags ^= C_EOF;
	}

	leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
	if (!F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		*countp = 1;
	} else {
		if (!(mc->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED))
			return EINVAL;

		*countp = mc->mc_xcursor->mx_db.md_entries;
	}
	return MDB_SUCCESS;
}

static int
mdb_cursor_list_dup_leaf2(MDB_cursor *mc, MDB_page *mp)
{
	MDB_cursor_leaf_cache *cache = &mc->mc_leaf_cache;
	unsigned int total = cache->decoded_count;
	unsigned int ksize = mp->mp_pad;

	for (unsigned int i = 0; i < total; ++i) {
		cache->decoded_vals[i].mv_size = ksize;
		cache->decoded_vals[i].mv_data = LEAF2KEY(mp, i, ksize);
		if (cache->decoded_ready)
			cache->decoded_ready[i] = 1;
	}
	return MDB_SUCCESS;
}

static int
mdb_cursor_list_dup_regular(MDB_cursor *mc, MDB_page *mp)
{
	MDB_cursor_leaf_cache *cache = &mc->mc_leaf_cache;
	unsigned int total = cache->decoded_count;
	int rc;

	if (!total)
		return MDB_SUCCESS;

	rc = mdb_cursor_leaf_cache_materialize(mc, mp);
	if (rc != MDB_SUCCESS)
		return rc;

	for (unsigned int i = 0; i < total; ++i) {
		if (cache->decoded_ready && cache->decoded_ready[i])
			continue;
		MDB_node *node = NODEPTR(mp, i);
		const unsigned char *encoded = NODEKEY(mp, node);
		rc = mdb_leaf_decode_key(&cache->decoded_vals[0], encoded,
		    node->mn_ksize, &cache->decoded_vals[i],
		    cache->decoded_vals[i].mv_data, cache->decoded_stride, 0, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
		if (cache->decoded_ready)
			cache->decoded_ready[i] = 1;
	}
	return MDB_SUCCESS;
}

/** Collect all dup values from a sub-database (F_SUBDATA) by traversing
 *  the nested B-tree.  For LEAF2 pages the returned MDB_val pointers
 *  reference the memory-mapped pages directly.  For regular (possibly
 *  prefix-compressed) pages the values are copied into the leaf cache
 *  buffer so that they remain valid after cursor movement. */
static int
mdb_cursor_list_dup_subdb(MDB_cursor *mc, const MDB_val **values,
	mdb_size_t *countp)
{
	MDB_cursor *mx = &mc->mc_xcursor->mx_cursor;
	mdb_size_t total = mc->mc_xcursor->mx_db.md_entries;
	MDB_cursor_leaf_cache *cache = &mx->mc_leaf_cache;
	MDB_val val;
	int rc;
	mdb_size_t idx;
	int is_leaf2;

	/* Save nested cursor state so we can restore after traversal. */
	indx_t   ki_save[CURSOR_STACK];
	MDB_page *pg_save[CURSOR_STACK];
	unsigned int top_save   = mx->mc_top;
	unsigned int snum_save  = mx->mc_snum;
	unsigned int flags_save = mx->mc_flags;
	unsigned int j;

	if (total == 0) {
		*countp = 0;
		*values = NULL;
		return MDB_SUCCESS;
	}

	for (j = 0; j < snum_save && j < CURSOR_STACK; j++) {
		ki_save[j] = mx->mc_ki[j];
		pg_save[j] = mx->mc_pg[j];
	}

	mdb_cursor_leaf_cache_reset(cache);
	rc = mdb_cursor_leaf_cache_ensure_vals(cache, (unsigned int)total);
	if (rc != MDB_SUCCESS)
		goto restore;

	/* Position at the first entry of the sub-database. */
	rc = mdb_cursor_first(mx, &val, NULL);
	if (rc != MDB_SUCCESS)
		goto restore;

	is_leaf2 = IS_LEAF2(mx->mc_pg[mx->mc_top]);

	if (is_leaf2) {
		/* LEAF2 / DUPFIXED: data resides directly in memory-mapped
		 * pages, so the pointers are stable for the txn lifetime. */
		cache->decoded_vals[0] = val;
		for (idx = 1; idx < total; idx++) {
			rc = mdb_cursor_next(mx, &val, NULL, MDB_NEXT);
			if (rc != MDB_SUCCESS) {
				if (rc == MDB_NOTFOUND)
					break;
				goto restore;
			}
			cache->decoded_vals[idx] = val;
		}
	} else {
		/* Non-LEAF2: values may be prefix-decoded into temporary
		 * buffers, so copy every value into stable cache storage. */
		size_t buf_used = 0;

		rc = mdb_cursor_leaf_cache_reserve_buf(cache, total * 64);
		if (rc != MDB_SUCCESS)
			goto restore;

		for (idx = 0; ; ) {
			size_t need = buf_used + val.mv_size;
			if (need > cache->decoded_buf_size) {
				rc = mdb_cursor_leaf_cache_reserve_buf(cache, need);
				if (rc != MDB_SUCCESS)
					goto restore;
			}
			memcpy(cache->decoded_buf + buf_used, val.mv_data,
				val.mv_size);
			cache->decoded_vals[idx].mv_size = val.mv_size;
			buf_used += val.mv_size;
			idx++;

			if (idx >= total)
				break;
			rc = mdb_cursor_next(mx, &val, NULL, MDB_NEXT);
			if (rc != MDB_SUCCESS) {
				if (rc == MDB_NOTFOUND)
					break;
				goto restore;
			}
		}

		/* The buffer address is now final (no more reserve_buf calls).
		 * Convert accumulated sizes into data pointers. */
		{
			size_t offset = 0;
			mdb_size_t k;
			for (k = 0; k < idx; k++) {
				cache->decoded_vals[k].mv_data =
					cache->decoded_buf + offset;
				offset += cache->decoded_vals[k].mv_size;
			}
		}
	}

	cache->decoded_count = (unsigned int)idx;
	*countp = idx;
	*values = cache->decoded_vals;
	rc = MDB_SUCCESS;

restore:
	for (j = 0; j < snum_save && j < CURSOR_STACK; j++) {
		mx->mc_ki[j] = ki_save[j];
		mx->mc_pg[j] = pg_save[j];
	}
	mx->mc_top  = top_save;
	mx->mc_snum = snum_save;
	mx->mc_flags = flags_save;

	return rc;
}

int
mdb_cursor_list_dup(MDB_cursor *mc, const MDB_val **values, mdb_size_t *countp)
{
	MDB_node *leaf;
	MDB_cursor *mx;
	MDB_page *dp;
	MDB_cursor_leaf_cache *cache;
	int rc;

	if (mc == NULL || values == NULL || countp == NULL)
		return EINVAL;

	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (mc->mc_xcursor == NULL || !(mc->mc_db->md_flags & MDB_DUPSORT))
		return MDB_INCOMPATIBLE;

	if (!(mc->mc_flags & C_INITIALIZED) || !mc->mc_snum)
		return MDB_NOTFOUND;

	if (mc->mc_flags & C_EOF) {
		if (mc->mc_ki[mc->mc_top] >= NUMKEYS(mc->mc_pg[mc->mc_top]))
			return MDB_NOTFOUND;
		mc->mc_flags ^= C_EOF;
	}

	leaf = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
	if (!F_ISSET(leaf->mn_flags, F_DUPDATA))
		return MDB_NOTFOUND;
	if (F_ISSET(leaf->mn_flags, F_SUBDATA))
		return mdb_cursor_list_dup_subdb(mc, values, countp);

	mx = &mc->mc_xcursor->mx_cursor;
	if (!(mx->mc_flags & C_INITIALIZED))
		return EINVAL;

	dp = mx->mc_pg[mx->mc_top];
	if (!dp)
		return MDB_CORRUPTED;

	cache = &mx->mc_leaf_cache;
	rc = mdb_cursor_leaf_cache_prepare(mx, dp);
	if (rc != MDB_SUCCESS)
		return rc;

	if (IS_LEAF2(dp))
		rc = mdb_cursor_list_dup_leaf2(mx, dp);
	else
		rc = mdb_cursor_list_dup_regular(mx, dp);
	if (rc != MDB_SUCCESS)
		return rc;

	*countp = cache->decoded_count;
	*values = cache->decoded_count ? cache->decoded_vals : NULL;
	return MDB_SUCCESS;
}

void
mdb_cursor_close(MDB_cursor *mc)
{
	MDB_TRACE(("%p", mc));
	if (mc) {
		MDB_CURSOR_UNREF(mc, 0);
	}
	if (mc && !mc->mc_backup) {
		/* Remove from txn, if tracked.
		 * A read-only txn (!C_UNTRACK) may have been freed already,
		 * so do not peek inside it.  Only write txns track cursors.
		 */
		if ((mc->mc_flags & C_UNTRACK) && mc->mc_txn->mt_cursors) {
			MDB_cursor **prev = &mc->mc_txn->mt_cursors[mc->mc_dbi];
			while (*prev && *prev != mc) prev = &(*prev)->mc_next;
			if (*prev == mc)
				*prev = mc->mc_next;
		}
		mdb_cursor_leaf_cache_clear(&mc->mc_leaf_cache);
		free(mc);
	}
}

MDB_txn *
mdb_cursor_txn(MDB_cursor *mc)
{
	if (!mc) return NULL;
	return mc->mc_txn;
}

MDB_dbi
mdb_cursor_dbi(MDB_cursor *mc)
{
	return mc->mc_dbi;
}

/** Replace the key for a branch node with a new key.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in] mc Cursor pointing to the node to operate on.
 * @param[in] key The new key to use.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_update_key(MDB_cursor *mc, MDB_val *key)
{
	MDB_page		*mp;
	MDB_node		*node;
	char			*base;
	size_t			 len;
	int				 delta, ksize, oksize;
	indx_t			 ptr, i, numkeys, indx;
	DKBUF;

	indx = mc->mc_ki[mc->mc_top];
	mp = mc->mc_pg[mc->mc_top];
	node = NODEPTR(mp, indx);
	ptr = mp->mp_ptrs[indx];
#if MDB_DEBUG
	{
		MDB_val	k2;
		char kbuf2[DKBUF_MAXKEYSIZE*2+1];
		k2.mv_data = NODEKEY(mp, node);
		k2.mv_size = node->mn_ksize;
		DPRINTF(("update key %u (ofs %u) [%s] to [%s] on page %"Yu,
			indx, ptr,
			mdb_dkey(&k2, kbuf2),
			DKEY(key),
			mp->mp_pgno));
	}
#endif

	/* Sizes must be 2-byte aligned. */
	ksize = EVEN(key->mv_size);
	oksize = EVEN(node->mn_ksize);
	delta = ksize - oksize;

	/* Shift node contents if EVEN(key length) changed. */
	if (delta) {
		if (delta > 0 && SIZELEFT(mp) < delta) {
			pgno_t pgno;
			/* not enough space left, do a delete and split */
			DPRINTF(("Not enough room, delta = %d, splitting...", delta));
			pgno = NODEPGNO(node);
			mdb_node_del(mc, 0);
			return mdb_page_split(mc, key, NULL, pgno, MDB_SPLIT_REPLACE);
		}

		numkeys = NUMKEYS(mp);
		for (i = 0; i < numkeys; i++) {
			if (mp->mp_ptrs[i] <= ptr)
				mp->mp_ptrs[i] -= delta;
		}

		base = (char *)mp + mp->mp_upper + PAGEBASE;
		len = ptr - mp->mp_upper + NODESIZE;
		memmove(base - delta, base, len);
		mp->mp_upper -= delta;

		node = NODEPTR(mp, indx);
	}

	/* But even if no shift was needed, update ksize */
	if (node->mn_ksize != key->mv_size)
		node->mn_ksize = key->mv_size;

	if (key->mv_size)
		memcpy(NODEKEY(mp, node), key->mv_data, key->mv_size);

	return MDB_SUCCESS;
}

static void
mdb_cursor_copy(const MDB_cursor *csrc, MDB_cursor *cdst);

/** Perform \b act while tracking temporary cursor \b mn */
#define WITH_CURSOR_TRACKING(mn, act) do { \
	MDB_cursor dummy, *tracked, **tp = &(mn).mc_txn->mt_cursors[mn.mc_dbi]; \
	if ((mn).mc_flags & C_SUB) { \
		dummy.mc_flags =  C_INITIALIZED; \
		dummy.mc_xcursor = (MDB_xcursor *)&(mn);	\
		tracked = &dummy; \
	} else { \
		tracked = &(mn); \
	} \
	tracked->mc_next = *tp; \
	*tp = tracked; \
	{ act; } \
	*tp = tracked->mc_next; \
} while (0)

/** Move a node from csrc to cdst.
 */
static int
mdb_node_move(MDB_cursor *csrc, MDB_cursor *cdst, int fromleft)
{
	MDB_node		*srcnode;
	MDB_val		 key, data;
	pgno_t	srcpg;
	MDB_cursor mn = (MDB_cursor){0};
	int			 rc;
	unsigned short flags;
	MDB_page	*psrc;
	int			 counted;
	uint64_t	 moved_total = 0;
	indx_t			 src_idx = csrc->mc_ki[csrc->mc_top];

	DKBUF;

	/* Mark src and dst as dirty. */
	if ((rc = mdb_page_touch(csrc)) ||
 	    (rc = mdb_page_touch(cdst)))
		return rc;
	psrc = csrc->mc_pg[csrc->mc_top];
	counted = (csrc->mc_db->md_flags & MDB_COUNTED);

	if (IS_LEAF2(csrc->mc_pg[csrc->mc_top])) {
		key.mv_size = csrc->mc_db->md_pad;
		key.mv_data = LEAF2KEY(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top], key.mv_size);
		data.mv_size = 0;
		data.mv_data = NULL;
		srcpg = 0;
		flags = 0;
		if (counted)
			moved_total = 1;
	} else {
		srcnode = NODEPTR(csrc->mc_pg[csrc->mc_top], csrc->mc_ki[csrc->mc_top]);
		mdb_cassert(csrc, !((size_t)srcnode & 1));
		srcpg = NODEPGNO(srcnode);
		flags = srcnode->mn_flags;
		if (counted) {
			if (IS_BRANCH(psrc))
				moved_total = mdb_node_get_count(psrc, srcnode);
			else
				moved_total = mdb_leaf_entry_contribution(psrc, srcnode);
		}
		if (csrc->mc_ki[csrc->mc_top] == 0 && IS_BRANCH(csrc->mc_pg[csrc->mc_top])) {
			unsigned int snum = csrc->mc_snum;
			MDB_node *s2;
			/* must find the lowest key below src */
			rc = mdb_page_search_lowest(csrc);
			if (rc)
				return rc;
			if (IS_LEAF2(csrc->mc_pg[csrc->mc_top])) {
				key.mv_size = csrc->mc_db->md_pad;
				key.mv_data = LEAF2KEY(csrc->mc_pg[csrc->mc_top], 0, key.mv_size);
			} else {
				s2 = NODEPTR(csrc->mc_pg[csrc->mc_top], 0);
				if (csrc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) {
					rc = mdb_cursor_read_key_at(csrc, csrc->mc_pg[csrc->mc_top], 0, &key);
					if (rc != MDB_SUCCESS)
						return rc;
				} else {
					key.mv_size = NODEKSZ(s2);
					key.mv_data = NODEKEY(csrc->mc_pg[csrc->mc_top], s2);
				}
			}
			csrc->mc_snum = snum--;
			csrc->mc_top = snum;
		} else {
			key.mv_size = NODEKSZ(srcnode);
			key.mv_data = NODEKEY(csrc->mc_pg[csrc->mc_top], srcnode);
		}
		if ((csrc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) && IS_LEAF(psrc) && !IS_LEAF2(psrc) && src_idx != 0) {
			MDB_node *tr = NODEPTR(psrc, 0);
			MDB_val trunk = { tr->mn_ksize, NODEKEY(psrc, tr) };
			MDB_val decoded = {0, csrc->mc_keybuf};
			rc = mdb_leaf_decode_key(&trunk, key.mv_data, key.mv_size,
			    &decoded, csrc->mc_keybuf, MDB_KEYBUF_MAX, 0, NULL);
			if (rc != MDB_SUCCESS)
				return rc;
			key = decoded;
		}
		data.mv_size = NODEDSZ(srcnode);
		data.mv_data = NODEDATA(srcnode);
	}
	mn.mc_xcursor = NULL;
	if (IS_BRANCH(cdst->mc_pg[cdst->mc_top]) && cdst->mc_ki[cdst->mc_top] == 0) {
		unsigned int snum = cdst->mc_snum;
		MDB_node *s2;
		MDB_val bkey;
		/* must find the lowest key below dst */
	mdb_cursor_copy(cdst, &mn);
	rc = mdb_page_search_lowest(&mn);
	if (rc)
		return rc;
		if (IS_LEAF2(mn.mc_pg[mn.mc_top])) {
			bkey.mv_size = mn.mc_db->md_pad;
			bkey.mv_data = LEAF2KEY(mn.mc_pg[mn.mc_top], 0, bkey.mv_size);
		} else {
			s2 = NODEPTR(mn.mc_pg[mn.mc_top], 0);
		if (mn.mc_db->md_flags & MDB_PREFIX_COMPRESSION) {
			rc = mdb_cursor_read_key_at(&mn, mn.mc_pg[mn.mc_top], 0, &bkey);
			if (rc != MDB_SUCCESS)
				return rc;
			} else {
				bkey.mv_size = NODEKSZ(s2);
				bkey.mv_data = NODEKEY(mn.mc_pg[mn.mc_top], s2);
			}
		}
		mn.mc_snum = snum--;
		mn.mc_top = snum;
		mn.mc_ki[snum] = 0;
	rc = mdb_update_key(&mn, &bkey);
	if (rc)
		return rc;
	}

	DPRINTF(("moving %s node %u [%s] on page %"Yu" to node %u on page %"Yu,
	    IS_LEAF(csrc->mc_pg[csrc->mc_top]) ? "leaf" : "branch",
	    csrc->mc_ki[csrc->mc_top],
		DKEY(&key),
	    csrc->mc_pg[csrc->mc_top]->mp_pgno,
	    cdst->mc_ki[cdst->mc_top], cdst->mc_pg[cdst->mc_top]->mp_pgno));

	/* Add the node to the destination page.
	 */
	MDB_page *dstpg = cdst->mc_pg[cdst->mc_top];
	uint64_t child_hint = MDB_COUNT_HINT_NONE;
	if (counted && IS_BRANCH(dstpg) && IS_COUNTED(dstpg))
		child_hint = moved_total;
	rc = mdb_node_add(cdst, cdst->mc_ki[cdst->mc_top], &key, &data, srcpg, flags,
		NULL, child_hint);
	if (rc != MDB_SUCCESS)
		return rc;

	/* Delete the node from the source page.
	 */
	mdb_node_del(csrc, key.mv_size);

	{
		/* Adjust other cursors pointing to mp */
		MDB_cursor *m2, *m3;
		MDB_dbi dbi = csrc->mc_dbi;
		MDB_page *mpd, *mps;

		mps = csrc->mc_pg[csrc->mc_top];
		/* If we're adding on the left, bump others up */
		if (fromleft) {
			mpd = cdst->mc_pg[csrc->mc_top];
			for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
				if (csrc->mc_flags & C_SUB)
					m3 = &m2->mc_xcursor->mx_cursor;
				else
					m3 = m2;
				if (!(m3->mc_flags & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
					continue;
				if (m3 != cdst &&
					m3->mc_pg[csrc->mc_top] == mpd &&
					m3->mc_ki[csrc->mc_top] >= cdst->mc_ki[csrc->mc_top]) {
					m3->mc_ki[csrc->mc_top]++;
				}
				if (m3 !=csrc &&
					m3->mc_pg[csrc->mc_top] == mps &&
					m3->mc_ki[csrc->mc_top] == csrc->mc_ki[csrc->mc_top]) {
					m3->mc_pg[csrc->mc_top] = cdst->mc_pg[cdst->mc_top];
					m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
					m3->mc_ki[csrc->mc_top-1]++;
				}
				if (IS_LEAF(mps))
					XCURSOR_REFRESH(m3, csrc->mc_top, m3->mc_pg[csrc->mc_top]);
			}
		} else
		/* Adding on the right, bump others down */
		{
			for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
				if (csrc->mc_flags & C_SUB)
					m3 = &m2->mc_xcursor->mx_cursor;
				else
					m3 = m2;
				if (m3 == csrc) continue;
				if (!(m3->mc_flags & C_INITIALIZED) || m3->mc_top < csrc->mc_top)
					continue;
				if (m3->mc_pg[csrc->mc_top] == mps) {
					if (!m3->mc_ki[csrc->mc_top]) {
						m3->mc_pg[csrc->mc_top] = cdst->mc_pg[cdst->mc_top];
						m3->mc_ki[csrc->mc_top] = cdst->mc_ki[cdst->mc_top];
						m3->mc_ki[csrc->mc_top-1]--;
					} else {
						m3->mc_ki[csrc->mc_top]--;
					}
					if (IS_LEAF(mps))
						XCURSOR_REFRESH(m3, csrc->mc_top, m3->mc_pg[csrc->mc_top]);
				}
			}
		}
	}

	/* Update the parent separators.
	 */
	if (csrc->mc_ki[csrc->mc_top] == 0) {
		if (csrc->mc_ki[csrc->mc_top-1] != 0) {
			if (IS_LEAF2(csrc->mc_pg[csrc->mc_top])) {
				key.mv_data = LEAF2KEY(csrc->mc_pg[csrc->mc_top], 0, key.mv_size);
			} else {
				srcnode = NODEPTR(csrc->mc_pg[csrc->mc_top], 0);
				if (csrc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) {
					rc = mdb_cursor_read_key_at(csrc, csrc->mc_pg[csrc->mc_top], 0, &key);
					if (rc != MDB_SUCCESS)
						return rc;
				} else {
					key.mv_size = NODEKSZ(srcnode);
					key.mv_data = NODEKEY(csrc->mc_pg[csrc->mc_top], srcnode);
				}
			}
			DPRINTF(("update separator for source page %"Yu" to [%s]",
				csrc->mc_pg[csrc->mc_top]->mp_pgno, DKEY(&key)));
			mdb_cursor_copy(csrc, &mn);
			mn.mc_snum--;
			mn.mc_top--;
			/* We want mdb_rebalance to find mn when doing fixups */
		WITH_CURSOR_TRACKING(mn,
			rc = mdb_update_key(&mn, &key));
		if (rc)
			return rc;
		}
		if (IS_BRANCH(csrc->mc_pg[csrc->mc_top])) {
			MDB_val	 nullkey;
			indx_t	ix = csrc->mc_ki[csrc->mc_top];
			nullkey.mv_size = 0;
			csrc->mc_ki[csrc->mc_top] = 0;
			rc = mdb_update_key(csrc, &nullkey);
			csrc->mc_ki[csrc->mc_top] = ix;
			mdb_cassert(csrc, rc == MDB_SUCCESS);
		}
	}

	if (cdst->mc_ki[cdst->mc_top] == 0) {
		if (cdst->mc_ki[cdst->mc_top-1] != 0) {
			if (IS_LEAF2(csrc->mc_pg[csrc->mc_top])) {
				key.mv_data = LEAF2KEY(cdst->mc_pg[cdst->mc_top], 0, key.mv_size);
			} else {
				srcnode = NODEPTR(cdst->mc_pg[cdst->mc_top], 0);
				if (cdst->mc_db->md_flags & MDB_PREFIX_COMPRESSION) {
					rc = mdb_cursor_read_key_at(cdst, cdst->mc_pg[cdst->mc_top], 0, &key);
					if (rc != MDB_SUCCESS)
						return rc;
				} else {
					key.mv_size = NODEKSZ(srcnode);
					key.mv_data = NODEKEY(cdst->mc_pg[cdst->mc_top], srcnode);
				}
			}
			DPRINTF(("update separator for destination page %"Yu" to [%s]",
				cdst->mc_pg[cdst->mc_top]->mp_pgno, DKEY(&key)));
			mdb_cursor_copy(cdst, &mn);
			mn.mc_snum--;
			mn.mc_top--;
			/* We want mdb_rebalance to find mn when doing fixups */
			WITH_CURSOR_TRACKING(mn,
				rc = mdb_update_key(&mn, &key));
			if (rc)
				return rc;
		}
		if (IS_BRANCH(cdst->mc_pg[cdst->mc_top])) {
			MDB_val	 nullkey;
			indx_t	ix = cdst->mc_ki[cdst->mc_top];
			nullkey.mv_size = 0;
			cdst->mc_ki[cdst->mc_top] = 0;
			rc = mdb_update_key(cdst, &nullkey);
			cdst->mc_ki[cdst->mc_top] = ix;
			mdb_cassert(cdst, rc == MDB_SUCCESS);
		}
	}

	if (counted) {
		if (csrc->mc_top > 0 && moved_total) {
			MDB_page *src_parent = csrc->mc_pg[csrc->mc_top - 1];
			int64_t diff_src = 0;
			if (IS_BRANCH(src_parent) && IS_COUNTED(src_parent)) {
				MDB_node *src_ref = NODEPTR(src_parent, csrc->mc_ki[csrc->mc_top - 1]);
				uint64_t before = mdb_node_get_count(src_parent, src_ref);
				uint64_t after = before >= moved_total ? before - moved_total : 0;
				if (after != before) {
					mdb_node_set_count(src_parent, src_ref, after);
					diff_src = (int64_t)after - (int64_t)before;
				}
			}
			if (diff_src)
				mdb_propagate_count_delta(csrc, csrc->mc_top - 2, diff_src);
		}
		if (cdst->mc_top > 0 && moved_total) {
			MDB_page *dst_parent = cdst->mc_pg[cdst->mc_top - 1];
			int64_t diff_dst = 0;
			if (IS_BRANCH(dst_parent) && IS_COUNTED(dst_parent)) {
				MDB_node *dst_ref = NODEPTR(dst_parent, cdst->mc_ki[cdst->mc_top - 1]);
				uint64_t before = mdb_node_get_count(dst_parent, dst_ref);
				uint64_t after = before + moved_total;
				if (after != before) {
					mdb_node_set_count(dst_parent, dst_ref, after);
					diff_dst = (int64_t)after - (int64_t)before;
				}
			}
			if (diff_dst)
				mdb_propagate_count_delta(cdst, cdst->mc_top - 2, diff_dst);
		}
	}

	return MDB_SUCCESS;
}

/** Merge one page into another.
 *  The nodes from the page pointed to by \b csrc will
 *	be copied to the page pointed to by \b cdst and then
 *	the \b csrc page will be freed.
 * @param[in] csrc Cursor pointing to the source page.
 * @param[in] cdst Cursor pointing to the destination page.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_merge(MDB_cursor *csrc, MDB_cursor *cdst)
{
	MDB_page	*psrc, *pdst;
	MDB_node	*srcnode;
	MDB_val		 key, data;
	unsigned	 nkeys;
	int			 rc;
	indx_t		 i, j;
	int			 counted;
	uint64_t	 moved_total = 0;

	psrc = csrc->mc_pg[csrc->mc_top];
	pdst = cdst->mc_pg[cdst->mc_top];
	counted = (csrc->mc_db->md_flags & MDB_COUNTED);

	DPRINTF(("merging page %"Yu" into %"Yu, psrc->mp_pgno, pdst->mp_pgno));

	mdb_cassert(csrc, csrc->mc_snum > 1);	/* can't merge root page */
	mdb_cassert(csrc, cdst->mc_snum > 1);

	/* Mark dst as dirty. */
	if ((rc = mdb_page_touch(cdst)))
		return rc;

	/* get dst page again now that we've touched it. */
	pdst = cdst->mc_pg[cdst->mc_top];

	/* Move all nodes from src to dst.
	 */
	j = nkeys = NUMKEYS(pdst);
	if (IS_LEAF2(psrc)) {
		key.mv_size = csrc->mc_db->md_pad;
		key.mv_data = METADATA(psrc);
		for (i = 0; i < NUMKEYS(psrc); i++, j++) {
			rc = mdb_node_add(cdst, j, &key, NULL, 0, 0,
		    NULL, MDB_COUNT_HINT_NONE);
			if (rc != MDB_SUCCESS)
				return rc;
			key.mv_data = (char *)key.mv_data + key.mv_size;
			if (counted)
				moved_total++;
		}
	} else {
		for (i = 0; i < NUMKEYS(psrc); i++, j++) {
			srcnode = NODEPTR(psrc, i);
			if (i == 0 && IS_BRANCH(psrc)) {
					MDB_cursor mn = (MDB_cursor){0};
				MDB_node *s2;
				mdb_cursor_copy(csrc, &mn);
				mn.mc_xcursor = NULL;
				/* must find the lowest key below src */
				rc = mdb_page_search_lowest(&mn);
				if (rc)
					return rc;
				if (IS_LEAF2(mn.mc_pg[mn.mc_top])) {
					key.mv_size = mn.mc_db->md_pad;
					key.mv_data = LEAF2KEY(mn.mc_pg[mn.mc_top], 0, key.mv_size);
				} else {
					s2 = NODEPTR(mn.mc_pg[mn.mc_top], 0);
				if (mn.mc_db->md_flags & MDB_PREFIX_COMPRESSION) {
					rc = mdb_cursor_read_key_at(&mn, mn.mc_pg[mn.mc_top], 0, &key);
					if (rc != MDB_SUCCESS)
						return rc;
					} else {
						key.mv_size = NODEKSZ(s2);
						key.mv_data = NODEKEY(mn.mc_pg[mn.mc_top], s2);
					}
				}
			} else {
				if (IS_LEAF(psrc) && (csrc->mc_db->md_flags & MDB_PREFIX_COMPRESSION)) {
					rc = mdb_cursor_read_key_at(csrc, psrc, i, &key);
					if (rc != MDB_SUCCESS)
						return rc;
				} else {
					key.mv_size = srcnode->mn_ksize;
					key.mv_data = NODEKEY(psrc, srcnode);
				}
			}

			data.mv_size = NODEDSZ(srcnode);
			data.mv_data = NODEDATA(srcnode);
			uint64_t contrib = 0;
			if (counted) {
				if (IS_BRANCH(psrc))
					contrib = mdb_node_get_count(psrc, srcnode);
				else
					contrib = mdb_leaf_entry_contribution(psrc, srcnode);
			}
			uint64_t child_hint = MDB_COUNT_HINT_NONE;
			if (counted && IS_BRANCH(pdst) && IS_COUNTED(pdst))
				child_hint = contrib;
			rc = mdb_node_add(cdst, j, &key, &data, NODEPGNO(srcnode),
			    srcnode->mn_flags, NULL, child_hint);
			if (rc != MDB_SUCCESS)
				return rc;
			if (counted)
				moved_total += contrib;
		}
	}

	DPRINTF(("dst page %"Yu" now has %u keys (%.1f%% filled)",
	    pdst->mp_pgno, NUMKEYS(pdst),
		(float)PAGEFILL(cdst->mc_txn->mt_env, pdst) / 10));

	if (counted && cdst->mc_top > 0 && moved_total) {
		MDB_page *parent = cdst->mc_pg[cdst->mc_top - 1];
		int64_t diff_dst = 0;
		if (IS_BRANCH(parent) && IS_COUNTED(parent)) {
			MDB_node *dst_ref = NODEPTR(parent, cdst->mc_ki[cdst->mc_top - 1]);
			uint64_t before = mdb_node_get_count(parent, dst_ref);
			uint64_t after = before + moved_total;
			if (after != before) {
				mdb_node_set_count(parent, dst_ref, after);
				diff_dst = (int64_t)after - (int64_t)before;
			}
		}
		if (diff_dst)
			mdb_propagate_count_delta(cdst, cdst->mc_top - 2, diff_dst);
	}

	/* Unlink the src page from parent and add to free list.
	 */
	csrc->mc_top--;
	if (counted && moved_total && csrc->mc_top > 0)
		mdb_propagate_count_delta(csrc, csrc->mc_top - 1,
		    -(int64_t)moved_total);
	mdb_node_del(csrc, 0);
	if (csrc->mc_ki[csrc->mc_top] == 0) {
		key.mv_size = 0;
		rc = mdb_update_key(csrc, &key);
		if (rc) {
			csrc->mc_top++;
			return rc;
		}
	}
	csrc->mc_top++;

	psrc = csrc->mc_pg[csrc->mc_top];
	/* If not operating on FreeDB, allow this page to be reused
	 * in this txn. Otherwise just add to free list.
	 */
	rc = mdb_page_loose(csrc, psrc);
	if (rc)
		return rc;
	if (IS_LEAF(psrc))
		csrc->mc_db->md_leaf_pages--;
	else
		csrc->mc_db->md_branch_pages--;
	{
		/* Adjust other cursors pointing to mp */
		MDB_cursor *m2, *m3;
		MDB_dbi dbi = csrc->mc_dbi;
		unsigned int top = csrc->mc_top;

		for (m2 = csrc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
			if (csrc->mc_flags & C_SUB)
				m3 = &m2->mc_xcursor->mx_cursor;
			else
				m3 = m2;
			if (m3 == csrc) continue;
			if (m3->mc_snum < csrc->mc_snum) continue;
			if (m3->mc_pg[top] == psrc) {
				m3->mc_pg[top] = pdst;
				m3->mc_ki[top] += nkeys;
				m3->mc_ki[top-1] = cdst->mc_ki[top-1];
			} else if (m3->mc_pg[top-1] == csrc->mc_pg[top-1] &&
				m3->mc_ki[top-1] > csrc->mc_ki[top-1]) {
				m3->mc_ki[top-1]--;
			}
			if (IS_LEAF(psrc))
				XCURSOR_REFRESH(m3, top, m3->mc_pg[top]);
		}
	}
	{
		unsigned int snum = cdst->mc_snum;
		uint16_t depth = cdst->mc_db->md_depth;
		mdb_cursor_pop(cdst);
	rc = mdb_rebalance(cdst);
		/* Did the tree height change? */
		if (depth != cdst->mc_db->md_depth)
			snum += cdst->mc_db->md_depth - depth;
		cdst->mc_snum = snum;
		cdst->mc_top = snum-1;
	}
	return rc;
}

/** Copy the contents of a cursor.
 * @param[in] csrc The cursor to copy from.
 * @param[out] cdst The cursor to copy to.
 */
static void
mdb_cursor_copy(const MDB_cursor *csrc, MDB_cursor *cdst)
{
	unsigned int i;

	cdst->mc_txn = csrc->mc_txn;
	cdst->mc_dbi = csrc->mc_dbi;
	cdst->mc_db  = csrc->mc_db;
	cdst->mc_dbx = csrc->mc_dbx;
	cdst->mc_snum = csrc->mc_snum;
	cdst->mc_top = csrc->mc_top;
	cdst->mc_flags = csrc->mc_flags;
	cdst->mc_key.mv_data = cdst->mc_keybuf;
	cdst->mc_key.mv_size = 0;
	cdst->mc_key_pgno = csrc->mc_key_pgno;
	cdst->mc_key_last = csrc->mc_key_last;
	cdst->mc_seq_pgno = csrc->mc_seq_pgno;
	cdst->mc_seq_idx = csrc->mc_seq_idx;
	cdst->mc_seq_shared = csrc->mc_seq_shared;
	cdst->mc_seq_cached_shared = csrc->mc_seq_cached_shared;
	cdst->mc_seq_cached_used = csrc->mc_seq_cached_used;
	cdst->mc_seq_cached_idx = csrc->mc_seq_cached_idx;
	cdst->mc_seq_cached_pgno = csrc->mc_seq_cached_pgno;
	cdst->mc_seq_cache_valid = csrc->mc_seq_cache_valid;
	cdst->mc_seq_keybuf_valid = 0;
	if ((csrc->mc_txn->mt_flags & MDB_TXN_RDONLY) &&
	    (csrc->mc_flags & C_LEAFCACHE) &&
	    csrc->mc_leaf_cache.decoded_pgno != P_INVALID &&
	    mdb_cursor_leaf_cache_clone(&cdst->mc_leaf_cache,
		    &csrc->mc_leaf_cache, csrc->mc_txn) != MDB_SUCCESS) {
		mdb_cursor_leaf_cache_reset(&cdst->mc_leaf_cache);
	} else if (!(csrc->mc_txn->mt_flags & MDB_TXN_RDONLY) ||
		   !(csrc->mc_flags & C_LEAFCACHE) ||
		   csrc->mc_leaf_cache.decoded_pgno == P_INVALID) {
		mdb_cursor_leaf_cache_reset(&cdst->mc_leaf_cache);
	}
	cdst->mc_seq_cmp_keyptr = NULL;
	cdst->mc_seq_cmp_keysize = 0;
	cdst->mc_seq_cmp_prefix = 0;
	cdst->mc_seq_cmp_pgno = P_INVALID;
	memcpy(cdst->mc_rank_base, csrc->mc_rank_base, sizeof(csrc->mc_rank_base));
	cdst->mc_rank_cached = csrc->mc_rank_cached;
	cdst->mc_rank_offset = csrc->mc_rank_offset;
	if (csrc->mc_key.mv_size && csrc->mc_key.mv_size <= MDB_KEYBUF_MAX) {
		memcpy(cdst->mc_keybuf, csrc->mc_keybuf, csrc->mc_key.mv_size);
		cdst->mc_key.mv_size = csrc->mc_key.mv_size;
		if (csrc->mc_seq_keybuf_valid)
			cdst->mc_seq_keybuf_valid = 1;
	}
	MC_SET_OVPG(cdst, MC_OVPG(csrc));

	for (i=0; i<csrc->mc_snum; i++) {
		cdst->mc_pg[i] = csrc->mc_pg[i];
		cdst->mc_ki[i] = csrc->mc_ki[i];
	}
}

/** Rebalance the tree after a delete operation.
 * @param[in] mc Cursor pointing to the page where rebalancing
 * should begin.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_rebalance(MDB_cursor *mc)
{
	MDB_node	*node;
	int rc, fromleft;
	unsigned int ptop, minkeys, thresh;
	MDB_cursor	mn;
	indx_t oldki;

	if (IS_BRANCH(mc->mc_pg[mc->mc_top])) {
		minkeys = 2;
		thresh = 1;
	} else {
		minkeys = 1;
		thresh = FILL_THRESHOLD;
	}
	DPRINTF(("rebalancing %s page %"Yu" (has %u keys, %.1f%% full)",
	    IS_LEAF(mc->mc_pg[mc->mc_top]) ? "leaf" : "branch",
	    mdb_dbg_pgno(mc->mc_pg[mc->mc_top]), NUMKEYS(mc->mc_pg[mc->mc_top]),
		(float)PAGEFILL(mc->mc_txn->mt_env, mc->mc_pg[mc->mc_top]) / 10));

	if (PAGEFILL(mc->mc_txn->mt_env, mc->mc_pg[mc->mc_top]) >= thresh &&
		NUMKEYS(mc->mc_pg[mc->mc_top]) >= minkeys) {
		DPRINTF(("no need to rebalance page %"Yu", above fill threshold",
		    mdb_dbg_pgno(mc->mc_pg[mc->mc_top])));
		return MDB_SUCCESS;
	}

	if (mc->mc_snum < 2) {
		MDB_page *mp = mc->mc_pg[0];
		if (IS_SUBP(mp)) {
			DPUTS("Can't rebalance a subpage, ignoring");
			return MDB_SUCCESS;
		}
		if (NUMKEYS(mp) == 0) {
			DPUTS("tree is completely empty");
			mc->mc_db->md_root = P_INVALID;
			mc->mc_db->md_depth = 0;
			mc->mc_db->md_leaf_pages = 0;
			rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
			if (rc)
				return rc;
			/* Adjust cursors pointing to mp */
			mc->mc_snum = 0;
			mc->mc_top = 0;
			mc->mc_flags &= ~C_INITIALIZED;
			{
				MDB_cursor *m2, *m3;
				MDB_dbi dbi = mc->mc_dbi;

				for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
					if (mc->mc_flags & C_SUB)
						m3 = &m2->mc_xcursor->mx_cursor;
					else
						m3 = m2;
					if (!(m3->mc_flags & C_INITIALIZED) || (m3->mc_snum < mc->mc_snum))
						continue;
					if (m3->mc_pg[0] == mp) {
						m3->mc_snum = 0;
						m3->mc_top = 0;
						m3->mc_flags &= ~C_INITIALIZED;
					}
				}
			}
		} else if (IS_BRANCH(mp) && NUMKEYS(mp) == 1) {
			int i;
			DPUTS("collapsing root page!");
			rc = mdb_midl_append(&mc->mc_txn->mt_free_pgs, mp->mp_pgno);
			if (rc)
				return rc;
			mc->mc_db->md_root = NODEPGNO(NODEPTR(mp, 0));
			rc = mdb_page_get(mc, mc->mc_db->md_root, &mc->mc_pg[0], NULL);
			if (rc)
				return rc;
			mc->mc_db->md_depth--;
			mc->mc_db->md_branch_pages--;
			mc->mc_ki[0] = mc->mc_ki[1];
			for (i = 1; i<mc->mc_db->md_depth; i++) {
				mc->mc_pg[i] = mc->mc_pg[i+1];
				mc->mc_ki[i] = mc->mc_ki[i+1];
			}
			{
				/* Adjust other cursors pointing to mp */
				MDB_cursor *m2, *m3;
				MDB_dbi dbi = mc->mc_dbi;

				for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
					if (mc->mc_flags & C_SUB)
						m3 = &m2->mc_xcursor->mx_cursor;
					else
						m3 = m2;
					if (m3 == mc) continue;
					if (!(m3->mc_flags & C_INITIALIZED))
						continue;
					if (m3->mc_pg[0] == mp) {
						for (i=0; i<mc->mc_db->md_depth; i++) {
							m3->mc_pg[i] = m3->mc_pg[i+1];
							m3->mc_ki[i] = m3->mc_ki[i+1];
						}
						m3->mc_snum--;
						m3->mc_top--;
					}
				}
			}
		} else
			DPUTS("root page doesn't need rebalancing");
		return MDB_SUCCESS;
	}

	/* The parent (branch page) must have at least 2 pointers,
	 * otherwise the tree is invalid.
	 */
	ptop = mc->mc_top-1;
	mdb_cassert(mc, NUMKEYS(mc->mc_pg[ptop]) > 1);

	/* Leaf page fill factor is below the threshold.
	 * Try to move keys from left or right neighbor, or
	 * merge with a neighbor page.
	 */

	/* Find neighbors.
	 */
	mdb_cursor_copy(mc, &mn);
	mn.mc_xcursor = NULL;

	oldki = mc->mc_ki[mc->mc_top];
	if (mc->mc_ki[ptop] == 0) {
		/* We're the leftmost leaf in our parent.
		 */
		DPUTS("reading right neighbor");
		mn.mc_ki[ptop]++;
		node = NODEPTR(mc->mc_pg[ptop], mn.mc_ki[ptop]);
		rc = mdb_page_get(mc, NODEPGNO(node), &mn.mc_pg[mn.mc_top], NULL);
		if (rc)
			return rc;
		mn.mc_ki[mn.mc_top] = 0;
		mc->mc_ki[mc->mc_top] = NUMKEYS(mc->mc_pg[mc->mc_top]);
		fromleft = 0;
	} else {
		/* There is at least one neighbor to the left.
		 */
		DPUTS("reading left neighbor");
		mn.mc_ki[ptop]--;
		node = NODEPTR(mc->mc_pg[ptop], mn.mc_ki[ptop]);
		rc = mdb_page_get(mc, NODEPGNO(node), &mn.mc_pg[mn.mc_top], NULL);
		if (rc)
			return rc;
		mn.mc_ki[mn.mc_top] = NUMKEYS(mn.mc_pg[mn.mc_top]) - 1;
		mc->mc_ki[mc->mc_top] = 0;
		fromleft = 1;
	}

	DPRINTF(("found neighbor page %"Yu" (%u keys, %.1f%% full)",
	    mn.mc_pg[mn.mc_top]->mp_pgno, NUMKEYS(mn.mc_pg[mn.mc_top]),
		(float)PAGEFILL(mc->mc_txn->mt_env, mn.mc_pg[mn.mc_top]) / 10));

	/* If the neighbor page is above threshold and has enough keys,
	 * move one key from it. Otherwise we should try to merge them.
	 * (A branch page must never have less than 2 keys.)
	 */
	if (PAGEFILL(mc->mc_txn->mt_env, mn.mc_pg[mn.mc_top]) >= thresh && NUMKEYS(mn.mc_pg[mn.mc_top]) > minkeys) {
	rc = mdb_node_move(&mn, mc, fromleft);
		if (fromleft) {
			/* if we inserted on left, bump position up */
			oldki++;
		}
	} else {
		if (!fromleft) {
			rc = mdb_page_merge(&mn, mc);
		} else {
			oldki += NUMKEYS(mn.mc_pg[mn.mc_top]);
			mn.mc_ki[mn.mc_top] += mc->mc_ki[mn.mc_top] + 1;
			/* We want mdb_rebalance to find mn when doing fixups */
			WITH_CURSOR_TRACKING(mn,
				rc = mdb_page_merge(mc, &mn));
			mdb_cursor_copy(&mn, mc);
		}
		mc->mc_flags &= ~C_EOF;
	}
	mc->mc_ki[mc->mc_top] = oldki;
	return rc;
}

/** Complete a delete operation started by #mdb_cursor_del(). */
static int
mdb_cursor_del0(MDB_cursor *mc)
{
	int rc;
	MDB_page *mp;
	indx_t ki;
	unsigned int nkeys;
	MDB_cursor *m2, *m3;
	MDB_dbi dbi = mc->mc_dbi;
	int counted = (mc->mc_db->md_flags & MDB_COUNTED);
	MDB_page *parent = NULL;
	MDB_node *parent_ref = NULL;
	indx_t parent_idx = 0;
	uint64_t parent_before = 0;
	int have_direct_parent = 0;

	ki = mc->mc_ki[mc->mc_top];
	mp = mc->mc_pg[mc->mc_top];
	uint64_t prior_total = 0;
	uint64_t removal_contrib = 0;
	if (counted) {
		MDB_node *leaf = IS_LEAF2(mp) ? NULL : NODEPTR(mp, ki);
		prior_total = mdb_leaf_contribution(mp);
		if (IS_LEAF2(mp))
			removal_contrib = 1;
		else
			removal_contrib = mdb_leaf_entry_contribution(mp, leaf);
		if (mc->mc_top > 0) {
			parent_idx = mc->mc_ki[mc->mc_top - 1];
			parent = mc->mc_pg[mc->mc_top - 1];
			if (parent && IS_BRANCH(parent) && IS_COUNTED(parent) && parent_idx < NUMKEYS(parent)) {
				MDB_node *ref = NODEPTR(parent, parent_idx);
				if (NODEPGNO(ref) == mp->mp_pgno) {
					parent_ref = ref;
					parent_before = mdb_node_get_count(parent, ref);
					have_direct_parent = 1;
				}
			}
		}
	}
	mdb_node_del(mc, mc->mc_db->md_pad);
	mc->mc_db->md_entries--;
	if (counted && mc->mc_top > 0) {
		uint64_t leaf_after = prior_total;
		if (removal_contrib >= leaf_after)
			leaf_after = 0;
		else
			leaf_after -= removal_contrib;
		int64_t delta;
		if (have_direct_parent) {
			if (leaf_after != parent_before)
				mdb_node_set_count(parent, parent_ref, leaf_after);
			delta = (int64_t)leaf_after - (int64_t)parent_before;
		} else {
			delta = mdb_update_parent_count(parent, mp->mp_pgno, leaf_after);
			if (!delta && removal_contrib)
				delta = - (int64_t)removal_contrib;
			if (!delta)
				delta = (int64_t)leaf_after - (int64_t)prior_total;
		}
		if (delta)
			mdb_propagate_count_delta(mc, mc->mc_top - 2, delta);
	}
	{
		/* Adjust other cursors pointing to mp */
		for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
			m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
			if (! (m2->mc_flags & m3->mc_flags & C_INITIALIZED))
				continue;
			if (m3 == mc || m3->mc_snum < mc->mc_snum)
				continue;
			if (m3->mc_pg[mc->mc_top] == mp) {
				if (m3->mc_ki[mc->mc_top] == ki) {
					m3->mc_flags |= C_DEL;
					if (mc->mc_db->md_flags & MDB_DUPSORT) {
						/* Sub-cursor referred into dataset which is gone */
						m3->mc_xcursor->mx_cursor.mc_flags &= ~(C_INITIALIZED|C_EOF);
					}
					continue;
				} else if (m3->mc_ki[mc->mc_top] > ki) {
					m3->mc_ki[mc->mc_top]--;
				}
				XCURSOR_REFRESH(m3, mc->mc_top, mp);
			}
		}
	}
	rc = mdb_rebalance(mc);
	if (rc) {
		goto fail;
	}

	/* DB is totally empty now, just bail out.
	 * Other cursors adjustments were already done
	 * by mdb_rebalance and aren't needed here.
	 */
	if (!mc->mc_snum) {
		mc->mc_flags |= C_EOF;
		goto done;
	}

	mp = mc->mc_pg[mc->mc_top];
	nkeys = NUMKEYS(mp);

	/* Adjust other cursors pointing to mp */
	for (m2 = mc->mc_txn->mt_cursors[dbi]; !rc && m2; m2=m2->mc_next) {
		m3 = (mc->mc_flags & C_SUB) ? &m2->mc_xcursor->mx_cursor : m2;
		if (!(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
			continue;
		if (m3->mc_snum < mc->mc_snum)
			continue;
		if (m3->mc_pg[mc->mc_top] == mp) {
			if (m3->mc_ki[mc->mc_top] >= mc->mc_ki[mc->mc_top]) {
			/* if m3 points past last node in page, find next sibling */
				if (m3->mc_ki[mc->mc_top] >= nkeys) {
					rc = mdb_cursor_sibling(m3, 1);
					if (rc == MDB_NOTFOUND) {
						m3->mc_flags |= C_EOF;
						rc = MDB_SUCCESS;
						continue;
					}
					if (rc)
						goto fail;
				}
				if (m3->mc_xcursor && !(m3->mc_flags & C_EOF)) {
					MDB_node *node = NODEPTR(m3->mc_pg[mc->mc_top], m3->mc_ki[mc->mc_top]);
					/* If this node has dupdata, it may need to be reinited
					 * because its data has moved.
					 * If the xcursor was not initd it must be reinited.
					 * Else if node points to a subDB, nothing is needed.
					 * Else (xcursor was initd, not a subDB) needs mc_pg[0] reset.
					 */
					if (node->mn_flags & F_DUPDATA) {
						if (m3->mc_xcursor->mx_cursor.mc_flags & C_INITIALIZED) {
							if (!(node->mn_flags & F_SUBDATA))
								m3->mc_xcursor->mx_cursor.mc_pg[0] = NODEDATA(node);
						} else {
							mdb_xcursor_init1(m3, node);
							rc = mdb_cursor_first(&m3->mc_xcursor->mx_cursor, NULL, NULL);
							if (rc)
								goto fail;
						}
					}
					m3->mc_xcursor->mx_cursor.mc_flags |= C_DEL;
				}
			}
		}
	}
	mc->mc_flags |= C_DEL;
	goto done;

fail:
	if (rc)
		mdb_txn_mark_error(mc->mc_txn, rc);
done:
	return rc;
}

int
mdb_del(MDB_txn *txn, MDB_dbi dbi,
	MDB_val *key, MDB_val *data)
{
	DKBUF;
	DDBUF;
	if (!key || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	if (!F_ISSET(txn->mt_dbs[dbi].md_flags, MDB_DUPSORT)) {
		/* must ignore any data */
		data = NULL;
	}

	MDB_TRACE(("%p, %u, %"Z"u[%s], %"Z"u%s",
		txn, dbi, key ? key->mv_size:0, DKEY(key), data ? data->mv_size:0,
		data ? mdb_dval(txn, dbi, data, dbuf):""));
	return mdb_del0(txn, dbi, key, data, 0);
}

static int
mdb_del0(MDB_txn *txn, MDB_dbi dbi,
	MDB_val *key, MDB_val *data, unsigned flags)
{
	MDB_cursor mc = (MDB_cursor){0};
	MDB_xcursor mx = (MDB_xcursor){0};
	MDB_cursor_op op;
	MDB_val rdata, *xdata;
	int		 rc, exact = 0;
	DKBUF;

	DPRINTF(("====> delete db %u key [%s]", dbi, DKEY(key)));

	mdb_cursor_init(&mc, txn, dbi, &mx);

	if (data) {
		op = MDB_GET_BOTH;
		rdata = *data;
		xdata = &rdata;
	} else {
		op = MDB_SET;
		xdata = NULL;
		flags |= MDB_NODUPDATA;
	}
	rc = mdb_cursor_set(&mc, key, xdata, op, &exact);
	if (rc == 0) {
		/* let mdb_page_split know about this cursor if needed:
		 * delete will trigger a rebalance; if it needs to move
		 * a node from one page to another, it will have to
		 * update the parent's separator key(s). If the new sepkey
		 * is larger than the current one, the parent page may
		 * run out of space, triggering a split. We need this
		 * cursor to be consistent until the end of the rebalance.
		 */
		mc.mc_next = txn->mt_cursors[dbi];
		txn->mt_cursors[dbi] = &mc;
		rc = _mdb_cursor_del(&mc, flags);
		txn->mt_cursors[dbi] = mc.mc_next;
	}
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	return rc;
}

/** Split a page and insert a new node.
 * Set #MDB_TXN_ERROR on failure.
 * @param[in,out] mc Cursor pointing to the page and desired insertion index.
 * The cursor will be updated to point to the actual page and index where
 * the node got inserted after the split.
 * @param[in] newkey The key for the newly inserted node.
 * @param[in] newdata The data for the newly inserted node.
 * @param[in] newpgno The page number, if the new node is a branch node.
 * @param[in] nflags The #NODE_ADD_FLAGS for the new node.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_page_split(MDB_cursor *mc, MDB_val *newkey, MDB_val *newdata, pgno_t newpgno,
	unsigned int nflags)
{
	unsigned int flags;
	int		 rc = MDB_SUCCESS, new_root = 0, did_split = 0;
	indx_t		 newindx;
	pgno_t		 pgno = 0;
	int	 i, j, split_indx, nkeys, pmax;
	MDB_env 	*env = mc->mc_txn->mt_env;
	MDB_node	*node;
	MDB_val	 sepkey, rkey, xdata, *rdata = &xdata;
	MDB_page	*copy = NULL;
	MDB_page	*mp, *rp, *pp;
	int ptop;
	MDB_cursor	mn;
	MDB_prefix_rebuild_entry *prefix_split_entries = NULL;
	unsigned int prefix_split_total = 0;
	int prefix_split_ready = 0;
	int		 track_counts = 0;
	uint64_t	pre_split_total = MDB_COUNT_HINT_NONE;
	uint64_t	 left_tally = 0, right_tally = 0;
	int		 have_left_tally = 0, have_right_tally = 0;
	DKBUF;

	mp = mc->mc_pg[mc->mc_top];
	if (mc->mc_txn)
		mdb_prefix_stride_entry_invalidate(mc->mc_txn, mp->mp_pgno);
	if (mc->mc_txn) {
		mc->mc_txn->mt_prefix.measure_cache.valid = 0;
		mc->mc_txn->mt_prefix.measure_cache.snapshot = NULL;
		mc->mc_txn->mt_prefix.measure_cache.snapshot_bytes = 0;
	}
	newindx = mc->mc_ki[mc->mc_top];
	nkeys = NUMKEYS(mp);
	if (mc->mc_db->md_flags & MDB_COUNTED)
		track_counts = 1;
	if (track_counts) {
		if (mc->mc_top > 0) {
			MDB_page *parent = mc->mc_pg[mc->mc_top - 1];
			if (parent && IS_BRANCH(parent) && IS_COUNTED(parent)) {
				indx_t parent_idx = mc->mc_ki[mc->mc_top - 1];
				if (parent_idx < NUMKEYS(parent)) {
					MDB_node *pnode = NODEPTR(parent, parent_idx);
					pre_split_total = mdb_node_get_count(parent, pnode);
				}
			}
		} else {
			pre_split_total = mdb_page_subtree_count(mp);
		}
	}

	DPRINTF(("-----> splitting %s page %"Yu" and adding [%s] at index %i/%i",
	    IS_LEAF(mp) ? "leaf" : "branch", mp->mp_pgno,
	    DKEY(newkey), mc->mc_ki[mc->mc_top], nkeys));

	/* Create a right sibling. */
	if ((rc = mdb_page_new(mc, mp->mp_flags, 1, &rp)))
		return rc;
	if (mc->mc_txn)
		mdb_prefix_stride_entry_invalidate(mc->mc_txn, rp->mp_pgno);
	rp->mp_pad = mp->mp_pad;
	DPRINTF(("new right sibling: page %"Yu, rp->mp_pgno));

	/* Usually when splitting the root page, the cursor
	 * height is 1. But when called from mdb_update_key,
	 * the cursor height may be greater because it walks
	 * up the stack while finding the branch slot to update.
	 */
	if (mc->mc_top < 1) {
		if ((rc = mdb_page_new(mc, P_BRANCH, 1, &pp)))
			goto done;
		/* shift current top to make room for new parent */
		for (i=mc->mc_snum; i>0; i--) {
			mc->mc_pg[i] = mc->mc_pg[i-1];
			mc->mc_ki[i] = mc->mc_ki[i-1];
		}
		mc->mc_pg[0] = pp;
		mc->mc_ki[0] = 0;
		mc->mc_db->md_root = pp->mp_pgno;
		DPRINTF(("root split! new root = %"Yu, pp->mp_pgno));
		new_root = mc->mc_db->md_depth++;

		/* Add left (implicit) pointer. */
		uint64_t root_hint = (track_counts && pre_split_total != MDB_COUNT_HINT_NONE)
		    ? pre_split_total : MDB_COUNT_HINT_NONE;
		if ((rc = mdb_node_add(mc, 0, NULL, NULL, mp->mp_pgno, 0,
		    mp, root_hint)) != MDB_SUCCESS) {
			/* undo the pre-push */
			mc->mc_pg[0] = mc->mc_pg[1];
			mc->mc_ki[0] = mc->mc_ki[1];
			mc->mc_db->md_root = mp->mp_pgno;
			mc->mc_db->md_depth--;
			goto done;
		}
		mc->mc_snum++;
		mc->mc_top++;
		ptop = 0;
	} else {
		ptop = mc->mc_top-1;
		DPRINTF(("parent branch page is %"Yu, mc->mc_pg[ptop]->mp_pgno));
	}

	mdb_cursor_copy(mc, &mn);
	mn.mc_xcursor = NULL;
	mn.mc_pg[mn.mc_top] = rp;
	mn.mc_ki[ptop] = mc->mc_ki[ptop]+1;

	if (nflags & MDB_APPEND) {
		mn.mc_ki[mn.mc_top] = 0;
		sepkey = *newkey;
		split_indx = newindx;
		nkeys = 0;
	} else {

		split_indx = (nkeys+1) / 2;

		if (IS_LEAF2(rp)) {
			char *split, *ins;
			int x;
			unsigned int lsize, rsize, ksize;
			/* Move half of the keys to the right sibling */
			x = mc->mc_ki[mc->mc_top] - split_indx;
			ksize = mc->mc_db->md_pad;
			split = LEAF2KEY(mp, split_indx, ksize);
			rsize = (nkeys - split_indx) * ksize;
			lsize = (nkeys - split_indx) * sizeof(indx_t);
			mp->mp_lower -= lsize;
			rp->mp_lower += lsize;
			mp->mp_upper += rsize - lsize;
			rp->mp_upper -= rsize - lsize;
			sepkey.mv_size = ksize;
			if (newindx == split_indx) {
				sepkey.mv_data = newkey->mv_data;
			} else {
				sepkey.mv_data = split;
			}
			if (x<0) {
				ins = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], ksize);
				memcpy(rp->mp_ptrs, split, rsize);
				sepkey.mv_data = rp->mp_ptrs;
				memmove(ins+ksize, ins, (split_indx - mc->mc_ki[mc->mc_top]) * ksize);
				memcpy(ins, newkey->mv_data, ksize);
				mp->mp_lower += sizeof(indx_t);
				mp->mp_upper -= ksize - sizeof(indx_t);
			} else {
				if (x)
					memcpy(rp->mp_ptrs, split, x * ksize);
				ins = LEAF2KEY(rp, x, ksize);
				memcpy(ins, newkey->mv_data, ksize);
				memcpy(ins+ksize, split + x * ksize, rsize - x * ksize);
				rp->mp_lower += sizeof(indx_t);
				rp->mp_upper -= ksize - sizeof(indx_t);
				mc->mc_ki[mc->mc_top] = x;
			}
			} else {
				int psize, nsize, k, keythresh;

				/* Maximum free space in an empty page */
				pmax = env->me_psize - PAGEHDRSZ;
			/* Threshold number of keys considered "small" */
			keythresh = env->me_psize >> 7;

				if (IS_LEAF(mp)) {
					int prefix_enabled = (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION) != 0;
					nsize = mdb_leaf_size(env, mp, newindx, newkey, newdata, prefix_enabled);
					if (prefix_enabled) {
						rc = mdb_prefix_leaf_split_collect_entries(mc, mp, newindx,
						    newkey, newdata, nflags,
						    &prefix_split_entries, &prefix_split_total);
						if (rc != MDB_SUCCESS)
							goto done;
						prefix_split_ready = 1;
					}
				} else
					nsize = mdb_branch_size(env, mp, newkey);
				nsize = EVEN(nsize);

			/* grab a page to hold a temporary copy */
			copy = mdb_page_malloc(mc->mc_txn, 1);
			if (copy == NULL) {
				rc = ENOMEM;
				goto done;
			}
			copy->mp_pgno  = mp->mp_pgno;
			copy->mp_flags = mp->mp_flags;
			copy->mp_lower = (PAGEHDRSZ-PAGEBASE);
			copy->mp_upper = env->me_psize - PAGEBASE;

			/* prepare to insert */
			for (i=0, j=0; i<nkeys; i++) {
				if (i == newindx) {
					copy->mp_ptrs[j++] = 0;
				}
				copy->mp_ptrs[j++] = mp->mp_ptrs[i];
			}

			/* When items are relatively large the split point needs
			 * to be checked, because being off-by-one will make the
			 * difference between success or failure in mdb_node_add.
			 *
			 * It's also relevant if a page happens to be laid out
			 * such that one half of its nodes are all "small" and
			 * the other half of its nodes are "large." If the new
			 * item is also "large" and falls on the half with
			 * "large" nodes, it also may not fit.
			 *
			 * As a final tweak, if the new item goes on the last
			 * spot on the page (and thus, onto the new page), bias
			 * the split so the new page is emptier than the old page.
			 * This yields better packing during sequential inserts.
			 */
				if (nkeys < keythresh || nsize > pmax/16 || newindx >= nkeys) {
					/* Find split point */
					psize = 0;
					if (newindx <= split_indx || newindx >= nkeys) {
						i = 0; j = 1;
						k = newindx >= nkeys ? nkeys : split_indx+1+IS_LEAF(mp);
					} else {
						i = nkeys; j = -1;
						k = split_indx-1;
					}
					for (; i!=k; i+=j) {
						if (i == newindx) {
							psize += nsize;
							node = NULL;
						} else {
							node = (MDB_node *)((char *)mp + copy->mp_ptrs[i] + PAGEBASE);
							psize += NODESIZE + NODEKSZ(node) + sizeof(indx_t);
							if (IS_LEAF(mp)) {
								if (F_ISSET(node->mn_flags, F_BIGDATA))
									psize += sizeof(pgno_t);
								else
									psize += NODEDSZ(node);
							}
							psize = EVEN(psize);
						}
						if (psize > pmax || i == k-j) {
							split_indx = i + (j<0);
							break;
						}
					}
				}
					if (prefix_split_ready) {
						rc = mdb_prefix_leaf_adjust_split_index(mc,
						    prefix_split_entries, prefix_split_total,
						    newindx, &split_indx);
						if (rc != MDB_SUCCESS)
							goto done;
					}
				if (split_indx == newindx) {
					sepkey.mv_size = newkey->mv_size;
					sepkey.mv_data = newkey->mv_data;
			} else {
				if (prefix_split_ready) {
					if (split_indx < 0 || (unsigned int)split_indx >= prefix_split_total) {
						rc = MDB_PROBLEM;
						goto done;
					}
					if (prefix_split_entries[split_indx].key.mv_size > MDB_KEYBUF_MAX) {
						rc = MDB_BAD_VALSIZE;
						goto done;
					}
					memcpy(mc->mc_keybuf, prefix_split_entries[split_indx].key.mv_data,
					    prefix_split_entries[split_indx].key.mv_size);
					mc->mc_key.mv_size = prefix_split_entries[split_indx].key.mv_size;
					mc->mc_key.mv_data = mc->mc_keybuf;
					sepkey = mc->mc_key;
				} else {
					node = (MDB_node *)((char *)mp + copy->mp_ptrs[split_indx] + PAGEBASE);
					if (IS_LEAF(mp) && (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION)) {
						if (split_indx == 0) {
							if (node->mn_ksize > MDB_KEYBUF_MAX)
								return MDB_BAD_VALSIZE;
							memcpy(mc->mc_keybuf, NODEKEY(mp, node), node->mn_ksize);
							mc->mc_key.mv_size = node->mn_ksize;
							mc->mc_key.mv_data = mc->mc_keybuf;
							sepkey = mc->mc_key;
						} else {
							MDB_node *trunk = NODEPTR(mp, 0);
							MDB_val trunkv = { trunk->mn_ksize, NODEKEY(mp, trunk) };
							int rc2 = mdb_leaf_decode_key(&trunkv, NODEKEY(mp, node), node->mn_ksize,
							    &mc->mc_key, mc->mc_keybuf, MDB_KEYBUF_MAX, 0, NULL);
							if (rc2 != MDB_SUCCESS)
								return rc2;
							sepkey = mc->mc_key;
						}
					} else {
						sepkey.mv_size = node->mn_ksize;
						sepkey.mv_data = NODEKEY(mp, node);
					}
				}
			}
			}
		}

	DPRINTF(("separator is %d [%s]", split_indx, DKEY(&sepkey)));

	/* Copy separator key to the parent.
	 */
	if (SIZELEFT(mn.mc_pg[ptop]) < mdb_branch_size(env, mn.mc_pg[ptop], &sepkey)) {
		int snum = mc->mc_snum;
		mn.mc_snum--;
		mn.mc_top--;
		did_split = 1;
		/* We want other splits to find mn when doing fixups */
		WITH_CURSOR_TRACKING(mn,
			rc = mdb_page_split(&mn, &sepkey, NULL, rp->mp_pgno, 0));
		if (rc)
			goto done;

		/* root split? */
		if (mc->mc_snum > snum) {
			ptop++;
		}
		/* Right page might now have changed parent.
		 * Check if left page also changed parent.
		 */
		if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
		    mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop])) {
			for (i=0; i<ptop; i++) {
				mc->mc_pg[i] = mn.mc_pg[i];
				mc->mc_ki[i] = mn.mc_ki[i];
			}
			mc->mc_pg[ptop] = mn.mc_pg[ptop];
			if (mn.mc_ki[ptop]) {
				mc->mc_ki[ptop] = mn.mc_ki[ptop] - 1;
			} else {
				/* find right page's left sibling */
				mc->mc_ki[ptop] = mn.mc_ki[ptop];
				rc = mdb_cursor_sibling(mc, 0);
			}
		}
	} else {
		mn.mc_top--;
		rc = mdb_node_add(&mn, mn.mc_ki[ptop], &sepkey, NULL, rp->mp_pgno, 0,
		    rp, MDB_COUNT_HINT_NONE);
		mn.mc_top++;
	}
		if (rc != MDB_SUCCESS) {
			if (rc == MDB_NOTFOUND) /* improper mdb_cursor_sibling() result */
				rc = MDB_PROBLEM;
			goto done;
		}
		if (prefix_split_ready && did_split && !(nflags & MDB_APPEND) && !IS_LEAF2(mp)) {
			rc = mdb_prefix_leaf_split_collect_entries(mc, mp, newindx,
			    newkey, newdata, nflags,
			    &prefix_split_entries, &prefix_split_total);
			if (rc != MDB_SUCCESS)
				goto done;
		}
		if (nflags & MDB_APPEND) {
			mc->mc_pg[mc->mc_top] = rp;
			mc->mc_ki[mc->mc_top] = 0;
		rc = mdb_node_add(mc, 0, newkey, newdata, newpgno, nflags,
		    NULL, MDB_COUNT_HINT_NONE);
		if (rc)
			goto done;
		if (track_counts) {
			MDB_page *dest = mc->mc_pg[mc->mc_top];
			MDB_node *added = NODEPTR(dest, 0);
			uint64_t contrib = 0;
			if (IS_BRANCH(dest) && IS_COUNTED(dest))
				contrib = mdb_node_get_count(dest, added);
			else if (IS_LEAF(dest))
				contrib = mdb_leaf_entry_contribution(dest, added);
			else if (IS_LEAF2(dest))
				contrib = 1;
			right_tally += contrib;
			have_right_tally = 1;
			if (!have_left_tally && pre_split_total != MDB_COUNT_HINT_NONE) {
				left_tally = pre_split_total;
				have_left_tally = 1;
			}
		}
		for (i=0; i<mc->mc_top; i++)
			mc->mc_ki[i] = mn.mc_ki[i];
	} else if (!IS_LEAF2(mp)) {
		/* Move nodes */
		mc->mc_pg[mc->mc_top] = rp;
		i = split_indx;
		j = 0;
		do {
			if (i == newindx) {
				rkey.mv_data = newkey->mv_data;
				rkey.mv_size = newkey->mv_size;
				if (IS_LEAF(mp)) {
					rdata = newdata;
				} else
					pgno = newpgno;
				flags = nflags;
				/* Update index for the new key. */
				mc->mc_ki[mc->mc_top] = j;
				} else {
					if (prefix_split_ready && IS_LEAF(mp)) {
						MDB_prefix_rebuild_entry *entry;
						if (i < 0 || (unsigned int)i >= prefix_split_total) {
							rc = MDB_PROBLEM;
							goto done;
						}
						entry = &prefix_split_entries[i];
						rkey = entry->key;
						xdata.mv_data = (void *)entry->data_ptr;
						xdata.mv_size = entry->data_size;
						rdata = &xdata;
						flags = entry->flags;
					} else {
						node = (MDB_node *)((char *)mp + copy->mp_ptrs[i] + PAGEBASE);
						if (IS_LEAF(mp) && (mc->mc_db->md_flags & MDB_PREFIX_COMPRESSION)) {
							int src_idx = (i < newindx) ? (int)i : (int)i - 1;
							if (src_idx == 0) {
								if (node->mn_ksize > MDB_KEYBUF_MAX) {
									rc = MDB_BAD_VALSIZE;
									goto done;
								}
								memcpy(mc->mc_keybuf, NODEKEY(mp, node), node->mn_ksize);
								mc->mc_key.mv_size = node->mn_ksize;
								mc->mc_key.mv_data = mc->mc_keybuf;
								rkey = mc->mc_key;
							} else {
								MDB_node *trunk = NODEPTR(mp, 0);
								MDB_val trunkv = { trunk->mn_ksize, NODEKEY(mp, trunk) };
								int rc2 = mdb_leaf_decode_key(&trunkv, NODEKEY(mp, node), node->mn_ksize,
								    &mc->mc_key, mc->mc_keybuf, MDB_KEYBUF_MAX, 0, NULL);
								if (rc2 != MDB_SUCCESS) {
									rc = rc2;
									goto done;
								}
								rkey = mc->mc_key;
							}
						} else {
							rkey.mv_data = NODEKEY(mp, node);
							rkey.mv_size = node->mn_ksize;
						}
						if (IS_LEAF(mp)) {
							xdata.mv_data = NODEDATA(node);
							xdata.mv_size = NODEDSZ(node);
							rdata = &xdata;
						} else
							pgno = NODEPGNO(node);
						flags = node->mn_flags;
					}
				}

			if (!IS_LEAF(mp) && j == 0) {
				/* First branch index doesn't need key data. */
				rkey.mv_size = 0;
			}

			MDB_page *dest = mc->mc_pg[mc->mc_top];
			indx_t dest_index = j;
			uint64_t child_hint = MDB_COUNT_HINT_NONE;
			if (!IS_LEAF(mp) && IS_BRANCH(dest) && IS_COUNTED(dest) && i != newindx)
				child_hint = mdb_node_get_count(mp, node);
			rc = mdb_node_add(mc, j, &rkey, rdata, pgno, flags,
			    NULL, child_hint);
			if (rc)
				goto done;
			if (track_counts) {
				uint64_t contrib = 0;
				MDB_node *added = NODEPTR(dest, dest_index);
				if (IS_BRANCH(dest) && IS_COUNTED(dest))
					contrib = mdb_node_get_count(dest, added);
				else if (IS_LEAF(dest))
					contrib = mdb_leaf_entry_contribution(dest, added);
				else if (IS_LEAF2(dest))
					contrib = 1;
				if (dest == rp) {
					right_tally += contrib;
					have_right_tally = 1;
				} else if (dest == copy || dest == mp) {
					left_tally += contrib;
					have_left_tally = 1;
				}
			}
			if (i == nkeys) {
				i = 0;
				j = 0;
				mc->mc_pg[mc->mc_top] = copy;
			} else {
				i++;
				j++;
			}
		} while (i != split_indx);

		nkeys = NUMKEYS(copy);
		for (i=0; i<nkeys; i++)
			mp->mp_ptrs[i] = copy->mp_ptrs[i];
		mp->mp_lower = copy->mp_lower;
		mp->mp_upper = copy->mp_upper;
		memcpy(NODEPTR(mp, nkeys-1), NODEPTR(copy, nkeys-1),
			env->me_psize - copy->mp_upper - PAGEBASE);

		/* reset back to original page */
		if (newindx < split_indx) {
			mc->mc_pg[mc->mc_top] = mp;
		} else {
			mc->mc_pg[mc->mc_top] = rp;
			mc->mc_ki[ptop]++;
			/* Make sure mc_ki is still valid.
			 */
			if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
				mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop])) {
				for (i=0; i<=ptop; i++) {
					mc->mc_pg[i] = mn.mc_pg[i];
					mc->mc_ki[i] = mn.mc_ki[i];
				}
			}
		}
		if (nflags & MDB_RESERVE) {
			node = NODEPTR(mc->mc_pg[mc->mc_top], mc->mc_ki[mc->mc_top]);
			if (!(node->mn_flags & F_BIGDATA))
				newdata->mv_data = NODEDATA(node);
		}
	} else {
		if (newindx >= split_indx) {
			mc->mc_pg[mc->mc_top] = rp;
			mc->mc_ki[ptop]++;
			/* Make sure mc_ki is still valid.
			 */
			if (mn.mc_pg[ptop] != mc->mc_pg[ptop] &&
				mc->mc_ki[ptop] >= NUMKEYS(mc->mc_pg[ptop])) {
				for (i=0; i<=ptop; i++) {
					mc->mc_pg[i] = mn.mc_pg[i];
					mc->mc_ki[i] = mn.mc_ki[i];
				}
			}
		}
	}

	if (track_counts && IS_LEAF2(mp)) {
		left_tally = NUMKEYS(mp);
		right_tally = NUMKEYS(rp);
		have_left_tally = 1;
		have_right_tally = 1;
	}

	{
		if (track_counts && mc->mc_top > 0) {
			MDB_page *parent = mc->mc_pg[ptop];
			if (parent) {
				uint64_t left_total = left_tally;
				uint64_t right_total = right_tally;
				if (!have_left_tally)
					left_total = mdb_page_subtree_count(mp);
				if (!have_right_tally)
					right_total = mdb_page_subtree_count(rp);
				int64_t delta_left = mdb_update_parent_count(parent, mp->mp_pgno, left_total);
				int64_t delta_right = mdb_update_parent_count(parent, rp->mp_pgno, right_total);
				int64_t delta = delta_left + delta_right;
				if (delta)
					mdb_propagate_count_delta(mc, ptop-1, delta);
			}
		}

		/* Adjust other cursors pointing to mp */
		MDB_cursor *m2, *m3;
		MDB_dbi dbi = mc->mc_dbi;
		nkeys = NUMKEYS(mp);

		for (m2 = mc->mc_txn->mt_cursors[dbi]; m2; m2=m2->mc_next) {
			if (mc->mc_flags & C_SUB)
				m3 = &m2->mc_xcursor->mx_cursor;
			else
				m3 = m2;
			if (m3 == mc)
				continue;
			if (!(m2->mc_flags & m3->mc_flags & C_INITIALIZED))
				continue;
			if (new_root) {
				int k;
				/* sub cursors may be on different DB */
				if (m3->mc_pg[0] != mp)
					continue;
				/* root split */
				for (k=new_root; k>=0; k--) {
					m3->mc_ki[k+1] = m3->mc_ki[k];
					m3->mc_pg[k+1] = m3->mc_pg[k];
				}
				if (m3->mc_ki[0] >= nkeys) {
					m3->mc_ki[0] = 1;
				} else {
					m3->mc_ki[0] = 0;
				}
				m3->mc_pg[0] = mc->mc_pg[0];
				m3->mc_snum++;
				m3->mc_top++;
			}
			if (m3->mc_top >= mc->mc_top && m3->mc_pg[mc->mc_top] == mp) {
				if (m3->mc_ki[mc->mc_top] >= newindx && !(nflags & MDB_SPLIT_REPLACE))
					m3->mc_ki[mc->mc_top]++;
				if (m3->mc_ki[mc->mc_top] >= nkeys) {
					m3->mc_pg[mc->mc_top] = rp;
					m3->mc_ki[mc->mc_top] -= nkeys;
					for (i=0; i<mc->mc_top; i++) {
						m3->mc_ki[i] = mn.mc_ki[i];
						m3->mc_pg[i] = mn.mc_pg[i];
					}
				}
			} else if (!did_split && m3->mc_top >= ptop && m3->mc_pg[ptop] == mc->mc_pg[ptop] &&
				m3->mc_ki[ptop] >= mc->mc_ki[ptop]) {
				m3->mc_ki[ptop]++;
			}
			if (IS_LEAF(mp))
				XCURSOR_REFRESH(m3, mc->mc_top, m3->mc_pg[mc->mc_top]);
		}
	}
	DPRINTF(("mp left: %d, rp left: %d", SIZELEFT(mp), SIZELEFT(rp)));

done:
	if (copy)					/* tmp page */
		mdb_page_free(env, copy);
	if (rc)
		mdb_txn_mark_error(mc->mc_txn, rc);
	return rc;
}

int
mdb_put(MDB_txn *txn, MDB_dbi dbi,
	MDB_val *key, MDB_val *data, unsigned int flags)
{
	MDB_cursor mc = (MDB_cursor){0};
	MDB_xcursor mx = (MDB_xcursor){0};
	int rc;
	DKBUF;
	DDBUF;

	if (!key || !data || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (flags & ~(MDB_NOOVERWRITE|MDB_NODUPDATA|MDB_RESERVE|MDB_APPEND|MDB_APPENDDUP))
		return EINVAL;

	if (txn->mt_flags & (MDB_TXN_RDONLY|MDB_TXN_BLOCKED))
		return (txn->mt_flags & MDB_TXN_RDONLY) ? EACCES : MDB_BAD_TXN;

	MDB_TRACE(("%p, %u, %"Z"u[%s], %"Z"u%s, %u",
		txn, dbi, key ? key->mv_size:0, DKEY(key), data->mv_size, mdb_dval(txn, dbi, data, dbuf), flags));
	mdb_cursor_init(&mc, txn, dbi, &mx);
	mc.mc_next = txn->mt_cursors[dbi];
	txn->mt_cursors[dbi] = &mc;
	rc = _mdb_cursor_put(&mc, key, data, flags);
	txn->mt_cursors[dbi] = mc.mc_next;
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	return rc;
}

#ifndef MDB_WBUF
#define MDB_WBUF	(1024*1024)
#endif
#define MDB_EOF		0x10	/**< #mdb_env_copyfd1() is done reading */

	/** State needed for a double-buffering compacting copy. */
typedef struct mdb_copy {
	MDB_env *mc_env;
	MDB_txn *mc_txn;
	pthread_mutex_t mc_mutex;
	pthread_cond_t mc_cond;	/**< Condition variable for #mc_new */
	char *mc_wbuf[2];
	char *mc_over[2];
	size_t mc_wlen[2];
	size_t mc_olen[2];
	pgno_t mc_next_pgno;
	HANDLE mc_fd;
	int mc_toggle;			/**< Buffer number in provider */
	int mc_new;				/**< (0-2 buffers to write) | (#MDB_EOF at end) */
	/** Error code.  Never cleared if set.  Both threads can set nonzero
	 *	to fail the copy.  Not mutex-protected, LMDB expects atomic int.
	 */
	volatile int mc_error;
} mdb_copy;

	/** Dedicated writer thread for compacting copy. */
static THREAD_RET ESECT CALL_CONV
mdb_env_copythr(void *arg)
{
	mdb_copy *my = arg;
	char *ptr;
	int toggle = 0, rc;
	size_t wsize;
#ifdef _WIN32
	DWORD len;
#define DO_WRITE(rc, fd, ptr, w2, len)	rc = WriteFile(fd, ptr, w2, &len, NULL)
#else
	int len;
#define DO_WRITE(rc, fd, ptr, w2, len)	len = write(fd, ptr, w2); rc = (len >= 0)
#ifdef SIGPIPE
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	if ((rc = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0)
		my->mc_error = rc;
#endif
#endif

	pthread_mutex_lock(&my->mc_mutex);
	for(;;) {
		while (!my->mc_new)
			pthread_cond_wait(&my->mc_cond, &my->mc_mutex);
		if (my->mc_new == 0 + MDB_EOF) /* 0 buffers, just EOF */
			break;
		wsize = my->mc_wlen[toggle];
		ptr = my->mc_wbuf[toggle];
again:
		rc = MDB_SUCCESS;
		while (wsize > 0 && !my->mc_error) {
			DO_WRITE(rc, my->mc_fd, ptr, wsize, len);
			if (!rc) {
				rc = ErrCode();
#if defined(SIGPIPE) && !defined(_WIN32)
				if (rc == EPIPE) {
					/* Collect the pending SIGPIPE, otherwise at least OS X
					 * gives it to the process on thread-exit (ITS#8504).
					 */
					int tmp;
					sigwait(&set, &tmp);
				}
#endif
				break;
			} else if (len > 0) {
				rc = MDB_SUCCESS;
				ptr += len;
				wsize -= len;
				continue;
			} else {
				rc = EIO;
				break;
			}
		}
		if (rc) {
			my->mc_error = rc;
		}
		/* If there's an overflow page tail, write it too */
		if (my->mc_olen[toggle]) {
			wsize = my->mc_olen[toggle];
			ptr = my->mc_over[toggle];
			my->mc_olen[toggle] = 0;
			goto again;
		}
		my->mc_wlen[toggle] = 0;
		toggle ^= 1;
		/* Return the empty buffer to provider */
		my->mc_new--;
		pthread_cond_signal(&my->mc_cond);
	}
	pthread_mutex_unlock(&my->mc_mutex);
	return (THREAD_RET)0;
#undef DO_WRITE
}

	/** Give buffer and/or #MDB_EOF to writer thread, await unused buffer.
	 *
	 * @param[in] my control structure.
	 * @param[in] adjust (1 to hand off 1 buffer) | (MDB_EOF when ending).
	 */
static int ESECT
mdb_env_cthr_toggle(mdb_copy *my, int adjust)
{
	pthread_mutex_lock(&my->mc_mutex);
	my->mc_new += adjust;
	pthread_cond_signal(&my->mc_cond);
	while (my->mc_new & 2)		/* both buffers in use */
		pthread_cond_wait(&my->mc_cond, &my->mc_mutex);
	pthread_mutex_unlock(&my->mc_mutex);

	my->mc_toggle ^= (adjust & 1);
	/* Both threads reset mc_wlen, to be safe from threading errors */
	my->mc_wlen[my->mc_toggle] = 0;
	return my->mc_error;
}

	/** Depth-first tree traversal for compacting copy.
	 * @param[in] my control structure.
	 * @param[in,out] pg database root.
	 * @param[in] flags includes #F_DUPDATA if it is a sorted-duplicate sub-DB.
	 */
static int ESECT
mdb_env_cwalk(mdb_copy *my, pgno_t *pg, int flags)
{
	MDB_cursor mc = {0};
	MDB_node *ni;
	MDB_page *mo, *mp, *leaf;
	char *buf, *ptr;
	int rc, toggle;
	unsigned int i;

	/* Empty DB, nothing to do */
	if (*pg == P_INVALID)
		return MDB_SUCCESS;

	mc.mc_snum = 1;
	mc.mc_txn = my->mc_txn;
	mc.mc_flags = my->mc_txn->mt_flags & (C_ORIG_RDONLY|C_WRITEMAP);

	rc = mdb_page_get(&mc, *pg, &mc.mc_pg[0], NULL);
	if (rc)
		return rc;
	rc = mdb_page_search_root(&mc, NULL, MDB_PS_FIRST);
	if (rc)
		return rc;

	/* Make cursor pages writable */
	buf = ptr = malloc(my->mc_env->me_psize * mc.mc_snum);
	if (buf == NULL)
		return ENOMEM;

	for (i=0; i<mc.mc_top; i++) {
		mdb_page_copy((MDB_page *)ptr, mc.mc_pg[i], my->mc_env->me_psize);
		mc.mc_pg[i] = (MDB_page *)ptr;
		ptr += my->mc_env->me_psize;
	}

	/* This is writable space for a leaf page. Usually not needed. */
	leaf = (MDB_page *)ptr;

	toggle = my->mc_toggle;
	while (mc.mc_snum > 0) {
		if (my->mc_env->me_interrupt) {
			rc = EINTR;
			goto done;
		}
		unsigned n;
		mp = mc.mc_pg[mc.mc_top];
		n = NUMKEYS(mp);

		if (IS_LEAF(mp)) {
			if (!IS_LEAF2(mp) && !(flags & F_DUPDATA)) {
				for (i=0; i<n; i++) {
					ni = NODEPTR(mp, i);
					if (ni->mn_flags & F_BIGDATA) {
						MDB_page *omp;
						pgno_t pg;

						/* Need writable leaf */
						if (mp != leaf) {
							mc.mc_pg[mc.mc_top] = leaf;
							mdb_page_copy(leaf, mp, my->mc_env->me_psize);
							mp = leaf;
							ni = NODEPTR(mp, i);
						}

						memcpy(&pg, NODEDATA(ni), sizeof(pg));
						memcpy(NODEDATA(ni), &my->mc_next_pgno, sizeof(pgno_t));
						rc = mdb_page_get(&mc, pg, &omp, NULL);
						if (rc)
							goto done;
						if (my->mc_wlen[toggle] >= MDB_WBUF) {
							rc = mdb_env_cthr_toggle(my, 1);
							if (rc)
								goto done;
							toggle = my->mc_toggle;
						}
						mo = (MDB_page *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
						memcpy(mo, omp, my->mc_env->me_psize);
						mo->mp_pgno = my->mc_next_pgno;
						my->mc_next_pgno += omp->mp_pages;
						my->mc_wlen[toggle] += my->mc_env->me_psize;
						if (omp->mp_pages > 1) {
							my->mc_olen[toggle] = my->mc_env->me_psize * (omp->mp_pages - 1);
							my->mc_over[toggle] = (char *)omp + my->mc_env->me_psize;
							rc = mdb_env_cthr_toggle(my, 1);
							if (rc)
								goto done;
							toggle = my->mc_toggle;
						}
					} else if (ni->mn_flags & F_SUBDATA) {
						MDB_db db;

						/* Need writable leaf */
						if (mp != leaf) {
							mc.mc_pg[mc.mc_top] = leaf;
							mdb_page_copy(leaf, mp, my->mc_env->me_psize);
							mp = leaf;
							ni = NODEPTR(mp, i);
						}

						memcpy(&db, NODEDATA(ni), sizeof(db));
						my->mc_toggle = toggle;
						rc = mdb_env_cwalk(my, &db.md_root, ni->mn_flags & F_DUPDATA);
						if (rc)
							goto done;
						toggle = my->mc_toggle;
						memcpy(NODEDATA(ni), &db, sizeof(db));
					}
				}
			}
		} else {
			mc.mc_ki[mc.mc_top]++;
			if (mc.mc_ki[mc.mc_top] < n) {
				pgno_t pg;
again:
				ni = NODEPTR(mp, mc.mc_ki[mc.mc_top]);
				pg = NODEPGNO(ni);
				rc = mdb_page_get(&mc, pg, &mp, NULL);
				if (rc)
					goto done;
				mc.mc_top++;
				mc.mc_snum++;
				mc.mc_ki[mc.mc_top] = 0;
				if (IS_BRANCH(mp)) {
					/* Whenever we advance to a sibling branch page,
					 * we must proceed all the way down to its first leaf.
					 */
					mdb_page_copy(mc.mc_pg[mc.mc_top], mp, my->mc_env->me_psize);
					goto again;
				} else
					mc.mc_pg[mc.mc_top] = mp;
				continue;
			}
		}
		if (my->mc_wlen[toggle] >= MDB_WBUF) {
			rc = mdb_env_cthr_toggle(my, 1);
			if (rc)
				goto done;
			toggle = my->mc_toggle;
		}
		mo = (MDB_page *)(my->mc_wbuf[toggle] + my->mc_wlen[toggle]);
		mdb_page_copy(mo, mp, my->mc_env->me_psize);
		mo->mp_pgno = my->mc_next_pgno++;
		my->mc_wlen[toggle] += my->mc_env->me_psize;
		if (mc.mc_top) {
			/* Update parent if there is one */
			ni = NODEPTR(mc.mc_pg[mc.mc_top-1], mc.mc_ki[mc.mc_top-1]);
			SETPGNO(ni, mo->mp_pgno);
			mdb_cursor_pop(&mc);
		} else {
			/* Otherwise we're done */
			*pg = mo->mp_pgno;
			break;
		}
	}
done:
	free(buf);
	return rc;
}

	/** Copy environment with compaction. */
static int ESECT
mdb_env_copyfd1(MDB_env *env, HANDLE fd)
{
	MDB_meta *mm;
	MDB_page *mp;
	mdb_copy my = {0};
	MDB_txn *txn = NULL;
	pthread_t thr;
	pgno_t root, new_root;
	int rc = MDB_SUCCESS;

#ifdef _WIN32
	if (!(my.mc_mutex = CreateMutex(NULL, FALSE, NULL)) ||
		!(my.mc_cond = CreateEvent(NULL, FALSE, FALSE, NULL))) {
		rc = ErrCode();
		goto done;
	}
	my.mc_wbuf[0] = _aligned_malloc(MDB_WBUF*2, env->me_os_psize);
	if (my.mc_wbuf[0] == NULL) {
		/* _aligned_malloc() sets errno, but we use Windows error codes */
		rc = ERROR_NOT_ENOUGH_MEMORY;
		goto done;
	}
#else
	if ((rc = pthread_mutex_init(&my.mc_mutex, NULL)) != 0)
		return rc;
	if ((rc = pthread_cond_init(&my.mc_cond, NULL)) != 0)
		goto done2;
#ifdef HAVE_MEMALIGN
	my.mc_wbuf[0] = memalign(env->me_os_psize, MDB_WBUF*2);
	if (my.mc_wbuf[0] == NULL) {
		rc = errno;
		goto done;
	}
#else
	{
		void *p;
		if ((rc = posix_memalign(&p, env->me_os_psize, MDB_WBUF*2)) != 0)
			goto done;
		my.mc_wbuf[0] = p;
	}
#endif
#endif
	memset(my.mc_wbuf[0], 0, MDB_WBUF*2);
	my.mc_wbuf[1] = my.mc_wbuf[0] + MDB_WBUF;
	my.mc_next_pgno = NUM_METAS;
	my.mc_env = env;
	my.mc_fd = fd;
	rc = THREAD_CREATE(thr, mdb_env_copythr, &my);
	if (rc)
		goto done;

	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if (rc)
		goto finish;

	mp = (MDB_page *)my.mc_wbuf[0];
	memset(mp, 0, NUM_METAS * env->me_psize);
	mp->mp_pgno = 0;
	mp->mp_flags = P_META;
	mm = (MDB_meta *)METADATA(mp);
	mdb_env_init_meta0(env, mm);
	mm->mm_address = env->me_metas[0]->mm_address;

	mp = (MDB_page *)(my.mc_wbuf[0] + env->me_psize);
	mp->mp_pgno = 1;
	mp->mp_flags = P_META;
	*(MDB_meta *)METADATA(mp) = *mm;
	mm = (MDB_meta *)METADATA(mp);

	/* Set metapage 1 with current main DB */
	root = new_root = txn->mt_dbs[MAIN_DBI].md_root;
	if (root != P_INVALID) {
		/* Count free pages + freeDB pages.  Subtract from last_pg
		 * to find the new last_pg, which also becomes the new root.
		 */
		MDB_ID freecount = 0;
			MDB_cursor mc = (MDB_cursor){0};
		MDB_xcursor mx = (MDB_xcursor){0};
		MDB_val key, data;
		mdb_cursor_init(&mc, txn, FREE_DBI, &mx);
		while ((rc = mdb_cursor_get(&mc, &key, &data, MDB_NEXT)) == 0)
			freecount += *(MDB_ID *)data.mv_data;
		if (rc != MDB_NOTFOUND)
			goto finish;
		freecount += txn->mt_dbs[FREE_DBI].md_branch_pages +
			txn->mt_dbs[FREE_DBI].md_leaf_pages +
			txn->mt_dbs[FREE_DBI].md_overflow_pages;

		new_root = txn->mt_next_pgno - 1 - freecount;
		mm->mm_last_pg = new_root;
		mm->mm_dbs[MAIN_DBI] = txn->mt_dbs[MAIN_DBI];
		mm->mm_dbs[MAIN_DBI].md_root = new_root;
	} else {
		/* When the DB is empty, handle it specially to
		 * fix any breakage like page leaks from ITS#8174.
		 */
		mm->mm_dbs[MAIN_DBI].md_flags = txn->mt_dbs[MAIN_DBI].md_flags;
	}
	if (root != P_INVALID || mm->mm_dbs[MAIN_DBI].md_flags) {
		mm->mm_txnid = 1;		/* use metapage 1 */
	}

	my.mc_wlen[0] = env->me_psize * NUM_METAS;
	my.mc_txn = txn;
	rc = mdb_env_cwalk(&my, &root, 0);
	if (rc == MDB_SUCCESS && root != new_root) {
		rc = MDB_INCOMPATIBLE;	/* page leak or corrupt DB */
	}

finish:
	if (rc)
		my.mc_error = rc;
	mdb_env_cthr_toggle(&my, 1 | MDB_EOF);
	rc = THREAD_FINISH(thr);
	_mdb_txn_abort(txn);

done:
#ifdef _WIN32
	if (my.mc_wbuf[0]) _aligned_free(my.mc_wbuf[0]);
	if (my.mc_cond)  CloseHandle(my.mc_cond);
	if (my.mc_mutex) CloseHandle(my.mc_mutex);
#else
	free(my.mc_wbuf[0]);
	pthread_cond_destroy(&my.mc_cond);
done2:
	pthread_mutex_destroy(&my.mc_mutex);
#endif
	return rc ? rc : my.mc_error;
}

	/** Copy environment as-is. */
static int ESECT
mdb_env_copyfd0(MDB_env *env, HANDLE fd)
{
	MDB_txn *txn = NULL;
	mdb_mutexref_t wmutex = NULL;
	int rc;
	mdb_size_t wsize, w3;
	char *ptr;
#ifdef _WIN32
	DWORD len, w2;
#define DO_WRITE(rc, fd, ptr, w2, len)	rc = WriteFile(fd, ptr, w2, &len, NULL)
#else
	ssize_t len;
	size_t w2;
#define DO_WRITE(rc, fd, ptr, w2, len)	len = write(fd, ptr, w2); rc = (len >= 0)
#endif

	/* Do the lock/unlock of the reader mutex before starting the
	 * write txn.  Otherwise other read txns could block writers.
	 */
	rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
	if (rc)
		return rc;

	if (env->me_txns) {
		/* We must start the actual read txn after blocking writers */
		mdb_txn_end(txn, MDB_END_RESET_TMP);

		/* Temporarily block writers until we snapshot the meta pages */
		wmutex = env->me_wmutex;
		if (LOCK_MUTEX(rc, env, wmutex))
			goto leave;

		rc = mdb_txn_renew0(txn);
		if (rc) {
			UNLOCK_MUTEX(wmutex);
			goto leave;
		}
	}

	wsize = env->me_psize * NUM_METAS;
	ptr = env->me_map;
	w2 = wsize;
	while (w2 > 0) {
		DO_WRITE(rc, fd, ptr, w2, len);
		if (!rc) {
			rc = ErrCode();
			break;
		} else if (len > 0) {
			rc = MDB_SUCCESS;
			ptr += len;
			w2 -= len;
			continue;
		} else {
			/* Non-blocking or async handles are not supported */
			rc = EIO;
			break;
		}
	}
	if (wmutex)
		UNLOCK_MUTEX(wmutex);

	if (rc)
		goto leave;

	w3 = txn->mt_next_pgno * env->me_psize;
	if (!(env->me_flags & MDB_INMEMORY)) {
		mdb_size_t fsize = 0;
		if ((rc = mdb_fsize(env->me_fd, &fsize)))
			goto leave;
		if (w3 > fsize)
			w3 = fsize;
	}
	wsize = w3 - wsize;
	while (wsize > 0) {
		if (wsize > MAX_WRITE)
			w2 = MAX_WRITE;
		else
			w2 = wsize;
		DO_WRITE(rc, fd, ptr, w2, len);
		if (!rc) {
			rc = ErrCode();
			break;
		} else if (len > 0) {
			rc = MDB_SUCCESS;
			ptr += len;
			wsize -= len;
			continue;
		} else {
			rc = EIO;
			break;
		}
	}

leave:
	_mdb_txn_abort(txn);
	return rc;
}

int ESECT
mdb_env_copyfd2(MDB_env *env, HANDLE fd, unsigned int flags)
{
	if (flags & MDB_CP_COMPACT)
		return mdb_env_copyfd1(env, fd);
	else
		return mdb_env_copyfd0(env, fd);
}

int ESECT
mdb_env_copyfd(MDB_env *env, HANDLE fd)
{
	return mdb_env_copyfd2(env, fd, 0);
}

int ESECT
mdb_env_copy2(MDB_env *env, const char *path, unsigned int flags)
{
	int rc;
	MDB_name fname;
	HANDLE newfd = INVALID_HANDLE_VALUE;

	rc = mdb_fname_init(path, env->me_flags | MDB_NOLOCK, &fname);
	if (rc == MDB_SUCCESS) {
		rc = mdb_fopen(env, &fname, MDB_O_COPY, 0666, &newfd);
		mdb_fname_destroy(fname);
	}
	if (rc == MDB_SUCCESS) {
		rc = mdb_env_copyfd2(env, newfd, flags);
		if (close(newfd) < 0 && rc == MDB_SUCCESS)
			rc = ErrCode();
	}
	return rc;
}

int ESECT
mdb_env_copy(MDB_env *env, const char *path)
{
	return mdb_env_copy2(env, path, 0);
}

int ESECT
mdb_env_set_flags(MDB_env *env, unsigned int flag, int onoff)
{
	if (flag & ~CHANGEABLE)
		return EINVAL;
	if (onoff)
		env->me_flags |= flag;
	else
		env->me_flags &= ~flag;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_flags(MDB_env *env, unsigned int *arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_flags & (CHANGEABLE|CHANGELESS);
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_interrupt(MDB_env *env, int onoff)
{
	if (!env)
		return EINVAL;
	env->me_interrupt = onoff ? 1 : 0;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_set_userctx(MDB_env *env, void *ctx)
{
	if (!env)
		return EINVAL;
	env->me_userctx = ctx;
	return MDB_SUCCESS;
}

void * ESECT
mdb_env_get_userctx(MDB_env *env)
{
	return env ? env->me_userctx : NULL;
}

int ESECT
mdb_env_set_assert(MDB_env *env, MDB_assert_func *func)
{
	if (!env)
		return EINVAL;
#ifndef NDEBUG
	env->me_assert_func = func;
#endif
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_path(MDB_env *env, const char **arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_path;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_fd(MDB_env *env, mdb_filehandle_t *arg)
{
	if (!env || !arg)
		return EINVAL;

	*arg = env->me_fd;
	return MDB_SUCCESS;
}

/** Common code for #mdb_stat() and #mdb_env_stat().
 * @param[in] env the environment to operate in.
 * @param[in] db the #MDB_db record containing the stats to return.
 * @param[out] arg the address of an #MDB_stat structure to receive the stats.
 * @return 0, this function always succeeds.
 */
static int ESECT
mdb_stat0(MDB_env *env, MDB_db *db, MDB_stat *arg)
{
	arg->ms_psize = env->me_psize;
	arg->ms_depth = db->md_depth;
	arg->ms_branch_pages = db->md_branch_pages;
	arg->ms_leaf_pages = db->md_leaf_pages;
	arg->ms_overflow_pages = db->md_overflow_pages;
	arg->ms_entries = db->md_entries;

	return MDB_SUCCESS;
}

int ESECT
mdb_env_stat(MDB_env *env, MDB_stat *arg)
{
	MDB_meta *meta;

	if (env == NULL || arg == NULL)
		return EINVAL;

	meta = mdb_env_pick_meta(env);

	return mdb_stat0(env, &meta->mm_dbs[MAIN_DBI], arg);
}

int ESECT
mdb_env_info(MDB_env *env, MDB_envinfo *arg)
{
	MDB_meta *meta;

	if (env == NULL || arg == NULL)
		return EINVAL;

	meta = mdb_env_pick_meta(env);
	arg->me_mapaddr = meta->mm_address;
	arg->me_last_pgno = meta->mm_last_pg;
	arg->me_last_txnid = meta->mm_txnid;

	arg->me_mapsize = env->me_mapsize;
	arg->me_maxreaders = env->me_maxreaders;
	arg->me_numreaders = env->me_txns ? env->me_txns->mti_numreaders : 0;
	return MDB_SUCCESS;
}

/** Set the default comparison functions for a database.
 * Called immediately after a database is opened to set the defaults.
 * The user can then override them with #mdb_set_compare() or
 * #mdb_set_dupsort().
 * @param[in] txn A transaction handle returned by #mdb_txn_begin()
 * @param[in] dbi A database handle returned by #mdb_dbi_open()
 */
static void
mdb_default_cmp(MDB_txn *txn, MDB_dbi dbi)
{
	uint16_t f = txn->mt_dbs[dbi].md_flags;

	txn->mt_dbxs[dbi].md_cmp =
		(f & MDB_REVERSEKEY) ? mdb_cmp_memnr :
		(f & MDB_INTEGERKEY) ? mdb_cmp_cint  : mdb_cmp_memn;

	txn->mt_dbxs[dbi].md_dcmp =
		!(f & MDB_DUPSORT) ? 0 :
		((f & MDB_INTEGERDUP)
		 ? ((f & MDB_DUPFIXED)   ? mdb_cmp_int   : mdb_cmp_cint)
		 : ((f & MDB_REVERSEDUP) ? mdb_cmp_memnr : mdb_cmp_memn));
}

int mdb_dbi_open(MDB_txn *txn, const char *name, unsigned int flags, MDB_dbi *dbi)
{
	MDB_val key, data;
	MDB_dbi i;
	MDB_cursor mc = (MDB_cursor){0};
	MDB_db dummy;
	int rc, dbflag, exact;
	unsigned int unused = 0, seq;
	char *namedup;
	size_t len;

	if (flags & ~VALID_FLAGS)
		return EINVAL;
	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;
	/* main DB? */
	if (!name) {
		*dbi = MAIN_DBI;
		if (flags & PERSISTENT_FLAGS) {
			uint16_t f2 = flags & PERSISTENT_FLAGS;
			/* make sure flag changes get committed */
			if ((txn->mt_dbs[MAIN_DBI].md_flags | f2) != txn->mt_dbs[MAIN_DBI].md_flags) {
				txn->mt_dbs[MAIN_DBI].md_flags |= f2;
				txn->mt_flags |= MDB_TXN_DIRTY;
			}
		}
		mdb_default_cmp(txn, MAIN_DBI);
		MDB_TRACE(("%p, (null), %u = %u", txn, flags, MAIN_DBI));
		return MDB_SUCCESS;
	}

	if (txn->mt_dbxs[MAIN_DBI].md_cmp == NULL) {
		mdb_default_cmp(txn, MAIN_DBI);
	}

	/* Is the DB already open? */
	len = strlen(name);
	for (i=CORE_DBS; i<txn->mt_numdbs; i++) {
		if (!txn->mt_dbxs[i].md_name.mv_size) {
			/* Remember this free slot */
			if (!unused) unused = i;
			continue;
		}
		if (len == txn->mt_dbxs[i].md_name.mv_size &&
			!strncmp(name, txn->mt_dbxs[i].md_name.mv_data, len)) {
			*dbi = i;
			return MDB_SUCCESS;
		}
	}

	/* If no free slot and max hit, fail */
	if (!unused && txn->mt_numdbs >= txn->mt_env->me_maxdbs)
		return MDB_DBS_FULL;

	/* Cannot mix named databases with some mainDB flags */
	if (txn->mt_dbs[MAIN_DBI].md_flags & (MDB_DUPSORT|MDB_INTEGERKEY))
		return (flags & MDB_CREATE) ? MDB_INCOMPATIBLE : MDB_NOTFOUND;

	/* Find the DB info */
	dbflag = DB_NEW|DB_VALID|DB_USRVALID;
	exact = 0;
	key.mv_size = len;
	key.mv_data = (void *)name;
	mdb_cursor_init(&mc, txn, MAIN_DBI, NULL);
	rc = mdb_cursor_set(&mc, &key, &data, MDB_SET, &exact);
	if (rc == MDB_SUCCESS) {
		/* make sure this is actually a DB */
		MDB_node *node = NODEPTR(mc.mc_pg[mc.mc_top], mc.mc_ki[mc.mc_top]);
		if ((node->mn_flags & (F_DUPDATA|F_SUBDATA)) != F_SUBDATA)
			return MDB_INCOMPATIBLE;
	} else {
		if (rc != MDB_NOTFOUND || !(flags & MDB_CREATE))
			return rc;
		if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
			return EACCES;
	}

	/* Done here so we cannot fail after creating a new DB */
	if ((namedup = strdup(name)) == NULL)
		return ENOMEM;

	if (rc) {
		/* MDB_NOTFOUND and MDB_CREATE: Create new DB */
		data.mv_size = sizeof(MDB_db);
		data.mv_data = &dummy;
		memset(&dummy, 0, sizeof(dummy));
		dummy.md_root = P_INVALID;
		dummy.md_flags = flags & PERSISTENT_FLAGS;
		WITH_CURSOR_TRACKING(mc,
			rc = _mdb_cursor_put(&mc, &key, &data, F_SUBDATA));
		dbflag |= DB_DIRTY;
	}

	if (rc) {
		free(namedup);
	} else {
		/* Got info, register DBI in this txn */
		unsigned int slot = unused ? unused : txn->mt_numdbs;
		txn->mt_dbxs[slot].md_name.mv_data = namedup;
		txn->mt_dbxs[slot].md_name.mv_size = len;
		txn->mt_dbxs[slot].md_rel = NULL;
		txn->mt_dbflags[slot] = dbflag;
		/* txn-> and env-> are the same in read txns, use
		 * tmp variable to avoid undefined assignment
		 */
		seq = ++txn->mt_env->me_dbiseqs[slot];
		txn->mt_dbiseqs[slot] = seq;

		memcpy(&txn->mt_dbs[slot], data.mv_data, sizeof(MDB_db));
		*dbi = slot;
		mdb_default_cmp(txn, slot);
		if (!unused) {
			txn->mt_numdbs++;
		}
		MDB_TRACE(("%p, %s, %u = %u", txn, name, flags, slot));
	}

	return rc;
}

int ESECT
mdb_count_all(MDB_txn *txn, MDB_dbi dbi, unsigned flags, uint64_t *out)
{
	if (!out || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;
	if (flags & ~MDB_COUNT_ALLOWED_FLAGS)
		return EINVAL;

	MDB_db *db = &txn->mt_dbs[dbi];
	unsigned char *dbflag = &txn->mt_dbflags[dbi];
	MDB_xcursor mx = (MDB_xcursor){0}, *mxp = NULL;

	if (!(db->md_flags & MDB_COUNTED))
		return MDB_INCOMPATIBLE;
	if (db->md_flags & MDB_DUPSORT) {
		memset(&mx, 0, sizeof(mx));
		mxp = &mx;
	}
	if (*dbflag & DB_STALE) {
		MDB_cursor mc = (MDB_cursor){0};
		mdb_cursor_init(&mc, txn, dbi, mxp);
		/* Refresh pointer after cursor init updates metadata. */
		db = &txn->mt_dbs[dbi];
		mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
		MDB_CURSOR_UNREF(&mc, 1);
	}

	*out = db->md_entries;
	return MDB_SUCCESS;
}

int ESECT
mdb_count_range(MDB_txn *txn, MDB_dbi dbi,
	const MDB_val *low, const MDB_val *high, unsigned flags, uint64_t *out)
{
	MDB_db *db;
	uint64_t lower = 0, upper = 0;
	int rc;
	int lower_incl = (flags & MDB_COUNT_LOWER_INCL) != 0;
	int upper_incl = (flags & MDB_COUNT_UPPER_INCL) != 0;

	if (!out || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;
	if (flags & ~MDB_COUNT_ALLOWED_FLAGS)
		return EINVAL;
	db = &txn->mt_dbs[dbi];
	if (!(db->md_flags & MDB_COUNTED))
		return MDB_INCOMPATIBLE;
	if (db->md_flags & MDB_DUPSORT)
		return MDB_INCOMPATIBLE;

	if (!db->md_entries) {
		*out = 0;
		return MDB_SUCCESS;
	}

	if (low && high) {
		MDB_cmp_func *cmp = txn->mt_dbxs[dbi].md_cmp;
		int c = cmp((MDB_val *)low, (MDB_val *)high);
		if (c > 0) {
			*out = 0;
			return MDB_SUCCESS;
		}
		if (c == 0 && !(lower_incl && upper_incl)) {
			*out = 0;
			return MDB_SUCCESS;
		}
	}

	if (high) {
		rc = mdb_prefix_count(txn, dbi, high, upper_incl, &upper);
		if (rc)
			return rc;
	} else {
		rc = mdb_count_all(txn, dbi, flags, &upper);
		if (rc)
			return rc;
	}

	if (low) {
		int include_self = !lower_incl; /* inclusive lower => exclude self */
		rc = mdb_prefix_count(txn, dbi, low, include_self, &lower);
		if (rc)
			return rc;
	}

	*out = (upper >= lower) ? (upper - lower) : 0;
	return MDB_SUCCESS;
}

static int
mdb_range_count_keys_dupsort(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *key_cmp,
	const MDB_val *key_low, const MDB_val *key_high,
	int lower_incl, int upper_incl, uint64_t *out)
{
	MDB_cursor mc = {0};
	MDB_xcursor mx = {0};
	MDB_val work_key = {0};
	MDB_val work_data = {0};
	uint64_t total = 0;
	int rc;

	if (!out || !key_cmp)
		return EINVAL;

	mdb_cursor_init(&mc, txn, dbi, &mx);

	if (key_low) {
		work_key = *key_low;
		rc = mdb_cursor_get(&mc, &work_key, &work_data, MDB_SET_RANGE);
	} else {
		rc = mdb_cursor_get(&mc, &work_key, &work_data, MDB_FIRST);
	}
	if (rc == MDB_NOTFOUND) {
		*out = 0;
		rc = MDB_SUCCESS;
		goto done;
	}
	if (rc != MDB_SUCCESS)
		goto done;

	if (key_low && !lower_incl) {
		int cmp = key_cmp(&work_key, (MDB_val *)key_low);
		while (cmp == 0) {
			rc = mdb_cursor_get(&mc, &work_key, &work_data, MDB_NEXT_NODUP);
			if (rc == MDB_NOTFOUND) {
				*out = 0;
				rc = MDB_SUCCESS;
				goto done;
			}
			if (rc != MDB_SUCCESS)
				goto done;
			cmp = key_cmp(&work_key, (MDB_val *)key_low);
		}
	}

	for (;;) {
		if (key_high) {
			int cmp = key_cmp(&work_key, (MDB_val *)key_high);
			if (cmp > 0 || (cmp == 0 && !upper_incl))
				break;
		}
		total++;
		rc = mdb_cursor_get(&mc, &work_key, &work_data, MDB_NEXT_NODUP);
		if (rc == MDB_NOTFOUND)
			break;
		if (rc != MDB_SUCCESS)
			goto done;
	}

	*out = total;
	rc = MDB_SUCCESS;

done:
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	MDB_CURSOR_UNREF(&mc, 1);
	return rc;
}

int ESECT
mdb_range_count_keys(MDB_txn *txn, MDB_dbi dbi,
	const MDB_val *key_low, const MDB_val *key_high,
	unsigned flags, uint64_t *out)
{
	MDB_db *db;
	MDB_cmp_func *key_cmp;
	int lower_incl = (flags & MDB_COUNT_LOWER_INCL) != 0;
	int upper_incl = (flags & MDB_COUNT_UPPER_INCL) != 0;

	if (!out || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;
	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;
	if (flags & ~MDB_COUNT_ALLOWED_FLAGS)
		return EINVAL;

	db = &txn->mt_dbs[dbi];
	if (!(db->md_flags & MDB_COUNTED))
		return MDB_INCOMPATIBLE;

	if (!db->md_entries) {
		*out = 0;
		return MDB_SUCCESS;
	}

	key_cmp = txn->mt_dbxs[dbi].md_cmp;
	if (!key_cmp)
		return MDB_PROBLEM;

	if (key_low && key_high) {
		int cmp = key_cmp((MDB_val *)key_low, (MDB_val *)key_high);
		if (cmp > 0 || (cmp == 0 && !(lower_incl && upper_incl))) {
			*out = 0;
			return MDB_SUCCESS;
		}
	}

	if (!(db->md_flags & MDB_DUPSORT))
		return mdb_count_range(txn, dbi, key_low, key_high, flags, out);

	return mdb_range_count_keys_dupsort(txn, dbi, key_cmp,
	    key_low, key_high, lower_incl, upper_incl, out);
}

static int
mdb_count_values_until(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *key_cmp,
	const MDB_val *limit_key, int inclusive, uint64_t *out)
{
	MDB_cursor mc = {0};
	MDB_xcursor mx = {0};
	MDB_val seek_key = {0};
	MDB_val seek_data = {0};
	int rc;
	uint64_t total = 0;

	if (!out)
		return EINVAL;

	mdb_cursor_init(&mc, txn, dbi, &mx);
	seek_key = *limit_key;
	rc = mdb_cursor_get(&mc, &seek_key, &seek_data, MDB_SET_RANGE);
	if (rc == MDB_NOTFOUND) {
		*out = txn->mt_dbs[dbi].md_entries;
		rc = MDB_SUCCESS;
		goto done;
	}
	if (rc != MDB_SUCCESS)
		goto done;

	int cmp = key_cmp(&seek_key, (MDB_val *)limit_key);

	rc = mdb_prefix_pair_leq(txn, dbi, &seek_key, 0, NULL, 0, &total);
	if (rc != MDB_SUCCESS)
		goto done;

	if (cmp > 0 || (cmp == 0 && !inclusive)) {
		*out = total;
		goto done;
	}

	if (cmp == 0 && inclusive) {
		uint64_t with_self = 0;
		rc = mdb_prefix_pair_leq(txn, dbi, &seek_key, 1, NULL, 1, &with_self);
		if (rc != MDB_SUCCESS)
			goto done;
		*out = with_self;
		goto done;
	}

	/* cmp < 0 should not occur, but fall back to total for safety. */
	*out = txn->mt_dbs[dbi].md_entries;

done:
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	MDB_CURSOR_UNREF(&mc, 1);
	return rc;
}

int ESECT
mdb_range_count_values(MDB_txn *txn, MDB_dbi dbi,
	const MDB_val *key_low, const MDB_val *key_high, unsigned key_flags,
	uint64_t *out)
{
	MDB_db *db;
	MDB_cmp_func *key_cmp;
	int rc;
	int key_lower_incl = (key_flags & MDB_COUNT_LOWER_INCL) != 0;
	int key_upper_incl = (key_flags & MDB_COUNT_UPPER_INCL) != 0;

	if (!out || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;
	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;
	if (key_flags & ~MDB_COUNT_ALLOWED_FLAGS)
		return EINVAL;

	db = &txn->mt_dbs[dbi];
	if (!(db->md_flags & MDB_DUPSORT))
		return MDB_INCOMPATIBLE;
	if (!(db->md_flags & MDB_COUNTED))
		return MDB_INCOMPATIBLE;

	if (!db->md_entries) {
		*out = 0;
		return MDB_SUCCESS;
	}

	key_cmp = txn->mt_dbxs[dbi].md_cmp;
	if (!key_cmp)
		return MDB_PROBLEM;

	if (key_low && key_high) {
		int c = key_cmp((MDB_val *)key_low, (MDB_val *)key_high);
		if (c > 0 || (c == 0 && !(key_lower_incl && key_upper_incl))) {
			*out = 0;
			return MDB_SUCCESS;
		}
	}

	uint64_t upper = 0, lower = 0;
	MDB_cursor mc = {0};
	MDB_xcursor mx = {0};
	int have_cursor = 0;

	if (key_high) {
		if (!have_cursor) {
			mdb_cursor_init(&mc, txn, dbi, &mx);
			have_cursor = 1;
		}
		rc = mdb_prefix_pair_leq_cursor(&mc, key_high,
		    key_upper_incl, NULL, key_upper_incl ? 1 : 0, &upper);
		if (rc != MDB_SUCCESS)
			goto cleanup;
		if (upper == db->md_entries) {
			rc = mdb_count_values_until(txn, dbi, key_cmp,
			    key_high, key_upper_incl ? 1 : 0, &upper);
			if (rc != MDB_SUCCESS)
				goto cleanup;
		}
	} else {
		upper = db->md_entries;
	}

	if (key_low) {
		if (!have_cursor) {
			mdb_cursor_init(&mc, txn, dbi, &mx);
			have_cursor = 1;
		}
		if (!key_lower_incl) {
			rc = mdb_prefix_pair_leq_cursor(&mc, key_low, 1, NULL, 1, &lower);
			if (rc != MDB_SUCCESS)
				goto cleanup;
		} else {
			rc = mdb_prefix_pair_leq_cursor(&mc, key_low, 0, NULL, 0, &lower);
			if (rc != MDB_SUCCESS)
				goto cleanup;
		}
		if (lower == db->md_entries) {
			int inclusive = key_lower_incl ? 0 : 1;
			rc = mdb_count_values_until(txn, dbi, key_cmp,
			    key_low, inclusive, &lower);
			if (rc != MDB_SUCCESS)
				goto cleanup;
		}
	}

	*out = (upper >= lower) ? (upper - lower) : 0;
	rc = MDB_SUCCESS;

cleanup:
	if (have_cursor) {
		mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
		MDB_CURSOR_UNREF(&mc, 1);
	}
	return rc;
}

static int
mdb_rank_for_key(MDB_txn *txn, MDB_dbi dbi,
	const MDB_val *key, const MDB_val *data,
	unsigned flags, uint64_t *rank_out)
{
	MDB_db *db;
	MDB_cursor mc = {0};
	MDB_xcursor mx = {0};
	MDB_val seek_key = {0};
	MDB_val stored_data = {0};
	int rc;
	int dupsort;

	if (!txn || !key || !rank_out)
		return EINVAL;
	if (flags)
		return EINVAL;
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;
	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	db = &txn->mt_dbs[dbi];
	if (!(db->md_flags & MDB_COUNTED))
		return MDB_INCOMPATIBLE;

	dupsort = (db->md_flags & MDB_DUPSORT) != 0;
	mdb_cursor_init(&mc, txn, dbi, dupsort ? &mx : NULL);

	seek_key = *key;
	rc = mdb_cursor_get(&mc, &seek_key, &stored_data, MDB_SET_KEY);
	if (rc != MDB_SUCCESS)
		goto done;

	if (data) {
		if (dupsort) {
			MDB_val seek_data = {data->mv_size, (void *)data->mv_data};
			rc = mdb_cursor_get(&mc, &seek_key, &seek_data, MDB_GET_BOTH);
			if (rc != MDB_SUCCESS)
				goto done;
		} else {
			if (stored_data.mv_size != data->mv_size ||
			    memcmp(stored_data.mv_data, data->mv_data, data->mv_size) != 0) {
				rc = MDB_NOTFOUND;
				goto done;
			}
		}
	}

	rc = MDB_SUCCESS;

done:
	MDB_CURSOR_UNREF(&mc, 1);
	if (rc != MDB_SUCCESS)
		return rc;

	if (dupsort && data)
		return mdb_prefix_pair_leq(txn, dbi, key, 1, data, 0, rank_out);
	return mdb_prefix_pair_leq(txn, dbi, key, 0, NULL, 0, rank_out);
}

static int
mdb_cursor_rank_descend(MDB_cursor *mc, uint64_t rank, uint64_t *entry_offset)
{
	MDB_page *mp;
	MDB_node *node = NULL;
	uint64_t remaining;
	int rc;

	if (!mc->mc_snum)
		return MDB_NOTFOUND;
	if (mc->mc_rank_base[mc->mc_top] > rank)
		return MDB_CORRUPTED;

	mp = mc->mc_pg[mc->mc_top];
	if (!mp)
		return MDB_CORRUPTED;
	remaining = rank - mc->mc_rank_base[mc->mc_top];

	while (IS_BRANCH(mp)) {
		indx_t i, nkeys = NUMKEYS(mp);
		if (!IS_COUNTED(mp))
			return MDB_INCOMPATIBLE;
		for (i = 0; i < nkeys; ++i) {
			node = NODEPTR(mp, i);
			uint64_t child_total = mdb_node_get_count(mp, node);
			if (remaining < child_total)
				break;
			remaining -= child_total;
		}
		if (i == nkeys)
			return MDB_NOTFOUND;
		mc->mc_ki[mc->mc_top] = i;
		rc = mdb_page_get(mc, NODEPGNO(node), &mp, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
		rc = mdb_cursor_push(mc, mp);
		if (rc != MDB_SUCCESS)
			return rc;
		mc->mc_rank_base[mc->mc_top] = rank - remaining;
	}

	if (IS_LEAF2(mp)) {
		indx_t nkeys = NUMKEYS(mp);
		if (remaining >= nkeys)
			return MDB_NOTFOUND;
		mc->mc_ki[mc->mc_top] = (indx_t)remaining;
		mc->mc_rank_cached = rank;
		mc->mc_rank_offset = 0;
		if (entry_offset)
			*entry_offset = 0;
		return MDB_SUCCESS;
	}

	if (!IS_LEAF(mp))
		return MDB_CORRUPTED;

	indx_t i, nkeys = NUMKEYS(mp);
	for (i = 0; i < nkeys; ++i) {
		node = NODEPTR(mp, i);
		uint64_t contrib = mdb_leaf_entry_contribution(mp, node);
		if (remaining < contrib)
			break;
		remaining -= contrib;
	}
	if (i == nkeys)
		return MDB_NOTFOUND;
	mc->mc_ki[mc->mc_top] = i;
	mc->mc_rank_cached = rank;
	mc->mc_rank_offset = remaining;
	if (entry_offset)
		*entry_offset = remaining;
	return MDB_SUCCESS;
}

static int
mdb_cursor_rank_search(MDB_cursor *mc, uint64_t rank, uint64_t *entry_offset)
{
	MDB_page *root;
	int rc;

	if (!mc || !mc->mc_db)
		return EINVAL;
	if (mc->mc_db->md_entries <= rank)
		return MDB_NOTFOUND;

	root = mc->mc_pg[0];
	/* Inline duplicate subpages already have their leaf page loaded. */
	if (!root || !IS_SUBP(root)) {
		rc = mdb_page_search(mc, NULL, MDB_PS_ROOTONLY);
		if (rc != MDB_SUCCESS)
			return rc;
	} else {
		mc->mc_top = 0;
		mc->mc_snum = 1;
	}

	mc->mc_rank_base[0] = 0;
	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~(C_EOF|C_DEL);

	return mdb_cursor_rank_descend(mc, rank, entry_offset);
}

static int
mdb_cursor_rank_resume(MDB_cursor *mc, uint64_t rank, uint64_t *entry_offset)
{
	int level;

	if (!mc || !(mc->mc_flags & C_INITIALIZED) || !mc->mc_snum)
		return MDB_NOTFOUND;
	if (mc->mc_db->md_entries <= rank)
		return MDB_NOTFOUND;
	if (rank == mc->mc_rank_cached) {
		if (entry_offset)
			*entry_offset = mc->mc_rank_offset;
		return MDB_SUCCESS;
	}

	for (level = mc->mc_top; level >= 0; --level) {
		MDB_page *page = mc->mc_pg[level];
		uint64_t base = mc->mc_rank_base[level];
		uint64_t window;
		if (!page)
			return MDB_CORRUPTED;
		if (rank < base)
			continue;
		window = rank - base;
		if (window < mdb_page_subtree_count(page))
			break;
	}
	if (level < 0)
		return MDB_NOTFOUND;

	while (mc->mc_top > level)
		mdb_cursor_pop(mc);

	mc->mc_flags |= C_INITIALIZED;
	mc->mc_flags &= ~(C_EOF|C_DEL);

	return mdb_cursor_rank_descend(mc, rank, entry_offset);
}

int ESECT
mdb_cursor_get_rank(MDB_cursor *mc, uint64_t rank,
	MDB_val *key, MDB_val *data, unsigned flags)
{
	MDB_page *mp;
	MDB_node *leaf;
	uint64_t dup_index = 0;
	int rc;

	if (!mc)
		return EINVAL;
	if (flags)
		return EINVAL;
	if (!(mc->mc_db->md_flags & MDB_COUNTED))
		return MDB_INCOMPATIBLE;
	if (mc->mc_txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;
	if (!mc->mc_db->md_entries)
		return MDB_NOTFOUND;
	if (mc->mc_db->md_entries <= rank)
		return MDB_NOTFOUND;

	if (mc->mc_xcursor)
		MDB_CURSOR_UNREF(&mc->mc_xcursor->mx_cursor, 0);

	if ((mc->mc_flags & (C_RANKVALID|C_INITIALIZED)) == (C_RANKVALID|C_INITIALIZED) &&
	    mc->mc_snum && rank >= mc->mc_rank_cached) {
		rc = mdb_cursor_rank_resume(mc, rank, &dup_index);
		if (rc == MDB_SUCCESS)
			goto positioned;
		if (rc != MDB_NOTFOUND)
			return rc;
	}

	rc = mdb_cursor_rank_search(mc, rank, &dup_index);
	if (rc != MDB_SUCCESS)
		return rc;

positioned:

	mp = mc->mc_pg[mc->mc_top];
	if (IS_LEAF2(mp)) {
		if (key) {
			key->mv_size = mc->mc_db->md_pad;
			key->mv_data = LEAF2KEY(mp, mc->mc_ki[mc->mc_top], key->mv_size);
		}
		if (data) {
			data->mv_size = 0;
			data->mv_data = NULL;
		}
		return MDB_SUCCESS;
	}

	leaf = NODEPTR(mp, mc->mc_ki[mc->mc_top]);
	if (F_ISSET(leaf->mn_flags, F_DUPDATA)) {
		if (!mc->mc_xcursor)
			return MDB_INCOMPATIBLE;
		mdb_xcursor_init1(mc, leaf);
		if (dup_index >= mc->mc_xcursor->mx_db.md_entries)
			return MDB_NOTFOUND;
		rc = mdb_cursor_rank_search(&mc->mc_xcursor->mx_cursor,
		    dup_index, NULL);
		if (rc != MDB_SUCCESS)
			return rc;
	} else if (dup_index) {
		return MDB_CORRUPTED;
	}

	if (!key && !data) {
		mc->mc_flags |= C_RANKVALID;
		return MDB_SUCCESS;
	}
	rc = mdb_cursor_get(mc, key, data, MDB_GET_CURRENT);
	if (rc == MDB_SUCCESS)
		mc->mc_flags |= C_RANKVALID;
	else
		mdb_cursor_rank_invalidate(mc);
	return rc;
}

int ESECT
mdb_cursor_key_rank(MDB_cursor *mc, const MDB_val *key,
	const MDB_val *data, unsigned flags, uint64_t *rank_out)
{
	if (!mc)
		return EINVAL;
	return mdb_rank_for_key(mc->mc_txn, mc->mc_dbi, key, data, flags, rank_out);
}

int ESECT
mdb_get_rank(MDB_txn *txn, MDB_dbi dbi, uint64_t rank,
	MDB_val *key, MDB_val *data)
{
	MDB_cursor mc = (MDB_cursor){0};
	MDB_xcursor mx = (MDB_xcursor){0};
	int rc;

	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;
	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

	if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT)
		mdb_cursor_init(&mc, txn, dbi, &mx);
	else
		mdb_cursor_init(&mc, txn, dbi, NULL);

	rc = mdb_cursor_get_rank(&mc, rank, key, data, 0);
	if (rc == MDB_SUCCESS && key &&
	    (txn->mt_dbs[dbi].md_flags & MDB_PREFIX_COMPRESSION)) {
		/* Prefix-decoded keys may point into cursor-local scratch. */
		unsigned char *stable_key = NULL;
		int krc = mdb_prefix_ensure_keybuf(txn, key->mv_size, &stable_key);
		if (krc == MDB_SUCCESS) {
			if (key->mv_size)
				memcpy(stable_key, key->mv_data, key->mv_size);
			key->mv_data = stable_key;
		} else {
			rc = krc;
		}
	}
	MDB_CURSOR_UNREF(&mc, 1);
	mdb_cursor_leaf_cache_clear(&mc.mc_leaf_cache);
	return rc;
}

int ESECT
mdb_get_key_rank(MDB_txn *txn, MDB_dbi dbi,
	const MDB_val *key, const MDB_val *data, uint64_t *rank_out)
{
	return mdb_rank_for_key(txn, dbi, key, data, 0, rank_out);
}

int ESECT
mdb_stat(MDB_txn *txn, MDB_dbi dbi, MDB_stat *arg)
{
	if (!arg || !TXN_DBI_EXIST(txn, dbi, DB_VALID))
		return EINVAL;

	if (txn->mt_flags & MDB_TXN_BLOCKED)
		return MDB_BAD_TXN;

		if (txn->mt_dbflags[dbi] & DB_STALE) {
			MDB_cursor mc = (MDB_cursor){0};
			MDB_xcursor mx = (MDB_xcursor){0};
		/* Stale, must read the DB's root. cursor_init does it for us. */
		mdb_cursor_init(&mc, txn, dbi, &mx);
	}
	return mdb_stat0(txn->mt_env, &txn->mt_dbs[dbi], arg);
}

void mdb_dbi_close(MDB_env *env, MDB_dbi dbi)
{
	char *ptr;
	if (dbi < CORE_DBS || dbi >= env->me_maxdbs)
		return;
	ptr = env->me_dbxs[dbi].md_name.mv_data;
	/* If there was no name, this was already closed */
	if (ptr) {
		MDB_TRACE(("%p, %u", env, dbi));
		env->me_dbxs[dbi].md_name.mv_data = NULL;
		env->me_dbxs[dbi].md_name.mv_size = 0;
		env->me_dbflags[dbi] = 0;
		env->me_dbiseqs[dbi]++;
		free(ptr);
	}
}

int mdb_dbi_flags(MDB_txn *txn, MDB_dbi dbi, unsigned int *flags)
{
	/* We could return the flags for the FREE_DBI too but what's the point? */
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;
	*flags = txn->mt_dbs[dbi].md_flags & PERSISTENT_FLAGS;
	return MDB_SUCCESS;
}

/** Add all the DB's pages to the free list.
 * @param[in] mc Cursor on the DB to free.
 * @param[in] subs non-Zero to check for sub-DBs in this DB.
 * @return 0 on success, non-zero on failure.
 */
static int
mdb_drop0(MDB_cursor *mc, int subs)
{
	int rc;

	rc = mdb_page_search(mc, NULL, MDB_PS_FIRST);
	if (rc == MDB_SUCCESS) {
		MDB_txn *txn = mc->mc_txn;
		MDB_node *ni;
			MDB_cursor mx = (MDB_cursor){0};
		unsigned int i;

		/* DUPSORT sub-DBs have no ovpages/DBs. Omit scanning leaves.
		 * This also avoids any P_LEAF2 pages, which have no nodes.
		 * Also if the DB doesn't have sub-DBs and has no overflow
		 * pages, omit scanning leaves.
		 */
		if ((mc->mc_flags & C_SUB) ||
			(!subs && !mc->mc_db->md_overflow_pages))
			mdb_cursor_pop(mc);

		mdb_cursor_copy(mc, &mx);
#ifdef MDB_VL32
		/* bump refcount for mx's pages */
		for (i=0; i<mc->mc_snum; i++)
			mdb_page_get(&mx, mc->mc_pg[i]->mp_pgno, &mx.mc_pg[i], NULL);
#endif
		while (mc->mc_snum > 0) {
			MDB_page *mp = mc->mc_pg[mc->mc_top];
			unsigned n = NUMKEYS(mp);
			if (IS_LEAF(mp)) {
				for (i=0; i<n; i++) {
					ni = NODEPTR(mp, i);
					if (ni->mn_flags & F_BIGDATA) {
						MDB_page *omp;
						pgno_t pg;
						memcpy(&pg, NODEDATA(ni), sizeof(pg));
						rc = mdb_page_get(mc, pg, &omp, NULL);
						if (rc != 0)
							goto done;
						mdb_cassert(mc, IS_OVERFLOW(omp));
						rc = mdb_midl_append_range(&txn->mt_free_pgs,
							pg, omp->mp_pages);
						if (rc)
							goto done;
						mc->mc_db->md_overflow_pages -= omp->mp_pages;
						if (!mc->mc_db->md_overflow_pages && !subs)
							break;
					} else if (subs && (ni->mn_flags & F_SUBDATA)) {
						mdb_xcursor_init1(mc, ni);
						rc = mdb_drop0(&mc->mc_xcursor->mx_cursor, 0);
						if (rc)
							goto done;
					}
				}
				if (!subs && !mc->mc_db->md_overflow_pages)
					goto pop;
			} else {
				if ((rc = mdb_midl_need(&txn->mt_free_pgs, n)) != 0)
					goto done;
				for (i=0; i<n; i++) {
					pgno_t pg;
					ni = NODEPTR(mp, i);
					pg = NODEPGNO(ni);
					/* free it */
					mdb_midl_xappend(txn->mt_free_pgs, pg);
				}
			}
			if (!mc->mc_top)
				break;
			mc->mc_ki[mc->mc_top] = i;
			rc = mdb_cursor_sibling(mc, 1);
			if (rc) {
				if (rc != MDB_NOTFOUND)
					goto done;
				/* no more siblings, go back to beginning
				 * of previous level.
				 */
pop:
				mdb_cursor_pop(mc);
				mc->mc_ki[0] = 0;
				for (i=1; i<mc->mc_snum; i++) {
					mc->mc_ki[i] = 0;
					mc->mc_pg[i] = mx.mc_pg[i];
				}
			}
		}
		/* free it */
		rc = mdb_midl_append(&txn->mt_free_pgs, mc->mc_db->md_root);
done:
		if (rc)
			mdb_txn_mark_error(txn, rc);
		/* drop refcount for mx's pages */
		MDB_CURSOR_UNREF(&mx, 0);
	} else if (rc == MDB_NOTFOUND) {
		rc = MDB_SUCCESS;
	}
	mc->mc_flags &= ~C_INITIALIZED;
	return rc;
}

int mdb_drop(MDB_txn *txn, MDB_dbi dbi, int del)
{
	MDB_cursor *mc, *m2;
	int rc;

	if ((unsigned)del > 1 || !TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	if (F_ISSET(txn->mt_flags, MDB_TXN_RDONLY))
		return EACCES;

	if (TXN_DBI_CHANGED(txn, dbi))
		return MDB_BAD_DBI;

	rc = mdb_cursor_open(txn, dbi, &mc);
	if (rc)
		return rc;

	MDB_TRACE(("%u, %d", dbi, del));
	rc = mdb_drop0(mc, mc->mc_db->md_flags & MDB_DUPSORT);
	/* Invalidate the dropped DB's cursors */
	for (m2 = txn->mt_cursors[dbi]; m2; m2 = m2->mc_next)
		m2->mc_flags &= ~(C_INITIALIZED|C_EOF);
	if (rc)
		goto leave;

	/* Can't delete the main DB */
	if (del && dbi >= CORE_DBS) {
		rc = mdb_del0(txn, MAIN_DBI, &mc->mc_dbx->md_name, NULL, F_SUBDATA);
		if (!rc) {
			txn->mt_dbflags[dbi] = DB_STALE;
			mdb_dbi_close(txn->mt_env, dbi);
		} else {
			mdb_txn_mark_error(txn, rc);
		}
	} else {
		/* reset the DB record, mark it dirty */
		txn->mt_dbflags[dbi] |= DB_DIRTY;
		txn->mt_dbs[dbi].md_depth = 0;
		txn->mt_dbs[dbi].md_branch_pages = 0;
		txn->mt_dbs[dbi].md_leaf_pages = 0;
		txn->mt_dbs[dbi].md_overflow_pages = 0;
		txn->mt_dbs[dbi].md_entries = 0;
		txn->mt_dbs[dbi].md_root = P_INVALID;

		txn->mt_flags |= MDB_TXN_DIRTY;
	}
leave:
	mdb_cursor_close(mc);
	return rc;
}

int mdb_set_compare(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *cmp)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_cmp = cmp;
	return MDB_SUCCESS;
}

int mdb_set_dupsort(MDB_txn *txn, MDB_dbi dbi, MDB_cmp_func *cmp)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_dcmp = cmp;
	return MDB_SUCCESS;
}

int mdb_set_relfunc(MDB_txn *txn, MDB_dbi dbi, MDB_rel_func *rel)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_rel = rel;
	return MDB_SUCCESS;
}

int mdb_set_relctx(MDB_txn *txn, MDB_dbi dbi, void *ctx)
{
	if (!TXN_DBI_EXIST(txn, dbi, DB_USRVALID))
		return EINVAL;

	txn->mt_dbxs[dbi].md_relctx = ctx;
	return MDB_SUCCESS;
}

int ESECT
mdb_env_get_maxkeysize(MDB_env *env)
{
	return ENV_MAXKEY(env);
}

int ESECT
mdb_reader_list(MDB_env *env, MDB_msg_func *func, void *ctx)
{
	unsigned int i, rdrs;
	MDB_reader *mr;
	char buf[64];
	int rc = 0, first = 1;

	if (!env || !func)
		return -1;
	if (!env->me_txns) {
		return func("(no reader locks)\n", ctx);
	}
	rdrs = env->me_txns->mti_numreaders;
	mr = env->me_txns->mti_readers;
	for (i=0; i<rdrs; i++) {
		if (mr[i].mr_pid) {
			txnid_t	txnid = mr[i].mr_txnid;
			sprintf(buf, txnid == (txnid_t)-1 ?
				"%10d %"Z"x -\n" : "%10d %"Z"x %"Yu"\n",
				(int)mr[i].mr_pid, (size_t)mr[i].mr_tid, txnid);
			if (first) {
				first = 0;
				rc = func("    pid     thread     txnid\n", ctx);
				if (rc < 0)
					break;
			}
			rc = func(buf, ctx);
			if (rc < 0)
				break;
		}
	}
	if (first) {
		rc = func("(no active readers)\n", ctx);
	}
	return rc;
}

/** Insert pid into list if not already present.
 * return -1 if already present.
 */
static int ESECT
mdb_pid_insert(MDB_PID_T *ids, MDB_PID_T pid)
{
	/* binary search of pid in list */
	unsigned base = 0;
	unsigned cursor = 1;
	int val = 0;
	unsigned n = ids[0];

	while( 0 < n ) {
		unsigned pivot = n >> 1;
		cursor = base + pivot + 1;
		val = pid - ids[cursor];

		if( val < 0 ) {
			n = pivot;

		} else if ( val > 0 ) {
			base = cursor;
			n -= pivot + 1;

		} else {
			/* found, so it's a duplicate */
			return -1;
		}
	}

	if( val > 0 ) {
		++cursor;
	}
	ids[0]++;
	for (n = ids[0]; n > cursor; n--)
		ids[n] = ids[n-1];
	ids[n] = pid;
	return 0;
}

int ESECT
mdb_reader_check(MDB_env *env, int *dead)
{
	if (!env)
		return EINVAL;
	if (dead)
		*dead = 0;
	return env->me_txns ? mdb_reader_check0(env, 0, dead) : MDB_SUCCESS;
}

/** As #mdb_reader_check(). \b rlocked is set if caller locked #me_rmutex. */
static int ESECT
mdb_reader_check0(MDB_env *env, int rlocked, int *dead)
{
	mdb_mutexref_t rmutex = rlocked ? NULL : env->me_rmutex;
	unsigned int i, j, rdrs;
	MDB_reader *mr;
	MDB_PID_T *pids, pid;
	int rc = MDB_SUCCESS, count = 0;

	rdrs = env->me_txns->mti_numreaders;
	pids = malloc((rdrs+1) * sizeof(MDB_PID_T));
	if (!pids)
		return ENOMEM;
	pids[0] = 0;
	mr = env->me_txns->mti_readers;
	for (i=0; i<rdrs; i++) {
		pid = mr[i].mr_pid;
		if (pid && pid != env->me_pid) {
			if (mdb_pid_insert(pids, pid) == 0) {
				if (!mdb_reader_pid(env, Pidcheck, pid)) {
					/* Stale reader found */
					j = i;
					if (rmutex) {
						if ((rc = LOCK_MUTEX0(rmutex)) != 0) {
							if ((rc = mdb_mutex_failed(env, rmutex, rc)))
								break;
							rdrs = 0; /* the above checked all readers */
						} else {
							/* Recheck, a new process may have reused pid */
							if (mdb_reader_pid(env, Pidcheck, pid))
								j = rdrs;
						}
					}
					for (; j<rdrs; j++)
							if (mr[j].mr_pid == pid) {
								DPRINTF(("clear stale reader pid %u txn %"Yd,
									(unsigned) pid, mr[j].mr_txnid));
								mr[j].mr_pid = 0;
								count++;
							}
					if (rmutex)
						UNLOCK_MUTEX(rmutex);
				}
			}
		}
	}
	free(pids);
	if (dead)
		*dead = count;
	return rc;
}

#ifdef MDB_ROBUST_SUPPORTED
/** Handle #LOCK_MUTEX0() failure.
 * Try to repair the lock file if the mutex owner died.
 * @param[in] env	the environment handle
 * @param[in] mutex	LOCK_MUTEX0() mutex
 * @param[in] rc	LOCK_MUTEX0() error (nonzero)
 * @return 0 on success with the mutex locked, or an error code on failure.
 */
static int ESECT
mdb_mutex_failed(MDB_env *env, mdb_mutexref_t mutex, int rc)
{
	int rlocked, rc2;
	MDB_meta *meta;

	if (rc == MDB_OWNERDEAD) {
		/* We own the mutex. Clean up after dead previous owner. */
		rc = MDB_SUCCESS;
		rlocked = (mutex == env->me_rmutex);
		if (!rlocked) {
			/* Keep mti_txnid updated, otherwise next writer can
			 * overwrite data which latest meta page refers to.
			 */
			meta = mdb_env_pick_meta(env);
			env->me_txns->mti_txnid = meta->mm_txnid;
			/* env is hosed if the dead thread was ours */
			if (env->me_txn) {
				env->me_flags |= MDB_FATAL_ERROR;
				env->me_txn = NULL;
				rc = MDB_PANIC;
			}
		}
		DPRINTF(("%cmutex owner died, %s", (rlocked ? 'r' : 'w'),
			(rc ? "this process' env is hosed" : "recovering")));
		rc2 = mdb_reader_check0(env, rlocked, NULL);
		if (rc2 == 0)
			rc2 = mdb_mutex_consistent(mutex);
		if (rc || (rc = rc2)) {
			DPRINTF(("LOCK_MUTEX recovery failed, %s", mdb_strerror(rc)));
			UNLOCK_MUTEX(mutex);
		}
	} else {
#ifdef _WIN32
		rc = ErrCode();
#endif
		DPRINTF(("LOCK_MUTEX failed, %s", mdb_strerror(rc)));
	}

	return rc;
}
#endif	/* MDB_ROBUST_SUPPORTED */

#if defined(_WIN32)
/** Convert \b src to new wchar_t[] string with room for \b xtra extra chars */
static int ESECT
utf8_to_utf16(const char *src, MDB_name *dst, int xtra)
{
	int rc, need = 0;
	wchar_t *result = NULL;
	for (;;) {					/* malloc result, then fill it in */
		need = MultiByteToWideChar(CP_UTF8, 0, src, -1, result, need);
		if (!need) {
			rc = ErrCode();
			free(result);
			return rc;
		}
		if (!result) {
			result = malloc(sizeof(wchar_t) * (need + xtra));
			if (!result)
				return ENOMEM;
			continue;
		}
		dst->mn_alloced = 1;
		dst->mn_len = need - 1;
		dst->mn_val = result;
		return MDB_SUCCESS;
	}
}
#endif /* defined(_WIN32) */
/** @} */
