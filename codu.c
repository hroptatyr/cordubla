#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
/* special purpose, normal entry is getpwuid_r() but
 * we need save every second under heavy load so fuck all that
 * nsswitch lookup bobsmyuncle and just use nscd */
#include <pwd.h>
#include "me_nscd_proto.h"

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
dbg_open(int __attribute__((unused)) fl)
{
	return;
}

static inline void
dbg_close(void)
{
	return;
}
#endif	/* DEBUG */


static const char*
user_name(int uid)
{
	/* just clip the fucker */
	struct passwd pw[1], *pwr;
	static char aux[NSS_BUFLEN_PASSWD] = {0};
	if (__nscd_getpwuid_r(uid, pw, aux, sizeof(aux), &pwr) == 0) {
		return pw->pw_name;
	}
	return NULL;
}

static const char*
user_home(int uid)
{
	/* just clip the fucker */
	struct passwd pw[1], *pwr;
	static char aux[NSS_BUFLEN_PASSWD] = {0};
	if ((__nscd_getpwuid_r(uid, pw, aux, sizeof(aux), &pwr) == 0) &&
	    (pw->pw_dir != NULL)) {
		return pw->pw_dir;
	}
	return NULL;
}


/* configurable? */
#define CORE_DIR	"/tmp/core"

