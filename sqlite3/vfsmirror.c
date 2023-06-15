/*
** 2011 March 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code implements a VFS shim that writes diagnostic
** output for each VFS call, similar to "strace".
**
** USAGE:
**
** This source file exports a single symbol which is the name of a
** function:
**
**   int vfsmirror_register(
**     const char *zTraceName,         // Name of the newly constructed VFS
**     const char *zOldVfsName,        // Name of the underlying VFS
**     int (*xOut)(const char*,void*), // Output routine.  ex: fputs
**     void *pOutArg,                  // 2nd argument to xOut.  ex: stderr
**     int makeDefault                 // Make the new VFS the default
**   );
**
** Applications that want to trace their VFS usage must provide a callback
** function with this prototype:
**
**   int traceOutput(const char *zMessage, void *pAppData);
**
** This function will "output" the trace messages, where "output" can
** mean different things to different applications.  The traceOutput function
** for the command-line shell (see shell.c) is "fputs" from the standard
** library, which means that all trace output is written on the stream
** specified by the second argument.  In the case of the command-line shell
** the second argument is stderr.  Other applications might choose to output
** trace information to a file, over a socket, or write it into a buffer.
**
** The vfsmirror_register() function creates a new "shim" VFS named by
** the zTraceName parameter.  A "shim" VFS is an SQLite backend that does
** not really perform the duties of a true backend, but simply filters or
** interprets VFS calls before passing them off to another VFS which does
** the actual work.  In this case the other VFS - the one that does the
** real work - is identified by the second parameter, zOldVfsName.  If
** the 2nd parameter is NULL then the default VFS is used.  The common
** case is for the 2nd parameter to be NULL.
**
** The third and fourth parameters are the pointer to the output function
** and the second argument to the output function.  For the SQLite
** command-line shell, when the -vfsmirror option is used, these parameters
** are fputs and stderr, respectively.
**
** The fifth argument is true (non-zero) to cause the newly created VFS
** to become the default VFS.  The common case is for the fifth parameter
** to be true.
**
** The call to vfsmirror_register() simply creates the shim VFS that does
** tracing.  The application must also arrange to use the new VFS for
** all database connections that are created and for which tracing is
** desired.  This can be done by specifying the trace VFS using URI filename
** notation, or by specifying the trace VFS as the 4th parameter to
** sqlite3_open_v2() or by making the trace VFS be the default (by setting
** the 5th parameter of vfsmirror_register() to 1).
**
**
** ENABLING VFSTRACE IN A COMMAND-LINE SHELL
**
** The SQLite command line shell implemented by the shell.c source file
** can be used with this module.  To compile in -vfsmirror support, first
** gather this file (test_vfsmirror.c), the shell source file (shell.c),
** and the SQLite amalgamation source files (sqlite3.c, sqlite3.h) into
** the working directory.  Then compile using a command like the following:
**
**    gcc -o sqlite3 -Os -I. -DSQLITE_ENABLE_VFSTRACE \
**        -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_RTREE \
**        -DHAVE_READLINE -DHAVE_USLEEP=1 \
**        shell.c test_vfsmirror.c sqlite3.c -ldl -lreadline -lncurses
**
** The gcc command above works on Linux and provides (in addition to the
** -vfsmirror option) support for FTS3 and FTS4, RTREE, and command-line
** editing using the readline library.  The command-line shell does not
** use threads so we added -DSQLITE_THREADSAFE=0 just to make the code
** run a little faster.   For compiling on a Mac, you'll probably need
** to omit the -DHAVE_READLINE, the -lreadline, and the -lncurses options.
** The compilation could be simplified to just this:
**
**    gcc -DSQLITE_ENABLE_VFSTRACE \
**         shell.c test_vfsmirror.c sqlite3.c -ldl -lpthread
**
** In this second example, all unnecessary options have been removed
** Note that since the code is now threadsafe, we had to add the -lpthread
** option to pull in the pthreads library.
**
** To cross-compile for windows using MinGW, a command like this might
** work:
**
**    /opt/mingw/bin/i386-mingw32msvc-gcc -o sqlite3.exe -Os -I \
**         -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_VFSTRACE \
**         shell.c test_vfsmirror.c sqlite3.c
**
** Similar compiler commands will work on different systems.  The key
** invariants are (1) you must have -DSQLITE_ENABLE_VFSTRACE so that
** the shell.c source file will know to include the -vfsmirror command-line
** option and (2) you must compile and link the three source files
** shell,c, test_vfsmirror.c, and sqlite3.c.
*/
#if defined(__GNUC__) && !defined(SQLITE_DISABLE_INTRINSIC)
# define GCC_VERSION (__GNUC__*1000000+__GNUC_MINOR__*1000+__GNUC_PATCHLEVEL__)
#else
# define GCC_VERSION 0
#endif
#if defined(_MSC_VER) && !defined(SQLITE_DISABLE_INTRINSIC)
# define MSVC_VERSION _MSC_VER
#else
# define MSVC_VERSION 0
#endif

#if defined(_MSC_VER)
#include <Windows.h>
#elif defined (__GNUC__)
#include <sys/stat.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

/*
** We need to define "NAME_MAX" if it was not present in "limits.h".
*/

#ifndef NAME_MAX
#  ifdef FILENAME_MAX
#    define NAME_MAX (FILENAME_MAX)
#  else
#    define NAME_MAX (260)
#  endif
#endif

