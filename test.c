#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char *argv[])
{
	void *bla = mmap(NULL, 90112000,
			 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
	memset(bla + 1024, -1, 16384);
	abort();
	return 0;
}
