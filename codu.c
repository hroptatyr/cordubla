#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/* fuck ugly */
#if defined DEBUG
static volatile int fdb;
static char dbg_buf[256];
static size_t dbg_len;
# define DBG_OUT(args...)						\
	do {								\
		dbg_len = snprintf(dbg_buf, sizeof(dbg_buf), args);	\
		write(fdb, dbg_buf, dbg_len);				\
	} while (0)

static inline void
dbg_open(int fl)
{
	fdb = open("/tmp/dbg", O_CREAT | O_WRONLY | fl, 0644);
	return;
}

static inline void
dbg_close(void)
{
	close(fdb);
	return;
}

#else  /* !DEBUG */
# define DBG_OUT(args...)
static inline void
dbg_open(int __attribute__((unused)))
{
	return;
}

static inline void
dbg_close(void)
{
	return;
}
#endif	/* DEBUG */


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

static const char*
user_home(int uid)
{
	/* just clip the fucker */
	static char un[PATH_MAX] = {0};
	struct passwd *pw = getpwuid(uid);
	if (pw->pw_dir) {
		strncpy(un, pw->pw_dir, sizeof(un));
	}
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
cnt(off_t off, size_t rlim, size_t max)
{
	if (off + max < rlim) {
		return max;
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

static inline long unsigned int
find_skip(const char *buf, size_t bsz, size_t pgsz)
{
/* we can take 64 pages max */
	long int res = 0;
	const char *bend = buf + bsz;
	for (int i = 0; buf < bend; i++) {
		size_t szl = bend - buf > pgsz ? pgsz : bend - buf;
		int sk = incl_page_p(buf, szl);
		res |= (long int)sk << i;
		buf += szl;
	}
	DBG_OUT("skip: %lx\n", (long unsigned int)res);
	return res;
}

static inline ssize_t
fill_buffer(int fd, char *restrict buf, size_t max)
{
	ssize_t tot = 0;
	ssize_t sz;
	while ((tot < max) &&
	       (sz = read(fd, buf + tot, max - tot)) > 0) {
		tot += sz;
	}
	return tot;
}

static int
dump_core_sparsely(const char *file, size_t rlim)
{
/* the usual page size we want to cp over */
#define PGSZ	(4096)
/* number of pages to process at a time */
#define CNT	(64 * PGSZ)
/* flags for the core file */
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
	/* coroutine:
	 * reading from fdi to buf first
	 * yielding 64k pages of kernel core dump goodness
	 * coroutine:
	 * determining the skip mask of */

	/* copy fdi to fdo */
	while ((sz = fill_buffer(fdi, buf, cnt(off, rlim, CNT))) > 0) {
		/* find longest zero sequence, page wise */
		long unsigned int skmsk = find_skip(buf, sz, PGSZ);
		long int cnt = 0;
		const char *p = buf;
		DBG_OUT("%zd %zd\n", off, sz);
		while (cnt < 64 && sz > 0) {
			if (sz < 2 * PGSZ) {
				off += write(fdo, p, sz);
				break;
			}
			/* otherwise */
			switch (skmsk & 3) {
			case 0/*00*/:
				lseek(fdo, 2 * PGSZ, SEEK_CUR);
				break;
			case 1/*01*/:
				(void)write(fdo, p, PGSZ);
				(void)lseek(fdo, PGSZ, SEEK_CUR);
				break;
			case 2/*10*/:
				(void)lseek(fdo, PGSZ, SEEK_CUR);
				(void)write(fdo, p, PGSZ);
				break;
			case 3/*11*/:
				(void)write(fdo, p, 2 * PGSZ);
				break;
			default:
				break;
			}
			p += 2 * PGSZ;
			off += 2 * PGSZ;
			sz -= 2 * PGSZ;
			skmsk >>= 2;
			cnt += 2;
		}
	}
	/* and we're out */
	close(fdo);
	close(fdi);
	return 0;
}

#if 0
/* doesnt handle sparsity */
static int
__attribute__((unused))
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
	while (splice(fdi, NULL, fdo, off, cnt(off[0], rlim, CNT), SPF) > 0);
	/* and we're out */
	close(fdo);
	close(fdi);
	return 0;
}
#endif

static ssize_t
get_cwd(char *buf, size_t bsz, const char *pid)
{
	char cwd_link[PATH_MAX];
	ssize_t cwd_len;

	snprintf(cwd_link, sizeof(cwd_link), "/proc/%s/cwd", pid);
	cwd_len = readlink(cwd_link, buf, bsz);
	if (cwd_len > 0) {
		buf[cwd_len] = '\0';
	}
	return cwd_len;
}

static int
daemonise(void)
{
	int fd;
	pid_t __attribute__((unused)) pid;

	switch (pid = fork()) {
	case -1:
		DBG_OUT("fork fucked\n");
		return -1;
	case 0:
		break;
	default:
		DBG_OUT("Successfully bore a squaller: %d\n", pid);
		exit(0);
	}

	if (setsid() == -1) {
		return -1;
	}
	/* close the debugging sock and all other descriptors */
	for (int i = getdtablesize(); i >= 0; --i) {
		close(i);
	}
	if ((fd = open("/dev/null", O_RDWR, 0)) >= 0) {
		(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fd, STDOUT_FILENO);
		(void)dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO) {
			(void)close(fd);
		}
	}
	/* reopen the debugging socket */
	dbg_open(O_APPEND);
	return 0;
}


static void magic(int uid);

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
	char cwd[PATH_MAX];
	char cnm[PATH_MAX];
	char cnm_full[PATH_MAX];
	char cnm_orig[PATH_MAX];
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

	/* get the working directory of the crashing process */
	get_cwd(cwd, sizeof(cwd), PID);

	/* lose some privileges */
	uid = strtoul(USER, NULL, 10);
	setuid(uid);

	dbg_open(O_TRUNC);

	/* core file name */
	snprintf(cnm, sizeof(cnm), "core-%s.%s.%s.%s.%s.dump",
		 PROG, USER, HOST, TIME, PID);
	/* and dump it */
	dump_core_sparsely(cnm, clim);

	/* links */
	snprintf(cnm_full, sizeof(cnm_full), "%s/%s", cwd, cnm);
	snprintf(cnm_orig, sizeof(cnm_orig), "%s.orig", cnm);
	symlink(cnm_full, cnm_orig);
	/* change into the crashing directory */
	chdir(cwd);
	/* create a symlink as well */
	snprintf(cnm_full, sizeof(cnm_full), CORE_DIR "/%s/%s", USER, cnm);
	symlink(cnm_full, cnm);

	if (daemonise() < 0) {
		return 0;
	}

	/* do user space shit */
	magic(uid);
	/* close debugging socket, maybe */
	dbg_close();
	return 0;
}

static void
magic(int uid)
{
	/* read ~/.codurc */
	const char *uho = user_home(uid);
	char uhodir[PATH_MAX];
	snprintf(uhodir, sizeof(uhodir), "/%s/.codurc", uho);
	DBG_OUT("user pref file: %s\n", uhodir);
	return;
}

/* codu.c ends here */
