#ifndef __MONO_MPLOG_UTILS_H__
#define __MONO_MPLOG_UTILS_H__

#include <inttypes.h>

void utils_init (int fast_time);
uint64_t current_time (void);
void* alloc_buffer (int size);
void free_buffer (void *buf, int size);
void take_lock (void);
void release_lock (void);
uintptr_t thread_id (void);

void encode_uleb128 (uint64_t value, uint8_t *buf, uint8_t **endbuf);
void encode_sleb128 (intptr_t value, uint8_t *buf, uint8_t **endbuf);
uint64_t decode_uleb128 (uint8_t *buf, uint8_t **endbuf);
intptr_t decode_sleb128 (uint8_t *buf, uint8_t **endbuf);


#endif /* __MONO_MPLOG_UTILS_H__ */

