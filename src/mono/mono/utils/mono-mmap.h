#ifndef __MONO_UTILS_MMAP_H__
#define __MONO_UTILS_MMAP_H__

enum {
	/* protection */
	MONO_MMAP_NONE = 0,
	MONO_MMAP_READ    = 1 << 0,
	MONO_MMAP_WRITE   = 1 << 1,
	MONO_MMAP_EXEC    = 1 << 2,
	/* make the OS discard the dirty data and fill with 0 */
	MONO_MMAP_DISCARD = 1 << 3,
	/* other flags (add commit, sync) */
	MONO_MMAP_PRIVATE = 1 << 4,
	MONO_MMAP_SHARED  = 1 << 5,
	MONO_MMAP_ANON    = 1 << 6,
	MONO_MMAP_FIXED   = 1 << 7,
	MONO_MMAP_32BIT   = 1 << 8
};

int   mono_pagesize   (void);
void* mono_valloc     (void *addr, size_t length, int flags);
int   mono_vfree      (void *addr, size_t length);
void* mono_file_map   (size_t length, int flags, int fd, off_t offset, void **ret_handle);
int   mono_file_unmap (void *addr, void *handle);
int   mono_mprotect   (void *addr, size_t length, int flags);

#endif /* __MONO_UTILS_MMAP_H__ */