/*
** An instance of this structure is attached to the each trace VFS to
** provide auxiliary information.
*/
typedef struct vfsmirror_info vfsmirror_info;
struct vfsmirror_info {
    sqlite3_vfs* pRootVfs;              /* The underlying real VFS */
    int (*xOut)(const char*, void*);    /* Send output here */
    void* pOutArg;                      /* First argument to xOut */
    const char* zVfsName;               /* Name of this trace-VFS */
    sqlite3_vfs* pTraceVfs;             /* Pointer back to the trace VFS */
};

/*
** The sqlite3_file object for the trace VFS
*/
typedef struct vfsmirror_file vfsmirror_file;
struct vfsmirror_file {
    sqlite3_file base;        /* Base class.  Must be first */
    vfsmirror_info* pInfo;     /* The trace-VFS to which this file belongs */
    const char* zFName;       /* Base name of the file */
    char zFname2[NAME_MAX];
    sqlite3_file* pReal[2];      /* The real underlying file */
};
static char zSlaveDir[NAME_MAX] = { 0, };
static int registered = 0;

/*
** Method declarations for vfsmirror_file.
*/
static int vfsmirrorClose(sqlite3_file*);
static int vfsmirrorRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int vfsmirrorWrite(sqlite3_file*, const void*, int iAmt, sqlite3_int64);
static int vfsmirrorTruncate(sqlite3_file*, sqlite3_int64 size);
static int vfsmirrorSync(sqlite3_file*, int flags);
static int vfsmirrorFileSize(sqlite3_file*, sqlite3_int64* pSize);
static int vfsmirrorLock(sqlite3_file*, int);
static int vfsmirrorUnlock(sqlite3_file*, int);
static int vfsmirrorCheckReservedLock(sqlite3_file*, int*);
static int vfsmirrorFileControl(sqlite3_file*, int op, void* pArg);
static int vfsmirrorSectorSize(sqlite3_file*);
static int vfsmirrorDeviceCharacteristics(sqlite3_file*);
static int vfsmirrorShmLock(sqlite3_file*, int, int, int);
static int vfsmirrorShmMap(sqlite3_file*, int, int, int, void volatile**);
static void vfsmirrorShmBarrier(sqlite3_file*);
static int vfsmirrorShmUnmap(sqlite3_file*, int);

