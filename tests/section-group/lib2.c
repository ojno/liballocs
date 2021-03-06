#define _GNU_SOURCE
#include <dlfcn.h>
#include <assert.h>
#include "liballocs.h"

struct uniqtype_cache_word
{
	unsigned long addr:47;
	unsigned flag:1;
	unsigned bits:16;
};
struct uniqtype
{
	struct uniqtype_cache_word cache_word;
	const char *name;
	unsigned short pos_maxoff; // 16 bits
	unsigned short neg_maxoff; // 16 bits
	unsigned nmemb:12;	 // 12 bits -- number of `contained's (always 1 if array)
	unsigned is_array:1;       // 1 bit
	unsigned array_len:19;     // 19 bits; 0 means undetermined length
	struct contained { 
		signed offset;
		struct uniqtype *ptr;
	} contained[];
};

extern struct uniqtype *l2a(void);

struct s2
{
	int x;
} s;

void *l2(int arg)
{
	/* Get our __uniqtype__s1. */
	struct uniqtype *resolved = dlsym(__liballocs_my_typeobj(), "__uniqtype__s2");
	struct uniqtype *int32 = resolved->contained[0].ptr;
	
	/* Check that we're using the same "__uniqtype_int$32" as l2a is. */
	assert(l2a() == int32);
	
	/* Pass our pointer up to main(), so it can test globally. */
	return int32;
}
