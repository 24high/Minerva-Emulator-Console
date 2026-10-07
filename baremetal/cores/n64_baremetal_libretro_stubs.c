#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <boolean.h>

typedef unsigned char *pu8;
struct r4300_core;

pu8 DMEM;
pu8 IMEM;

uint64_t cpu_features_get(void)
{
	return 0;
}

void dyna_jump(void)
{
}

void dyna_stop(struct r4300_core *r4300)
{
	(void)r4300;
}

void dynarec_jump_to(struct r4300_core *r4300, uint32_t address)
{
	(void)r4300;
	(void)address;
}

void dynarec_jump_to_recomp_address(void)
{
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	(void)prot;
	(void)fd;
	(void)offset;

	if ((flags & MAP_FIXED) && addr)
	{
		return addr;
	}

	void *ptr = malloc(len);
	return ptr ? ptr : MAP_FAILED;
}

int munmap(void *addr, size_t len)
{
	(void)addr;
	(void)len;
	return 0;
}

// libc/circle_bridge.cpp
int ra_libc_make_executable(void *pStart, unsigned long nLength);

// The dynarec keeps its code in .bss, which Circle maps execute-never.
int mprotect(void *addr, size_t len, int prot)
{
	if ((prot & PROT_EXEC) && ra_libc_make_executable(addr, len) != 0)
	{
		return -1;
	}
	return 0;
}

long sysconf(int name)
{
	(void)name;	// only _SC_PAGESIZE is asked for
	return 4096;
}

char *strdup(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
	{
		return 0;
	}

	len = strlen(value) + 1;
	copy = (char *)malloc(len);
	if (copy)
	{
		memcpy(copy, value, len);
	}
	return copy;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (!memptr || alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
	{
		return 22;
	}

	if (size >= 0x10000000U)
	{
		*memptr = 0;
		return 12;
	}

	(void)alignment;
	*memptr = malloc(size);
	if (!*memptr)
	{
		return 12;
	}
	return 0;
}

void *_sbrk(ptrdiff_t incr)
{
	(void)incr;
	return (void *)-1;
}

void _init(void)
{
}

void _fini(void)
{
}

int _close(int fd)
{
	(void)fd;
	return -1;
}

int _fstat(int fd, void *st)
{
	(void)fd;
	(void)st;
	return -1;
}

int _getpid(void)
{
	return 1;
}

int _gettimeofday(void *tv, void *tz)
{
	(void)tv;
	(void)tz;
	return -1;
}

int _isatty(int fd)
{
	(void)fd;
	return 0;
}

int _kill(int pid, int sig)
{
	(void)pid;
	(void)sig;
	return -1;
}

int _lseek(int fd, int offset, int whence)
{
	(void)fd;
	(void)offset;
	(void)whence;
	return -1;
}

int _open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	return -1;
}

int _read(int fd, void *buf, unsigned count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return -1;
}

int _write(int fd, const void *buf, unsigned count)
{
	(void)fd;
	(void)buf;
	return (int)count;
}

int config_userdata_get_float(void *userdata, const char *key_str,
	float *value, float default_value)
{
	(void)userdata;
	(void)key_str;
	if (value)
	{
		*value = default_value;
	}
	return 0;
}

int config_userdata_get_int(void *userdata, const char *key_str,
	int *value, int default_value)
{
	(void)userdata;
	(void)key_str;
	if (value)
	{
		*value = default_value;
	}
	return 0;
}

int config_userdata_get_float_array(void *userdata, const char *key_str,
	float **values, unsigned *out_num_values,
	const float *default_values, unsigned num_default_values)
{
	(void)userdata;
	(void)key_str;
	if (out_num_values)
	{
		*out_num_values = num_default_values;
	}
	if (!values || !num_default_values)
	{
		return 0;
	}
	*values = (float *)calloc(num_default_values, sizeof(float));
	if (*values && default_values)
	{
		memcpy(*values, default_values, num_default_values * sizeof(float));
	}
	return 0;
}

int config_userdata_get_int_array(void *userdata, const char *key_str,
	int **values, unsigned *out_num_values,
	const int *default_values, unsigned num_default_values)
{
	(void)userdata;
	(void)key_str;
	if (out_num_values)
	{
		*out_num_values = num_default_values;
	}
	if (!values || !num_default_values)
	{
		return 0;
	}
	*values = (int *)calloc(num_default_values, sizeof(int));
	if (*values && default_values)
	{
		memcpy(*values, default_values, num_default_values * sizeof(int));
	}
	return 0;
}

int config_userdata_get_string(void *userdata, const char *key_str,
	char **output, const char *default_output)
{
	(void)userdata;
	(void)key_str;
	if (!output)
	{
		return 0;
	}
	*output = default_output ? strdup(default_output) : 0;
	return 0;
}

void config_userdata_free(void *ptr)
{
	free(ptr);
}