/*
** Method declarations for vfsmirror_vfs.
*/
static int vfsmirrorOpen(sqlite3_vfs*, const char*, sqlite3_file*, int, int*);
static int vfsmirrorDelete(sqlite3_vfs*, const char* zName, int syncDir);
static int vfsmirrorAccess(sqlite3_vfs*, const char* zName, int flags, int*);
static int vfsmirrorFullPathname(sqlite3_vfs*, const char* zName, int, char*);
static void* vfsmirrorDlOpen(sqlite3_vfs*, const char* zFilename);
static void vfsmirrorDlError(sqlite3_vfs*, int nByte, char* zErrMsg);
static void (*vfsmirrorDlSym(sqlite3_vfs*, void*, const char* zSymbol))(void);
static void vfsmirrorDlClose(sqlite3_vfs*, void*);
static int vfsmirrorRandomness(sqlite3_vfs*, int nByte, char* zOut);
static int vfsmirrorSleep(sqlite3_vfs*, int microseconds);
static int vfsmirrorCurrentTime(sqlite3_vfs*, double*);
static int vfsmirrorGetLastError(sqlite3_vfs*, int, char*);
static int vfsmirrorCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);
static int vfsmirrorSetSystemCall(sqlite3_vfs*, const char*, sqlite3_syscall_ptr);
static sqlite3_syscall_ptr vfsmirrorGetSystemCall(sqlite3_vfs*, const char*);
static const char* vfsmirrorNextSystemCall(sqlite3_vfs*, const char* zName);
#define RETURN_CODE(RC1, RC2) (RC1 == RC2 ? RC1 : RC1 == SQLITE_OK ? RC2: RC1)
/*
** Return a pointer to the tail of the pathname.  Examples:
**
**     /home/drh/xyzzy.txt -> xyzzy.txt
**     xyzzy.txt           -> xyzzy.txt
*/
static const char* fileTail(const char* z) {
    int i;
    if (z == 0) return 0;
    i = strlen(z) - 1;
    while (i > 0 && z[i - 1] != '/' && z[i - 1] != '\\') { i--; }
    return &z[i];
}
#if 0
/*
** Send trace output defined by zFormat and subsequent arguments.
*/
static void vfsmirror_printf(
    vfsmirror_info* pInfo,
    const char* zFormat,
    ...
) {
    va_list ap;
    char* zMsg;
    va_start(ap, zFormat);
    zMsg = sqlite3_vmprintf(zFormat, ap);
    va_end(ap);
    pInfo->xOut(zMsg, pInfo->pOutArg);
    sqlite3_free(zMsg);
}
#else
#define vfsmirror_printf(...) (void)0
#endif
/*
** Convert value rc into a string and print it using zFormat.  zFormat
** should have exactly one %s
*/
static void vfsmirror_print_errcode(
    vfsmirror_info* pInfo,
    const char* zFormat,
    int rc
) {
    char zBuf[50];
    char* zVal;
    switch (rc) {
    case SQLITE_OK:         zVal = "SQLITE_OK";          break;
    case SQLITE_ERROR:      zVal = "SQLITE_ERROR";       break;
    case SQLITE_PERM:       zVal = "SQLITE_PERM";        break;
    case SQLITE_ABORT:      zVal = "SQLITE_ABORT";       break;
    case SQLITE_BUSY:       zVal = "SQLITE_BUSY";        break;
    case SQLITE_NOMEM:      zVal = "SQLITE_NOMEM";       break;
    case SQLITE_READONLY:   zVal = "SQLITE_READONLY";    break;
    case SQLITE_INTERRUPT:  zVal = "SQLITE_INTERRUPT";   break;
    case SQLITE_IOERR:      zVal = "SQLITE_IOERR";       break;
    case SQLITE_CORRUPT:    zVal = "SQLITE_CORRUPT";     break;
    case SQLITE_FULL:       zVal = "SQLITE_FULL";        break;
    case SQLITE_CANTOPEN:   zVal = "SQLITE_CANTOPEN";    break;
    case SQLITE_PROTOCOL:   zVal = "SQLITE_PROTOCOL";    break;
    case SQLITE_EMPTY:      zVal = "SQLITE_EMPTY";       break;
    case SQLITE_SCHEMA:     zVal = "SQLITE_SCHEMA";      break;
    case SQLITE_CONSTRAINT: zVal = "SQLITE_CONSTRAINT";  break;
    case SQLITE_MISMATCH:   zVal = "SQLITE_MISMATCH";    break;
    case SQLITE_MISUSE:     zVal = "SQLITE_MISUSE";      break;
    case SQLITE_NOLFS:      zVal = "SQLITE_NOLFS";       break;
    case SQLITE_IOERR_READ:         zVal = "SQLITE_IOERR_READ";         break;
    case SQLITE_IOERR_SHORT_READ:   zVal = "SQLITE_IOERR_SHORT_READ";   break;
    case SQLITE_IOERR_WRITE:        zVal = "SQLITE_IOERR_WRITE";        break;
    case SQLITE_IOERR_FSYNC:        zVal = "SQLITE_IOERR_FSYNC";        break;
    case SQLITE_IOERR_DIR_FSYNC:    zVal = "SQLITE_IOERR_DIR_FSYNC";    break;
    case SQLITE_IOERR_TRUNCATE:     zVal = "SQLITE_IOERR_TRUNCATE";     break;
    case SQLITE_IOERR_FSTAT:        zVal = "SQLITE_IOERR_FSTAT";        break;
    case SQLITE_IOERR_UNLOCK:       zVal = "SQLITE_IOERR_UNLOCK";       break;
    case SQLITE_IOERR_RDLOCK:       zVal = "SQLITE_IOERR_RDLOCK";       break;
    case SQLITE_IOERR_DELETE:       zVal = "SQLITE_IOERR_DELETE";       break;
    case SQLITE_IOERR_BLOCKED:      zVal = "SQLITE_IOERR_BLOCKED";      break;
    case SQLITE_IOERR_NOMEM:        zVal = "SQLITE_IOERR_NOMEM";        break;
    case SQLITE_IOERR_ACCESS:       zVal = "SQLITE_IOERR_ACCESS";       break;
    case SQLITE_IOERR_CHECKRESERVEDLOCK:
        zVal = "SQLITE_IOERR_CHECKRESERVEDLOCK"; break;
    case SQLITE_IOERR_LOCK:         zVal = "SQLITE_IOERR_LOCK";         break;
    case SQLITE_IOERR_CLOSE:        zVal = "SQLITE_IOERR_CLOSE";        break;
    case SQLITE_IOERR_DIR_CLOSE:    zVal = "SQLITE_IOERR_DIR_CLOSE";    break;
    case SQLITE_IOERR_SHMOPEN:      zVal = "SQLITE_IOERR_SHMOPEN";      break;
    case SQLITE_IOERR_SHMSIZE:      zVal = "SQLITE_IOERR_SHMSIZE";      break;
    case SQLITE_IOERR_SHMLOCK:      zVal = "SQLITE_IOERR_SHMLOCK";      break;
    case SQLITE_IOERR_SHMMAP:       zVal = "SQLITE_IOERR_SHMMAP";       break;
    case SQLITE_IOERR_SEEK:         zVal = "SQLITE_IOERR_SEEK";         break;
    case SQLITE_IOERR_GETTEMPPATH:  zVal = "SQLITE_IOERR_GETTEMPPATH";  break;
    case SQLITE_IOERR_CONVPATH:     zVal = "SQLITE_IOERR_CONVPATH";     break;
    case SQLITE_READONLY_DBMOVED:   zVal = "SQLITE_READONLY_DBMOVED";   break;
    case SQLITE_LOCKED_SHAREDCACHE: zVal = "SQLITE_LOCKED_SHAREDCACHE"; break;
    case SQLITE_BUSY_RECOVERY:      zVal = "SQLITE_BUSY_RECOVERY";      break;
    case SQLITE_CANTOPEN_NOTEMPDIR: zVal = "SQLITE_CANTOPEN_NOTEMPDIR"; break;
    default: {
        sqlite3_snprintf(sizeof(zBuf), zBuf, "%d", rc);
        zVal = zBuf;
        break;
    }
    }
    vfsmirror_printf(pInfo, zFormat, zVal);
}

/*
** Append to a buffer.
*/
static void strappend(char* z, int* pI, const char* zAppend) {
    int i = *pI;
    while (zAppend[0]) { z[i++] = *(zAppend++); }
    z[i] = 0;
    *pI = i;
}

