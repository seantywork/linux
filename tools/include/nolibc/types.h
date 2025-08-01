/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * Special types used by various syscalls for NOLIBC
 * Copyright (C) 2017-2021 Willy Tarreau <w@1wt.eu>
 */

/* make sure to include all global symbols */
#include "nolibc.h"

#ifndef _NOLIBC_TYPES_H
#define _NOLIBC_TYPES_H

#include "std.h"
#include <linux/mman.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/wait.h>


/* Only the generic macros and types may be defined here. The arch-specific
 * ones such as the O_RDONLY and related macros used by fcntl() and open()
 * must not be defined here.
 */

/* stat flags (WARNING, octal here). We need to check for an existing
 * definition because linux/stat.h may omit to define those if it finds
 * that any glibc header was already included.
 */
#if !defined(S_IFMT)
#define S_IFDIR        0040000
#define S_IFCHR        0020000
#define S_IFBLK        0060000
#define S_IFREG        0100000
#define S_IFIFO        0010000
#define S_IFLNK        0120000
#define S_IFSOCK       0140000
#define S_IFMT         0170000

#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode)  (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode)  (((mode) & S_IFMT) == S_IFBLK)
#define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISLNK(mode)  (((mode) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#endif

/* dirent types */
#define DT_UNKNOWN     0x0
#define DT_FIFO        0x1
#define DT_CHR         0x2
#define DT_DIR         0x4
#define DT_BLK         0x6
#define DT_REG         0x8
#define DT_LNK         0xa
#define DT_SOCK        0xc

/* commonly an fd_set represents 256 FDs */
#ifndef FD_SETSIZE
#define FD_SETSIZE     256
#endif

/* PATH_MAX and MAXPATHLEN are often used and found with plenty of different
 * values.
 */
#ifndef PATH_MAX
#define PATH_MAX       4096
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN     (PATH_MAX)
#endif

/* flags for mmap */
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

/* whence values for lseek() */
#define SEEK_SET       0
#define SEEK_CUR       1
#define SEEK_END       2

/* flags for reboot */
#define RB_AUTOBOOT     LINUX_REBOOT_CMD_RESTART
#define RB_HALT_SYSTEM  LINUX_REBOOT_CMD_HALT
#define RB_ENABLE_CAD   LINUX_REBOOT_CMD_CAD_ON
#define RB_DISABLE_CAD  LINUX_REBOOT_CMD_CAD_OFF
#define RB_POWER_OFF    LINUX_REBOOT_CMD_POWER_OFF
#define RB_SW_SUSPEND   LINUX_REBOOT_CMD_SW_SUSPEND
#define RB_KEXEC        LINUX_REBOOT_CMD_KEXEC

/* Macros used on waitpid()'s return status */
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WIFSIGNALED(status) ((status) - 1 < 0xff)

/* standard exit() codes */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define FD_SETIDXMASK (8 * sizeof(unsigned long))
#define FD_SETBITMASK (8 * sizeof(unsigned long)-1)

/* for select() */
typedef struct {
	unsigned long fds[(FD_SETSIZE + FD_SETBITMASK) / FD_SETIDXMASK];
} fd_set;

#define FD_CLR(fd, set) do {						\
		fd_set *__set = (set);					\
		int __fd = (fd);					\
		if (__fd >= 0)						\
			__set->fds[__fd / FD_SETIDXMASK] &=		\
				~(1U << (__fd & FD_SETBITMASK));	\
	} while (0)

#define FD_SET(fd, set) do {						\
		fd_set *__set = (set);					\
		int __fd = (fd);					\
		if (__fd >= 0)						\
			__set->fds[__fd / FD_SETIDXMASK] |=		\
				1 << (__fd & FD_SETBITMASK);		\
	} while (0)

#define FD_ISSET(fd, set) ({						\
			fd_set *__set = (set);				\
			int __fd = (fd);				\
		int __r = 0;						\
		if (__fd >= 0)						\
			__r = !!(__set->fds[__fd / FD_SETIDXMASK] &	\
1U << (__fd & FD_SETBITMASK));						\
		__r;							\
	})

#define FD_ZERO(set) do {						\
		fd_set *__set = (set);					\
		int __idx;						\
		int __size = (FD_SETSIZE+FD_SETBITMASK) / FD_SETIDXMASK;\
		for (__idx = 0; __idx < __size; __idx++)		\
			__set->fds[__idx] = 0;				\
	} while (0)

/* for getdents64() */
struct linux_dirent64 {
	uint64_t       d_ino;
	int64_t        d_off;
	unsigned short d_reclen;
	unsigned char  d_type;
	char           d_name[];
};

/* The format of the struct as returned by the libc to the application, which
 * significantly differs from the format returned by the stat() syscall flavours.
 */
struct stat {
	dev_t     st_dev;     /* ID of device containing file */
	ino_t     st_ino;     /* inode number */
	mode_t    st_mode;    /* protection */
	nlink_t   st_nlink;   /* number of hard links */
	uid_t     st_uid;     /* user ID of owner */
	gid_t     st_gid;     /* group ID of owner */
	dev_t     st_rdev;    /* device ID (if special file) */
	off_t     st_size;    /* total size, in bytes */
	blksize_t st_blksize; /* blocksize for file system I/O */
	blkcnt_t  st_blocks;  /* number of 512B blocks allocated */
	union { time_t st_atime; struct timespec st_atim; }; /* time of last access */
	union { time_t st_mtime; struct timespec st_mtim; }; /* time of last modification */
	union { time_t st_ctime; struct timespec st_ctim; }; /* time of last status change */
};

typedef __kernel_clockid_t clockid_t;
typedef int timer_t;

#ifndef container_of
#define container_of(PTR, TYPE, FIELD) ({			\
	__typeof__(((TYPE *)0)->FIELD) *__FIELD_PTR = (PTR);	\
	(TYPE *)((char *) __FIELD_PTR - offsetof(TYPE, FIELD));	\
})
#endif

#endif /* _NOLIBC_TYPES_H */
