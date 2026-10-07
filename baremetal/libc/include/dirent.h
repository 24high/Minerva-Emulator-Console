#ifndef RA_BAREMETAL_DIRENT_H
#define RA_BAREMETAL_DIRENT_H

// newlib for arm-none-eabi has no <dirent.h>. This replacement is backed by
// FatFs, see baremetal/libc/newlib_glue.cpp.

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN	0
#define DT_FIFO		1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK		12

struct dirent
{
	ino_t d_ino;
	unsigned char d_type;
	char d_name[256];
};

typedef struct ra_dirstream DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
void rewinddir(DIR *dirp);
int closedir(DIR *dirp);
int scandir(const char *dirp, struct dirent ***namelist,
	    int (*filter)(const struct dirent *),
	    int (*compar)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);

#ifdef __cplusplus
}
#endif

#endif