/*
** Close an vfsmirror-file.
*/
static int vfsmirrorClose(sqlite3_file* pFile) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc, rc1 = SQLITE_OK;
    vfsmirror_printf(pInfo, "%s.xClose(%s)", pInfo->zVfsName, p->zFName);
    rc = p->pReal[0]->pMethods->xClose(p->pReal[0]);
    if (p->pReal[1]) {
        rc1 = p->pReal[1]->pMethods->xClose(p->pReal[1]);
        vfsmirror_print_errcode(pInfo, " -> %s\n", rc1);
    }
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    if (rc == SQLITE_OK) {
        sqlite3_free((void*)p->base.pMethods);
        p->base.pMethods = 0;
    }
    return RETURN_CODE(rc, rc1);
}

/*
** Read data from an vfsmirror-file.
*/
static int vfsmirrorRead(
    sqlite3_file* pFile,
    void* zBuf,
    int iAmt,
    sqlite_int64 iOfst
) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xRead(%s,n=%d,ofst=%lld)",
        pInfo->zVfsName, p->zFName, iAmt, iOfst);
    rc = p->pReal[0]->pMethods->xRead(p->pReal[0], zBuf, iAmt, iOfst);

    //if (p->pReal[1])
        //rc = p->pReal[1]->pMethods->xRead(p->pReal[1], zBuf, iAmt, iOfst);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return rc;
}

/*
** Write data to an vfsmirror-file.
*/
static int vfsmirrorWrite(
    sqlite3_file* pFile,
    const void* zBuf,
    int iAmt,
    sqlite_int64 iOfst
) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc, rc1 = SQLITE_OK;
    vfsmirror_printf(pInfo, "%s.xWrite(%s,n=%d,ofst=%lld)",
        pInfo->zVfsName, p->zFName, iAmt, iOfst);
    rc = p->pReal[0]->pMethods->xWrite(p->pReal[0], zBuf, iAmt, iOfst);
    if (p->pReal[1]) {
        rc1 = p->pReal[1]->pMethods->xWrite(p->pReal[1], zBuf, iAmt, iOfst);
        vfsmirror_print_errcode(pInfo, " -> %s\n", rc1);
    }
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return RETURN_CODE(rc, rc1);
}

/*
** Truncate an vfsmirror-file.
*/
static int vfsmirrorTruncate(sqlite3_file* pFile, sqlite_int64 size) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc, rc1 = SQLITE_OK;
    vfsmirror_printf(pInfo, "%s.xTruncate(%s,%lld)", pInfo->zVfsName, p->zFName,
        size);
    rc = p->pReal[0]->pMethods->xTruncate(p->pReal[0], size);
    if (p->pReal[1]) {
        rc1 = p->pReal[1]->pMethods->xTruncate(p->pReal[1], size);
        vfsmirror_printf(pInfo, " -> %d\n", rc1);
    }
    vfsmirror_printf(pInfo, " -> %d\n", rc);
    return RETURN_CODE(rc, rc1);
}

/*
** Sync an vfsmirror-file.
*/
static int vfsmirrorSync(sqlite3_file* pFile, int flags) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc, rc1 = SQLITE_OK;
#ifdef FULL_TRACE
    int i;
    char zBuf[100];
    memcpy(zBuf, "|0", 3);
    i = 0;
    if (flags & SQLITE_SYNC_FULL)        strappend(zBuf, &i, "|FULL");
    else if (flags & SQLITE_SYNC_NORMAL) strappend(zBuf, &i, "|NORMAL");
    if (flags & SQLITE_SYNC_DATAONLY)    strappend(zBuf, &i, "|DATAONLY");
    if (flags & ~(SQLITE_SYNC_FULL | SQLITE_SYNC_DATAONLY)) {
        sqlite3_snprintf(sizeof(zBuf) - i, &zBuf[i], "|0x%x", flags);
    }
    vfsmirror_printf(pInfo, "%s.xSync(%s,%s)", pInfo->zVfsName, p->zFName,
        &zBuf[1]);
#endif
    rc = p->pReal[0]->pMethods->xSync(p->pReal[0], flags);
    if (p->pReal[1]) {
        rc1 = p->pReal[1]->pMethods->xSync(p->pReal[1], flags);
    }
    vfsmirror_printf(pInfo, " -> %d\n", rc);
    return RETURN_CODE(rc, rc1);
}

/*
** Return the current file-size of an vfsmirror-file.
*/
static int vfsmirrorFileSize(sqlite3_file* pFile, sqlite_int64* pSize) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xFileSize(%s)", pInfo->zVfsName, p->zFName);
    rc = p->pReal[0]->pMethods->xFileSize(p->pReal[0], pSize);
    //    if(p->pReal[1])
    //          rc = p->pReal[1]->pMethods->xFileSize(p->pReal[1], pSize);
    vfsmirror_print_errcode(pInfo, " -> %s,", rc);
    vfsmirror_printf(pInfo, " size=%lld\n", *pSize);
    return rc;
}

/*
** Return the name of a lock.
*/
static const char* lockName(int eLock) {
    const char* azLockNames[] = {
       "NONE", "SHARED", "RESERVED", "PENDING", "EXCLUSIVE"
    };
    if (eLock < 0 || eLock >= sizeof(azLockNames) / sizeof(azLockNames[0])) {
        return "???";
    }
    else {
        return azLockNames[eLock];
    }
}