typedef struct codu_ctx_s {
	int uid, gid;
	/* full path to cwd */
	char cwd[PATH_MAX];
	/* the core file name */
	char cnm[PATH_MAX];
	/* core limit size */
	size_t clim;
} *codu_ctx_t;

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
mkdir_core_dir(codu_ctx_t ctx)
{
	char uid[32] = {0};

	(void)mkdir("/tmp", 0755);
	if (chdir("/tmp") < 0) {
		return -1;
	}
	(void)mkdir("core", 0755);
	if (chdir("core") < 0) {
		return -1;
	}
	/* printed repr of the uid */
	snprintf(uid, sizeof(uid), "%d", ctx->uid);
	/* now create the user dir, maybe */
	switch (mkdir_safe(uid, 0700)) {
	case 0: {
		const char *unm;
		/* got created */
		chown(uid, ctx->uid, ctx->gid);
		/* also generate a symlink */
		if ((unm = user_name(ctx->uid)) != NULL) {
			symlink(uid, unm);
			lchown(unm, ctx->uid, ctx->gid);
		}
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

static inline void
write_buffer(int fd, const char *buf, size_t bsz, size_t pgsz)
{
/* write stuff in BUF (of size BSZ) into FD, skip zero pages
 * of size PGSZ and return the number of written pages*/
	long unsigned int skmsk = find_skip(buf, bsz, pgsz);
	const char *ben;

	/* just a quick seek beforehand? */
	if (skmsk == 0) {
		lseek(fd, bsz, SEEK_CUR);
		DBG_OUT("w:0 s:64\n");
		return;
	} else if (skmsk == 0xffffffffffffffff) {
		write(fd, buf, bsz);
		DBG_OUT("w:64 s:0\n");
		return;
	}
	ben = buf + bsz;
	while (buf < ben) {
		int f1, f0;

		f1 = ffsl(skmsk);
		if (f1 == 0) {
			/* just seek to bsz */
			lseek(fd, ben - buf, SEEK_CUR);
			DBG_OUT("s:? %zd\n", ben - buf);
			break;
		} else if (f1 > 1) {
			lseek(fd, (f1 - 1) * pgsz, SEEK_CUR);
			skmsk >>= (f1 - 1);
			buf += (f1 - 1) * pgsz;
			DBG_OUT("s:%d %zu\n", f1 - 1, (f1 - 1) * pgsz);
		}
		f0 = ffsl(~skmsk);
		if (f0 == 0) {
			/* just write to bsz */
			write(fd, buf, ben - buf);
			DBG_OUT("w:? %zd\n", ben - buf);
			break;
		} else if (f0 > 1/*must be true*/) {
			write(fd, buf, (f0 - 1) * pgsz);
			skmsk >>= (f0 - 1);
			buf += (f0 - 1) * pgsz;
			DBG_OUT("w:%d %zu\n", f0 - 1, (f0 - 1) * pgsz);
		}
	}
	return;
}

static int
dump_core_sparsely(codu_ctx_t ctx)
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
	/* get stuff from the context */
	const char *file = ctx->cnm;
	size_t rlim = ctx->clim;
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
		write_buffer(fdo, buf, sz, PGSZ);
		DBG_OUT("%zd %zd\n", off, sz);
		off += sz;
	}
	ftruncate(fdo, off);
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
	for (int i = 6; i >= 0; --i) {
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


static void magic(codu_ctx_t ctx, char *argv[]);

/* expected to be called like %u %g %h %t %p %s %c %e, use
 * sysctl -w kernel.core_pattern="|/path/to/codu %u %g %h %t %p %s %c %e" */
int
main(int argc, char *argv[])
{
#define USER	(argv[1])
#define GRP	(argv[2])
#define HOST	(argv[3])
#define TIME	(argv[4])
#define PID	(argv[5])
#define SIG	(argv[6])
#define CLIM	(argv[7])
#define PROG	(argv[8])
	struct codu_ctx_s ctx[1] = {{0}};

	/* check caller first */
	if (argc < 6) {
		fputs("Usage: codu UID GID HOST "
		      "STAMP PID SIG LIMIT [NAME]\n", stderr);
		return 1;
	}

	/* initialise our context */
	ctx->uid = strtoul(USER, NULL, 10);
	ctx->gid = strtoul(GRP, NULL, 10);

	/* create the directories and cd there */
	if (mkdir_core_dir(ctx) < 0) {
		return 1;
	}

	/* check if the core file limit allows us to write a core */
	if ((ctx->clim = strtol(CLIM, NULL, 10)) == 0) {
		/* nope */
		return 0;
	}

	/* get the working directory of the crashing process */
	get_cwd(ctx->cwd, sizeof(ctx->cwd), PID);

	/* lose some privileges, become uid/gid */
	setuid(ctx->uid);
	setgid(ctx->gid);

	dbg_open(O_TRUNC);

	/* core file name */
	snprintf(ctx->cnm, sizeof(ctx->cnm), "core-%s.%s.%s.%s.%s.dump",
		 PROG, USER, HOST, TIME, PID);
	/* and dump it */
	dump_core_sparsely(ctx);

	if (daemonise() < 0) {
		return 0;
	}

	/* do user space shit */
	magic(ctx, argv);

	/* close debugging socket, maybe */
	dbg_close();
	return 0;
}

static void
magic(codu_ctx_t ctx, char *argv[])
{
	/* read ~/.codurc */
	const char *uho;
	char cnm_full[PATH_MAX];
	char cnm_orig[PATH_MAX];
	/* the user's script */
	char uscr[PATH_MAX];
	/* stat of the user's post script */
	struct stat st[1] = {{0}};

	/* links */
	snprintf(cnm_full, sizeof(cnm_full), "%s/%s", ctx->cwd, ctx->cnm);
	snprintf(cnm_orig, sizeof(cnm_orig), "%s.orig", ctx->cnm);
	symlink(cnm_full, cnm_orig);
	/* change into the crashing directory */
	chdir(ctx->cwd);
	/* create a symlink as well, reuse cnm_orig */
	snprintf(cnm_orig, sizeof(cnm_orig),
		 CORE_DIR "/%d/%s", ctx->uid, ctx->cnm);
	symlink(cnm_orig, ctx->cnm);

	/* find the user's codurc file */
	if ((uho = user_home(ctx->uid)) != NULL &&
	    (snprintf(uscr, sizeof(uscr), "/%s/.codu.post.sh", uho)) &&
	    (stat(uscr, st) == 0) &&
	    (st->st_mode & S_IEXEC)) {
		DBG_OUT("user post script file: %s\n", uscr);

		/* now execute the bugger */
		argv[0] = uscr;
		/* first argument is the actual location of the core file */
		argv[1] = cnm_orig;
		/* second argument is the link we've created */
		argv[2] = cnm_full;
		/* off we go */
		execve(uscr, argv, __environ);
	}
	return;
}

/* codu.c ends here */
