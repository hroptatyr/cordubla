#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>


/* configurable? */
#define CORE_DIR	"/tmp/core"

static int
dump_core(const char *file)
{
	int fdi, fdo, fdb;
	ssize_t sz;
	off_t off[1] = {0};

	if ((fdi = STDIN_FILENO) < 0) {
		return -1;
	}
	if ((fdo = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
		/* no need to close fdi */
		return -1;
	}
	if ((fdb = open("dbg", O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
		close(fdo);
		return -1;
	}
	/* copy fdi to fdo */
	while ((sz = sendfile(fdo, fdi, off, /*count*/4096)) > 0) {
		char szs[32];
		size_t szl;
		szl = snprintf(szs, sizeof(szs), "%zd\n", sz);
		write(fdb, szs, szl);
	}
	{
		char szs[32];
		size_t szl;
		szl = snprintf(szs, sizeof(szs), "that's it %zd %s\n", sz, strerror(errno));
		write(fdb, szs, szl);
	}
	/* and we're out */
	close(fdb);
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
	char cwd[PATH_MAX];
	char cnm[PATH_MAX];
	long int clim;
	int uid;

	/* check caller first */
	if (argc < 6) {
		fputs("Usage: codu UID HOST STAMP PID LIMIT [NAME]\n", stderr);
		return 1;
	}

	/* create the directories and cd there */
	if (chdir(CORE_DIR) < 0) {
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
	dump_core(cnm);

	/* Change our current working directory to that of the
	 * crashing process */
	snprintf(cwd, sizeof(cwd), "/proc/%s/cwd", argv[4]);
	chdir(cwd);
	return 0;
}

/* codu.c ends here */