/*
** Lock an vfsmirror-file.
*/
static int vfsmirrorLock(sqlite3_file* pFile, int eLock) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xLock(%s,%s)", pInfo->zVfsName, p->zFName,
        lockName(eLock));
    rc = p->pReal[0]->pMethods->xLock(p->pReal[0], eLock);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return rc;
}

/*
** Unlock an vfsmirror-file.
*/
static int vfsmirrorUnlock(sqlite3_file* pFile, int eLock) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xUnlock(%s,%s)", pInfo->zVfsName, p->zFName,
        lockName(eLock));
    rc = p->pReal[0]->pMethods->xUnlock(p->pReal[0], eLock);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an vfsmirror-file.
*/
static int vfsmirrorCheckReservedLock(sqlite3_file* pFile, int* pResOut) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xCheckReservedLock(%s,%d)",
        pInfo->zVfsName, p->zFName);
    rc = p->pReal[0]->pMethods->xCheckReservedLock(p->pReal[0], pResOut);
    vfsmirror_print_errcode(pInfo, " -> %s", rc);
    vfsmirror_printf(pInfo, ", out=%d\n", *pResOut);
    return rc;
}

/*
** File control method. For custom operations on an vfsmirror-file.
*/
static int vfsmirrorFileControl(sqlite3_file* pFile, int op, void* pArg) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc, rc1 = SQLITE_OK;
#ifdef FULL_TRACE
    char zBuf[100];
    char* zOp;
    switch (op) {
    case SQLITE_FCNTL_LOCKSTATE:    zOp = "LOCKSTATE";          break;
    case SQLITE_GET_LOCKPROXYFILE:  zOp = "GET_LOCKPROXYFILE";  break;
    case SQLITE_SET_LOCKPROXYFILE:  zOp = "SET_LOCKPROXYFILE";  break;
    case SQLITE_LAST_ERRNO:         zOp = "LAST_ERRNO";         break;
    case SQLITE_FCNTL_SIZE_HINT: {
        sqlite3_snprintf(sizeof(zBuf), zBuf, "SIZE_HINT,%lld",
            *(sqlite3_int64*)pArg);
        zOp = zBuf;
        break;
    }
    case SQLITE_FCNTL_CHUNK_SIZE: {
        sqlite3_snprintf(sizeof(zBuf), zBuf, "CHUNK_SIZE,%d", *(int*)pArg);
        zOp = zBuf;
        break;
    }
    case SQLITE_FCNTL_FILE_POINTER: zOp = "FILE_POINTER";       break;
    case SQLITE_FCNTL_SYNC_OMITTED: zOp = "SYNC_OMITTED";       break;
    case SQLITE_FCNTL_WIN32_AV_RETRY: zOp = "WIN32_AV_RETRY";   break;
    case SQLITE_FCNTL_PERSIST_WAL:  zOp = "PERSIST_WAL";        break;
    case SQLITE_FCNTL_OVERWRITE:    zOp = "OVERWRITE";          break;
    case SQLITE_FCNTL_VFSNAME:      zOp = "VFSNAME";            break;
    case SQLITE_FCNTL_TEMPFILENAME: zOp = "TEMPFILENAME";       break;
    case 0xca093fa0:                zOp = "DB_UNCHANGED";       break;
    case SQLITE_FCNTL_PRAGMA: {
        const char* const* a = (const char* const*)pArg;
        sqlite3_snprintf(sizeof(zBuf), zBuf, "PRAGMA,[%s,%s]", a[1], a[2]);
        zOp = zBuf;
        break;
    }
    default: {
        sqlite3_snprintf(sizeof zBuf, zBuf, "%d", op);
        zOp = zBuf;
        break;
    }
    }
    vfsmirror_printf(pInfo, "%s.xFileControl(%s,%s)",
        pInfo->zVfsName, p->zFName, zOp);
#endif 
    rc = p->pReal[0]->pMethods->xFileControl(p->pReal[0], op, pArg);
    if (p->pReal[1]) {
        rc1 = p->pReal[1]->pMethods->xFileControl(p->pReal[1], op, pArg);
        vfsmirror_print_errcode(pInfo, " -> %s\n", rc1);
    }
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    if (op == SQLITE_FCNTL_VFSNAME && rc == SQLITE_OK) {
        *(char**)pArg = sqlite3_mprintf("vfsmirror.%s/%z",
            pInfo->zVfsName, *(char**)pArg);
    }
    if ((op == SQLITE_FCNTL_PRAGMA || op == SQLITE_FCNTL_TEMPFILENAME)
        && rc == SQLITE_OK && *(char**)pArg) {
        vfsmirror_printf(pInfo, "%s.xFileControl(%s,%s) returns %s",
            pInfo->zVfsName, p->zFName, zOp, *(char**)pArg);
    }
    return RETURN_CODE(rc, rc1);
}

/*
** Return the sector-size in bytes for an vfsmirror-file.
*/
static int vfsmirrorSectorSize(sqlite3_file* pFile) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xSectorSize(%s)", pInfo->zVfsName, p->zFName);
    rc = p->pReal[0]->pMethods->xSectorSize(p->pReal[0]);
    vfsmirror_printf(pInfo, " -> %d\n", rc);
    return rc;
}

/*
** Return the device characteristic flags supported by an vfsmirror-file.
*/
static int vfsmirrorDeviceCharacteristics(sqlite3_file* pFile) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xDeviceCharacteristics(%s)",
        pInfo->zVfsName, p->zFName);
    rc = p->pReal[0]->pMethods->xDeviceCharacteristics(p->pReal[0]);
    vfsmirror_printf(pInfo, " -> 0x%08x\n", rc);
    return rc;
}

