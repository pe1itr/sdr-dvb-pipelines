/* Allocation and release must use the same CRT API on Windows. */
#ifndef LINRAD_ALIGNED_MEMORY_H
#define LINRAD_ALIGNED_MEMORY_H
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#define linrad_aligned_alloc(alignment,size) _aligned_malloc(size,alignment)
#define linrad_aligned_free(pointer) _aligned_free(pointer)
#else
#define linrad_aligned_alloc(alignment,size) aligned_alloc(alignment,size)
#define linrad_aligned_free(pointer) free(pointer)
#endif
#endif
