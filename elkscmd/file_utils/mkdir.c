#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

static unsigned short newmode;

static int make_dir(char *name, int f)
{
	char iname[256];
	char *line;

	strcpy(iname, name);
	if (((line = rindex(iname,'/')) != NULL) && f) {
		while ((line > iname) && (*line == '/'))
			--line;
		line[1] = 0;
		make_dir(iname,1);
	}
	if (mkdir(name, newmode) && !f)
		return(1);
	else
		return(0);

}


int main(int argc, char **argv)
{
	int i, parent = 0, er = 0;

	if (argc < 2) goto usage;

	if ((argv[1][0] == '-') && (argv[1][1] == 'p'))
		parent = 1;

	newmode = 0777 & ~umask(0);

	for (i = parent + 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			if (argv[i][strlen(argv[i])-1] == '/')
				argv[i][strlen(argv[i])-1] = '\0';

			if (make_dir(argv[i],parent)) {
				fprintf(stderr, "mkdir: cannot create directory: %s\n", argv[i]);
				er = 1;
			}
		} else goto usage;
	}
	exit(er);

usage:
	fprintf(stderr, "usage: %s new_dir1 [new_dir2] ...\n", argv[0]);
	exit(1);
}