/*
** Shared-memory operations.
*/
static int vfsmirrorShmLock(sqlite3_file* pFile, int ofst, int n, int flags) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
#ifdef FULL_TRACE
    char zLck[100];
    int i = 0;
    memcpy(zLck, "|0", 3);
    if (flags & SQLITE_SHM_UNLOCK)    strappend(zLck, &i, "|UNLOCK");
    if (flags & SQLITE_SHM_LOCK)      strappend(zLck, &i, "|LOCK");
    if (flags & SQLITE_SHM_SHARED)    strappend(zLck, &i, "|SHARED");
    if (flags & SQLITE_SHM_EXCLUSIVE) strappend(zLck, &i, "|EXCLUSIVE");
    if (flags & ~(0xf)) {
        sqlite3_snprintf(sizeof(zLck) - i, &zLck[i], "|0x%x", flags);
    }
    vfsmirror_printf(pInfo, "%s.xShmLock(%s,ofst=%d,n=%d,%s)",
        pInfo->zVfsName, p->zFName, ofst, n, &zLck[1]);
#endif // FULL_TRACE

    rc = p->pReal[0]->pMethods->xShmLock(p->pReal[0], ofst, n, flags);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return rc;
}
static int vfsmirrorShmMap(
    sqlite3_file* pFile,
    int iRegion,
    int szRegion,
    int isWrite,
    void volatile** pp
) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xShmMap(%s,iRegion=%d,szRegion=%d,isWrite=%d,*)",
        pInfo->zVfsName, p->zFName, iRegion, szRegion, isWrite);
    rc = p->pReal[0]->pMethods->xShmMap(p->pReal[0], iRegion, szRegion, isWrite, pp);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return rc;
}
static void vfsmirrorShmBarrier(sqlite3_file* pFile) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    vfsmirror_printf(pInfo, "%s.xShmBarrier(%s)\n", pInfo->zVfsName, p->zFName);
    p->pReal[0]->pMethods->xShmBarrier(p->pReal[0]);
}
static int vfsmirrorShmUnmap(sqlite3_file* pFile, int delFlag) {
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = p->pInfo;
    int rc;
    vfsmirror_printf(pInfo, "%s.xShmUnmap(%s,delFlag=%d)",
        pInfo->zVfsName, p->zFName, delFlag);
    rc = p->pReal[0]->pMethods->xShmUnmap(p->pReal[0], delFlag);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return rc;
}

