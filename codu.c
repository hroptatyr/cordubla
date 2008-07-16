#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>


/* special purpose */
#include <pwd.h>
#include <string.h>

static const char*
user_name(int uid)
{
	/* just clip the fucker */
	static char un[9] = {0};
	struct passwd *pw = getpwuid(uid);
	strncpy(un, pw->pw_name, sizeof(un) - 1);
	return un;
}


/* configurable? */
#define CORE_DIR	"/tmp/core"

static int
mkdir_safe(const char *name, mode_t mode)
{
	struct stat st[1] = {{0}};
	lstat(name, st);
	if (S_ISDIR(st->st_mode)) {
		return 1;
	}
	return mkdir(name, mode);
}

static int
mkdir_core_dir(const char *uid)
{
	(void)mkdir("/tmp", 0755);
	if (chdir("/tmp") < 0) {
		return -1;
	}
	(void)mkdir("core", 0755);
	if (chdir("core") < 0) {
		return -1;
	}
	/* now create the user dir, maybe */
	switch (mkdir_safe(uid, 0700)) {
	case 0: {
		int uidn = strtoul(uid, NULL, 10);
		const char *unm = user_name(uidn);
		/* got created */
		chown(uid, uidn, 0);
		/* also generate a symlink */
		symlink(uid, unm);
		lchown(unm, uidn, 0);
	}
	case 1:
		/* was there before */
		break;
	case -1:
		/* whatever */
		return -1;
	}
	/* finally cd into user's core dir */
	if (chdir(uid) < 0) {
		return -1;
	}
	return 0;
}

static inline size_t
cnt(off_t off, size_t rlim)
{
/* the usual page size we want to cp over */
#define PGSZ	(4096)
#define CNT	(64 * PGSZ)
	if (off + CNT < rlim) {
		return CNT;
	}
	return rlim - off;
}

static inline int
incl_page_p(const char *buf, size_t pgsz)
{
	/* assume alignment */
	const long int *p = (const void*)buf;
	const long int *e = (const void*)(buf + pgsz);
	while (p < e && *p++ == 0);
	return p < e;
}

static inline long int
find_skip(const char *buf, size_t bsz)
{
/* we can take 64 pages max */
	long int res = 0;
	for (int i = 0; i < 64; i++) {
		int sk = incl_page_p(buf + PGSZ * i, bsz > PGSZ ? PGSZ : bsz);
		res |= (long int)sk << i;
	}
	return res;
}


static int
dump_core_sparsely(const char *file, size_t rlim)
{
#define CFILE_FLAGS	(O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_EXCL)
	int fdi, fdo;
	ssize_t sz;
	off_t off = 0;
	/* use the stack space */
	char buf[CNT] __attribute__((aligned(16)));

	if ((fdi = STDIN_FILENO) < 0) {
		return -1;
	}
	if ((fdo = open(file, CFILE_FLAGS, 0600)) < 0) {
		/* no need to close fdi */
		return -1;
	}
	/* copy fdi to fdo */
	while ((sz = read(fdi, buf, cnt(off, rlim))) > 0) {
		/* find longest zero sequence, page wise */
		long int skmsk = find_skip(buf, sz);
		long int cnt = 0;
		const char *p = buf;

		while (cnt < 64 && sz > 0) {
			if (sz < 2 * PGSZ) {
				off += write(fdo, p, sz);
				break;
			}
			/* otherwise */
			switch (skmsk & 3) {
			case 0:
				lseek(fdo, 2 * PGSZ, SEEK_CUR);
				break;
			case 1:
				(void)write(fdo, p, PGSZ);
				(void)lseek(fdo, PGSZ, SEEK_CUR);
				break;
			case 2:
				(void)lseek(fdo, PGSZ, SEEK_CUR);
				(void)write(fdo, p, PGSZ);
				break;
			case 3:
				(void)write(fdo, p, 2 * PGSZ);
				break;
			default:
				break;
			}
			p += 2 * PGSZ;
			off += 2 * PGSZ;
			skmsk >>= 2;
			cnt++;
		}
	}
	/* and we're out */
	close(fdo);
	close(fdi);
	return 0;
}

static int
dump_core(const char *file, size_t rlim)
{
#define CFILE_FLAGS	(O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_EXCL)
	int fdi, fdo;
	off_t off[1] = {0};

	if ((fdi = STDIN_FILENO) < 0) {
		return -1;
	}
	if ((fdo = open(file, CFILE_FLAGS, 0600)) < 0) {
		/* no need to close fdi */
		return -1;
	}
	/* copy fdi to fdo */
#define SPF	(SPLICE_F_MORE | SPLICE_F_MOVE)
	while (splice(fdi, NULL, fdo, off, cnt(off[0], rlim), SPF) > 0);
	/* and we're out */
	close(fdo);
	close(fdi);
	return 0;
}


/* expected to be called like %u %h %t %p %c %e, use
 * sysctl -w kernel.core_pattern="|/path/to/codu %u %h %t %p %c %e" */
int
main(int argc, char *argv[])
{
#define USER	(argv[1])
#define HOST	(argv[2])
#define TIME	(argv[3])
#define PID	(argv[4])
#define CLIM	(argv[5])
#define PROG	(argv[6])
	char cnm[PATH_MAX];
	size_t clim;
	int uid;

	/* check caller first */
	if (argc < 6) {
		fputs("Usage: codu UID HOST STAMP PID LIMIT [NAME]\n", stderr);
		return 1;
	}

	/* create the directories and cd there */
	if (mkdir_core_dir(USER) < 0) {
		return 1;
	}

	/* check if the core file limit allows us to write a core */
	if ((clim = strtol(CLIM, NULL, 10)) == 0) {
		/* nope */
		return 0;
	}

	/* lose some privileges */
	uid = strtoul(USER, NULL, 10);
	setuid(uid);

	/* core file name */
	snprintf(cnm, sizeof(cnm), "core-%s.%s.%s.%s.%s.dump",
		 PROG, USER, HOST, TIME, PID);
	/* and dump it */
	dump_core_sparsely(cnm, clim);
	return 0;
}

/* codu.c ends here */