static const char* vfsmirrorReplicaPath(char* dest, const char* sourcePath, size_t size)
{
    strncpy(dest, zSlaveDir, size);
    strncat(dest, "\\", size);
    strncat(dest, fileTail(sourcePath), size);
    return dest;
}
/*
** Open an vfsmirror file handle.
*/
static int vfsmirrorOpen(
    sqlite3_vfs* pVfs,
    const char* zName,
    sqlite3_file* pFile,
    int flags,
    int* pOutFlags
) {
    int rc, rc1 = SQLITE_OK;
    vfsmirror_file* p = (vfsmirror_file*)pFile;
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    int retry;

    p->pInfo = pInfo;
    p->zFName = zName ? fileTail(zName) : "<temp>";
    p->pReal[0] = (sqlite3_file*)&p[1];
    rc = pRoot->xOpen(pRoot, zName, p->pReal[0], flags, pOutFlags);
    vfsmirror_printf(pInfo, "%s.xOpen(%s,flags=0x%x)",
        pInfo->zVfsName, p->zFName, flags);
    if (p->pReal[0]->pMethods) {
        sqlite3_io_methods* pNew = (sqlite3_io_methods*)sqlite3_malloc(sizeof(*pNew));
        const sqlite3_io_methods* pSub = p->pReal[0]->pMethods;
        memset(pNew, 0, sizeof(*pNew));
        pNew->iVersion = 2;// pSub->iVersion;
        pNew->xClose = vfsmirrorClose;
        pNew->xRead = vfsmirrorRead;
        pNew->xWrite = vfsmirrorWrite;
        pNew->xTruncate = vfsmirrorTruncate;
        pNew->xSync = vfsmirrorSync;
        pNew->xFileSize = vfsmirrorFileSize;
        pNew->xLock = vfsmirrorLock;
        pNew->xUnlock = vfsmirrorUnlock;
        pNew->xCheckReservedLock = vfsmirrorCheckReservedLock;
        pNew->xFileControl = vfsmirrorFileControl;
        pNew->xSectorSize = vfsmirrorSectorSize;
        pNew->xDeviceCharacteristics = vfsmirrorDeviceCharacteristics;
        if (pNew->iVersion >= 2) {
            pNew->xShmMap = pSub->xShmMap ? vfsmirrorShmMap : 0;
            pNew->xShmLock = pSub->xShmLock ? vfsmirrorShmLock : 0;
            pNew->xShmBarrier = pSub->xShmBarrier ? vfsmirrorShmBarrier : 0;
            pNew->xShmUnmap = pSub->xShmUnmap ? vfsmirrorShmUnmap : 0;
        }
        pFile->pMethods = pNew;
    }

    if (zName && rc == SQLITE_OK && (flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL))) {
        vfsmirrorReplicaPath(p->zFname2, p->zFName, sizeof(p->zFname2));
        p->pReal[1] = (sqlite3_file*)(((char*)&(p[1])) + pRoot->szOsFile);
        retry = 10;
        while (retry > 0 && (rc1 = pRoot->xOpen(pRoot, p->zFname2, p->pReal[1], flags, pOutFlags)) != SQLITE_OK) {
            sqlite3_sleep(5);
            retry--;
        }
        vfsmirror_print_errcode(pInfo, " -> %s", rc1);
    }
    else {
        p->pReal[1] = 0;
    }



    vfsmirror_print_errcode(pInfo, " -> %s", rc);
    if (pOutFlags) {
        vfsmirror_printf(pInfo, ", outFlags=0x%x\n", *pOutFlags);
    }
    else {
        vfsmirror_printf(pInfo, "\n");
    }
    return RETURN_CODE(rc, rc1);
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int vfsmirrorDelete(sqlite3_vfs* pVfs, const char* zPath, int dirSync) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    int rc, rc1 = SQLITE_OK;
    vfsmirror_printf(pInfo, "%s.xDelete(\"%s\",%d)",
        pInfo->zVfsName, zPath, dirSync);
    rc = pRoot->xDelete(pRoot, zPath, dirSync);
    char tmpPath[NAME_MAX];
    vfsmirrorReplicaPath(tmpPath, zPath, sizeof(tmpPath));
    rc1 = pRoot->xDelete(pRoot, tmpPath, dirSync);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc1);
    vfsmirror_print_errcode(pInfo, " -> %s\n", rc);
    return RETURN_CODE(rc, rc1);
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int vfsmirrorAccess(
    sqlite3_vfs* pVfs,
    const char* zPath,
    int flags,
    int* pResOut
) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    int rc;
    vfsmirror_printf(pInfo, "%s.xAccess(\"%s\",%d)",
        pInfo->zVfsName, zPath, flags);
    rc = pRoot->xAccess(pRoot, zPath, flags, pResOut);
    vfsmirror_print_errcode(pInfo, " -> %s", rc);
    vfsmirror_printf(pInfo, ", out=%d\n", *pResOut);
    return rc;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int vfsmirrorFullPathname(
    sqlite3_vfs* pVfs,
    const char* zPath,
    int nOut,
    char* zOut
) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    int rc;
    vfsmirror_printf(pInfo, "%s.xFullPathname(\"%s\")",
        pInfo->zVfsName, zPath);
    rc = pRoot->xFullPathname(pRoot, zPath, nOut, zOut);
    vfsmirror_print_errcode(pInfo, " -> %s", rc);
    vfsmirror_printf(pInfo, ", out=\"%.*s\"\n", nOut, zOut);
    return rc;
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void* vfsmirrorDlOpen(sqlite3_vfs* pVfs, const char* zPath) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    vfsmirror_printf(pInfo, "%s.xDlOpen(\"%s\")\n", pInfo->zVfsName, zPath);
    return pRoot->xDlOpen(pRoot, zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated
** with dynamic libraries.
*/
static void vfsmirrorDlError(sqlite3_vfs* pVfs, int nByte, char* zErrMsg) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    vfsmirror_printf(pInfo, "%s.xDlError(%d)", pInfo->zVfsName, nByte);
    pRoot->xDlError(pRoot, nByte, zErrMsg);
    vfsmirror_printf(pInfo, " -> \"%s\"", zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*vfsmirrorDlSym(sqlite3_vfs* pVfs, void* p, const char* zSym))(void) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    vfsmirror_printf(pInfo, "%s.xDlSym(\"%s\")\n", pInfo->zVfsName, zSym);
    return pRoot->xDlSym(pRoot, p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void vfsmirrorDlClose(sqlite3_vfs* pVfs, void* pHandle) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    vfsmirror_printf(pInfo, "%s.xDlOpen()\n", pInfo->zVfsName);
    pRoot->xDlClose(pRoot, pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of
** random data.
*/
static int vfsmirrorRandomness(sqlite3_vfs* pVfs, int nByte, char* zBufOut) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    vfsmirror_printf(pInfo, "%s.xRandomness(%d)\n", pInfo->zVfsName, nByte);
    return pRoot->xRandomness(pRoot, nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds
** actually slept.
*/
static int vfsmirrorSleep(sqlite3_vfs* pVfs, int nMicro) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xSleep(pRoot, nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int vfsmirrorCurrentTime(sqlite3_vfs* pVfs, double* pTimeOut) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xCurrentTime(pRoot, pTimeOut);
}
static int vfsmirrorCurrentTimeInt64(sqlite3_vfs* pVfs, sqlite3_int64* pTimeOut) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xCurrentTimeInt64(pRoot, pTimeOut);
}

/*
** Return th3 emost recent error code and message
*/
static int vfsmirrorGetLastError(sqlite3_vfs* pVfs, int iErr, char* zErr) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xGetLastError(pRoot, iErr, zErr);
}

/*
** Override system calls.
*/
static int vfsmirrorSetSystemCall(
    sqlite3_vfs* pVfs,
    const char* zName,
    sqlite3_syscall_ptr pFunc
) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xSetSystemCall(pRoot, zName, pFunc);
}
static sqlite3_syscall_ptr vfsmirrorGetSystemCall(
    sqlite3_vfs* pVfs,
    const char* zName
) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xGetSystemCall(pRoot, zName);
}
static const char* vfsmirrorNextSystemCall(sqlite3_vfs* pVfs, const char* zName) {
    vfsmirror_info* pInfo = (vfsmirror_info*)pVfs->pAppData;
    sqlite3_vfs* pRoot = pInfo->pRootVfs;
    return pRoot->xNextSystemCall(pRoot, zName);
}


/*
** Clients invoke this routine to construct a new trace-vfs shim.
**
** Return SQLITE_OK on success.
**
** SQLITE_NOMEM is returned in the case of a memory allocation error.
** SQLITE_NOTFOUND is returned if zOldVfsName does not exist.
*/
SQLITE_API int vfsmirror_register(
    const char* zTraceName,           /* Name of the newly constructed VFS */
    const char* zOldVfsName,          /* Name of the underlying VFS */
    int (*xOut)(const char*, void*),   /* Output routine.  ex: fputs */
    void* pOutArg,                    /* 2nd argument to xOut.  ex: stderr */
    int makeDefault                   /* True to make the new VFS the default */
) {
    sqlite3_vfs* pNew;
    sqlite3_vfs* pRoot;
    vfsmirror_info* pInfo;
    int nName;
    int nByte;

    pRoot = sqlite3_vfs_find(zOldVfsName);
    if (pRoot == 0) return SQLITE_NOTFOUND;
    nName = strlen(zTraceName);
    nByte = sizeof(*pNew) + sizeof(*pInfo) + nName + 1;
    pNew = (sqlite3_vfs*)sqlite3_malloc(nByte);
    if (pNew == 0) return SQLITE_NOMEM;
    memset(pNew, 0, nByte);
    pInfo = (vfsmirror_info*)&pNew[1];
    pNew->iVersion = pRoot->iVersion;
    pNew->szOsFile = (2 * pRoot->szOsFile) + sizeof(vfsmirror_file);
    pNew->mxPathname = pRoot->mxPathname;
    pNew->zName = (char*)&pInfo[1];
    memcpy((char*)&pInfo[1], zTraceName, nName + 1);
    pNew->pAppData = pInfo;
    pNew->xOpen = vfsmirrorOpen;
    pNew->xDelete = vfsmirrorDelete;
    pNew->xAccess = vfsmirrorAccess;
    pNew->xFullPathname = vfsmirrorFullPathname;
    pNew->xDlOpen = pRoot->xDlOpen == 0 ? 0 : vfsmirrorDlOpen;
    pNew->xDlError = pRoot->xDlError == 0 ? 0 : vfsmirrorDlError;
    pNew->xDlSym = pRoot->xDlSym == 0 ? 0 : vfsmirrorDlSym;
    pNew->xDlClose = pRoot->xDlClose == 0 ? 0 : vfsmirrorDlClose;
    pNew->xRandomness = vfsmirrorRandomness;
    pNew->xSleep = vfsmirrorSleep;
    pNew->xCurrentTime = vfsmirrorCurrentTime;
    pNew->xGetLastError = pRoot->xGetLastError == 0 ? 0 : vfsmirrorGetLastError;
    if (pNew->iVersion >= 2) {
        pNew->xCurrentTimeInt64 = pRoot->xCurrentTimeInt64 == 0 ? 0 :
            vfsmirrorCurrentTimeInt64;
        if (pNew->iVersion >= 3) {
            pNew->xSetSystemCall = pRoot->xSetSystemCall == 0 ? 0 :
                vfsmirrorSetSystemCall;
            pNew->xGetSystemCall = pRoot->xGetSystemCall == 0 ? 0 :
                vfsmirrorGetSystemCall;
            pNew->xNextSystemCall = pRoot->xNextSystemCall == 0 ? 0 :
                vfsmirrorNextSystemCall;
        }
    }
    pInfo->pRootVfs = pRoot;
    pInfo->xOut = xOut;
    pInfo->pOutArg = pOutArg;
    pInfo->zVfsName = pNew->zName;
    pInfo->pTraceVfs = pNew;
    vfsmirror_printf(pInfo, "%s.enabled_for(\"%s\")\n",
        pInfo->zVfsName, pRoot->zName);
    return sqlite3_vfs_register(pNew, makeDefault);
}

static int dirExists(const char* dirName)
{
#if defined(_MSC_VER)
    DWORD ftyp = GetFileAttributesA(dirName);
    if (ftyp == INVALID_FILE_ATTRIBUTES)
        return 0;  //something is wrong with your path!

    if (ftyp & FILE_ATTRIBUTE_DIRECTORY)
        return 1;   // this is a directory!
#elif defined(__GNUC__)
    struct stat stats;
    if (stat(dirName, &stats) == 0 && S_ISDIR(stats.st_mode))
        return 1;   // this is a directory!
#endif
    return 0;    // this is not a directory!
}

SQLITE_API int set_mirror_directory(const char* slave_dir) {
    if (registered) {
        return 0;
    }
    size_t converted = strlen(slave_dir);
    if (converted >= NAME_MAX)
    {
        return 0;
    }
    snprintf(zSlaveDir, NAME_MAX, slave_dir);
    while (zSlaveDir[converted - 1] == '/' || zSlaveDir[converted - 1] == '\\' && converted > 0) {
        converted--;
        zSlaveDir[converted] = 0;
    }
    if (converted < 2 || !dirExists(zSlaveDir)) {
        return 0;
    }
    registered = 1;
    vfsmirror_register("trace", 0, (int (*)(const char*, void*))fputs, stderr, 1);
    return 1;
}
