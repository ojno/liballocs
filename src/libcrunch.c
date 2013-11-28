/* Libcrunch contains all the non-inline code that we need for doing run-time 
 * type checks on C code. */

#define _GNU_SOURCE

#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <link.h>
#include <libunwind.h>
#include "libcrunch.h"

static const char *allocsites_base;
static unsigned allocsites_base_len;

_Bool __libcrunch_is_initialized;
allocsmt_entry_type *__libcrunch_allocsmt;

// FIXME: do better!
static char *realpath_quick_hack(const char *arg)
{
	static char buf[4096];
	return realpath(arg, &buf[0]);
}

static const char *dynobj_name_from_dlpi_name(const char *dlpi_name, void *dlpi_addr)
{
	static char execfile_name[4096];
	if (strlen(dlpi_name) == 0)
	{
		/* libdl can give us an empty name for 
		 *
		 * - the executable;
		 * - itself;
		 * - any others?
		 *
		 * To avoid parsing /proc/self/maps, we use a quick hack: 
		 * ask dladdr, and expect it to know the pathname. It seems
		 * to work.
		 */
		if (dlpi_addr == 0)
		{
			// use /proc to get our executable filename
			int count = readlink("/proc/self/exe", execfile_name, sizeof execfile_name);
			assert(count != -1); // nothing we can do
			
			// use this filename now
			return execfile_name;
		}
		else
		{
			dlerror();
			Dl_info addr_info;
			int dladdr_ret = dladdr(dlpi_addr, &addr_info);
			assert(dladdr_ret != 0);
			assert(addr_info.dli_fname != NULL);
			return realpath_quick_hack(addr_info.dli_fname);
		}
	} 
	else
	{
		// we need to realpath() it
		return realpath_quick_hack(dlpi_name);
	}
}

static const char *helper_libfile_name(const char *objname, const char *suffix)
{
	static char libfile_name[4096];
	unsigned bytes_left = sizeof libfile_name - 1;
	
	libfile_name[0] = '\0';
	bytes_left--;
	// append the uniqtypes base path
	strncat(libfile_name, allocsites_base, bytes_left);
	bytes_left -= (bytes_left < allocsites_base_len) ? bytes_left : allocsites_base_len;
	
	// now append the object name
	unsigned file_name_len = strlen(objname);
	assert(file_name_len > 0);
	strncat(libfile_name, objname, bytes_left);
	bytes_left -= (bytes_left < file_name_len) ? bytes_left : file_name_len;
	
	// now append the suffix
	strncat(libfile_name, suffix, bytes_left);
	// no need to compute the last bytes_left
	
	return &libfile_name[0];
}	

static int load_types_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	// get the canonical libfile name
	const char *canon_objname = dynobj_name_from_dlpi_name(info->dlpi_name, (void *) info->dlpi_addr);

	// skip objects that are themselves types/allocsites objects
	if (0 == strncmp(canon_objname, allocsites_base, allocsites_base_len)) return 0;
	
	// get the -types.so object's name
	const char *libfile_name = helper_libfile_name(canon_objname, "-types.so");
	// don't load if we end with "-types.so"
	if (0 == strcmp("-types.so", canon_objname + strlen(canon_objname) - strlen("-types.so")))
	{
		return 0;
	}

	// fprintf(stderr, "libcrunch: trying to open %s\n", libfile_name);

	dlerror();
	void *handle = dlopen(libfile_name, RTLD_NOW | RTLD_GLOBAL);
	if (!handle)
	{
		warnx("Could not load types object (%s)", dlerror());
		return 0;
	}
	
	// always continue with further objects
	return 0;
}

static void chain_allocsite_entries(struct allocsite_entry *cur_ent, 
	struct allocsite_entry *prev_ent, unsigned *p_current_bucket_size, 
	intptr_t load_addr, intptr_t extrabits)
{
#define FIXADDR(a) 	((void*)((intptr_t)(a) | extrabits))

	// fix up the allocsite by the containing object's load address
	*((unsigned char **) &cur_ent->allocsite) += load_addr;

	// debugging: print out entry
	/* fprintf(stderr, "allocsite entry: %p, to uniqtype at %p\n", 
		cur_ent->allocsite, cur_ent->uniqtype); */

	// if we've moved to a different bucket, point the table entry at us
	struct allocsite_entry **bucketpos = ALLOCSMT_FUN(ADDR, FIXADDR(cur_ent->allocsite));
	struct allocsite_entry **prev_ent_bucketpos
	 = prev_ent ? ALLOCSMT_FUN(ADDR, FIXADDR(prev_ent->allocsite)) : NULL;

	// first iteration is too early to do chaining, 
	// but we do need to set up the first bucket
	if (!prev_ent || bucketpos != prev_ent_bucketpos)
	{
		// fresh bucket, so should be null
		assert(!*bucketpos);
		*bucketpos = cur_ent;
	}
	if (!prev_ent) return;

	void *cur_range_base = ALLOCSMT_FUN(ADDR_RANGE_BASE, FIXADDR(cur_ent->allocsite));
	void *prev_range_base = ALLOCSMT_FUN(ADDR_RANGE_BASE, FIXADDR(prev_ent->allocsite));

	if (cur_range_base == prev_range_base)
	{
		// chain these guys together
		prev_ent->next = cur_ent;
		cur_ent->prev = prev_ent;

		++(*p_current_bucket_size);
	} else *p_current_bucket_size = 1; 
	// we don't (currently) distinguish buckets of zero from buckets of one

	// last iteration doesn't need special handling -- next will be null,
	// prev will be set within the "if" above, if it needs to be set.
#undef FIXADDR
}

static int load_and_init_allocsites_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	// get the canonical libfile name
	const char *canon_objname = dynobj_name_from_dlpi_name(info->dlpi_name, (void *) info->dlpi_addr);

	// skip objects that are themselves types/allocsites objects
	if (0 == strncmp(canon_objname, allocsites_base, allocsites_base_len)) return 0;
	
	// get the -allocsites.so object's name
	const char *libfile_name = helper_libfile_name(canon_objname, "-allocsites.so");
	// don't load if we end with "-allocsites.so"
	if (0 == strcmp("-allocsites.so", canon_objname + strlen(canon_objname) - strlen("-allocsites.so")))
	{
		return 0;
	}

	// fprintf(stderr, "libcrunch: trying to open %s\n", libfile_name);
	dlerror();
	void *allocsites_handle = dlopen(libfile_name, RTLD_NOW);
	if (!allocsites_handle)
	{
		warnx("Could not load allocsites object (%s)", dlerror());
		return 0;
	}
	
	dlerror();
	struct allocsite_entry *first_entry = (struct allocsite_entry *) dlsym(allocsites_handle, "allocsites");
	// allocsites cannot be null anyhow
	assert(first_entry && "symbol 'allocsites' must be present in -allocsites.so"); 

	/* We walk through allocsites in this object, chaining together those which
	 * should be in the same bucket. NOTE that this is the kind of thing we'd
	 * like to get the linker to do for us, but it's not quite expressive enough. */
	struct allocsite_entry *cur_ent = first_entry;
	struct allocsite_entry *prev_ent = NULL;
	unsigned current_bucket_size = 1; // out of curiosity...
	for (; cur_ent->allocsite; prev_ent = cur_ent++)
	{
		chain_allocsite_entries(cur_ent, prev_ent, &current_bucket_size, 
			info->dlpi_addr, 0);
	}

	// debugging: check that we can look up the first entry, if we are non-empty
	assert(!first_entry || 
		allocsite_to_uniqtype(first_entry->allocsite) == first_entry->uniqtype);
	
	// always continue with further objects
	return 0;
}

static int link_stackaddr_allocs_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	// get the canonical libfile name
	const char *canon_objname = dynobj_name_from_dlpi_name(info->dlpi_name, (void *) info->dlpi_addr);

	// skip objects that are themselves types/allocsites objects
	if (0 == strncmp(canon_objname, allocsites_base, allocsites_base_len)) return 0;
	
	// get the -allocsites.so object's name
	const char *libfile_name = helper_libfile_name(canon_objname, "-types.so");
	// don't load if we end with "-types.so"
	if (0 == strcmp("-types.so", canon_objname + strlen(canon_objname) - strlen("-types.so")))
	{
		return 0;
	}

	dlerror();
	void *types_handle = dlopen(libfile_name, RTLD_NOW | RTLD_NOLOAD);
	if (!types_handle)
	{
		warnx("Could not re-load types object (%s)", dlerror());
		return 0;
	}
	
	dlerror();
	struct allocsite_entry *first_entry = (struct allocsite_entry *) dlsym(types_handle, "frame_vaddrs");
	if (!first_entry)
	{
		warnx("Could not load frame vaddrs (%s)", dlerror());
		return 0;
	}
	
	/* We chain these much like the allocsites, BUT we OR each vaddr with 
	 * STACK_BEGIN first.  */
	struct allocsite_entry *cur_ent = first_entry;
	struct allocsite_entry *prev_ent = NULL;
	unsigned current_bucket_size = 1; // out of curiosity...
	for (; cur_ent->allocsite; prev_ent = cur_ent++)
	{
		chain_allocsite_entries(cur_ent, prev_ent, &current_bucket_size,
			info->dlpi_addr, STACK_BEGIN);
	}

	// debugging: check that we can look up the first entry, if we are non-empty
	assert(!first_entry || 
		vaddr_to_uniqtype(first_entry->allocsite) == first_entry->uniqtype);
	
	// always continue with further objects
	return 0;
	
}

const struct rec *__libcrunch_uniqtype_void; // remember the location of the void uniqtype
/* counters */
unsigned long __libcrunch_begun;
unsigned long __libcrunch_aborted_init;
unsigned long __libcrunch_aborted_stack;
unsigned long __libcrunch_aborted_static;
unsigned long __libcrunch_aborted_typestr;
unsigned long __libcrunch_aborted_unknown_storage;
unsigned long __libcrunch_aborted_unindexed_heap;
unsigned long __libcrunch_aborted_unrecognised_allocsite;
unsigned long __libcrunch_failed;
unsigned long __libcrunch_trivially_succeeded_null;
unsigned long __libcrunch_trivially_succeeded_void;
unsigned long __libcrunch_succeeded;

static void print_exit_summary(void)
{
	fprintf(stderr, "libcrunch summary: \n");
	fprintf(stderr, "checks begun:                          % 7ld\n", __libcrunch_begun);
	fprintf(stderr, "checks aborted due to init failure:    % 7ld\n", __libcrunch_aborted_init);
	fprintf(stderr, "checks aborted for stack objects:      % 7ld\n", __libcrunch_aborted_stack);
	fprintf(stderr, "checks aborted for static objects:     % 7ld\n", __libcrunch_aborted_static);
	fprintf(stderr, "checks aborted for unrecognised type   % 7ld\n", __libcrunch_aborted_typestr);
	fprintf(stderr, "checks aborted for unknown storage:    % 7ld\n", __libcrunch_aborted_unknown_storage);
	fprintf(stderr, "checks aborted for unindexed heap:     % 7ld\n", __libcrunch_aborted_unindexed_heap);
	fprintf(stderr, "checks aborted for unrecognised alloc: % 7ld\n", __libcrunch_aborted_unrecognised_allocsite);
	fprintf(stderr, "checks failed:                         % 7ld\n", __libcrunch_failed);
	fprintf(stderr, "checks trivially passed on null ptr:   % 7ld\n", __libcrunch_trivially_succeeded_null);
	fprintf(stderr, "checks trivially passed for void type: % 7ld\n", __libcrunch_trivially_succeeded_void);
	fprintf(stderr, "checks nontrivially passed:            % 7ld\n", __libcrunch_succeeded);
}

/* This is *not* a constructor. We don't want to be called too early,
 * because it might not be safe to open the -uniqtypes.so handle yet.
 * So, initialize on demand. */
int __libcrunch_global_init(void)
{
	if (__libcrunch_is_initialized) return 0; // we are okay
	static _Bool tried_to_initialize;
	
	if (tried_to_initialize) return -1;
	tried_to_initialize = 1;
	
	// print a summary when the program exits
	atexit(print_exit_summary);

	// the user can specify where we get our -types.so and -allocsites.so
	allocsites_base = getenv("ALLOCSITES_BASE");
	if (!allocsites_base) allocsites_base = "/usr/lib/allocsites";
	allocsites_base_len = strlen(allocsites_base);
	
	int ret_types = dl_iterate_phdr(load_types_cb, NULL);
	assert(ret_types == 0);
	
	/* Allocate the memtable. 
	 * Assume we don't need to cover addresses >= STACK_BEGIN.
	 * BUT we store vaddrs in the same table, with addresses ORed
	 * with STACK_BEGIN. So double up the size of the table accordingly. */
	__libcrunch_allocsmt = MEMTABLE_NEW_WITH_TYPE(allocsmt_entry_type, allocsmt_entry_coverage, 
		(void*) 0, (void*) (STACK_BEGIN << 1));
	assert(__libcrunch_allocsmt != MAP_FAILED);
	
	int ret_allocsites = dl_iterate_phdr(load_and_init_allocsites_cb, NULL);
	assert(ret_allocsites == 0);

	int ret_stackaddr = dl_iterate_phdr(link_stackaddr_allocs_cb, NULL);
	assert(ret_stackaddr == 0);
	
	__libcrunch_is_initialized = 1;
	fprintf(stderr, "libcrunch successfully initialized\n");
	return 0;
}

void *typeobj_handle_for_addr(void *caller)
{
	// find out what object the caller is in
	Dl_info info;
	dlerror();
	int dladdr_ret = dladdr(caller, &info);
	assert(dladdr_ret != 0);
	
	// dlopen the typeobj
	const char *types_libname = helper_libfile_name(dynobj_name_from_dlpi_name(info.dli_fname, info.dli_fbase), "-types.so");
	return dlopen(types_libname, RTLD_NOW | RTLD_NOLOAD);
}

/* FIXME: hook dlopen and dlclose so that we can load/unload allocsites and types
 * as execution proceeds. */

/* This is left out-of-line because it's inherently a slow path. */
struct rec *typestr_to_uniqtype(const char *typestr)
{
	if (!typestr) return NULL;
	
	/* Note that the client always gives us a header-based typestr to look up. 
	 * We erase the header part and walk symbols in the -types.so to look for 
	 * a unique match. FIXME: this requires us to define aliases in unique cases! 
	 * in types.so, so dumptypes has to do this. */
	int prefix_len = strlen("__uniqtype_");
	assert(0 == strncmp(typestr, "__uniqtype_", prefix_len));
	int header_name_len;
	int nmatched = sscanf(typestr, "__uniqtype_%d", &header_name_len);
	char typestr_to_use[4096];
	if (nmatched == 1)
	{
		// assert sanity
		assert(header_name_len > 0 && header_name_len < 4096);
		// read the remainder
		typestr_to_use[0] = '\0';
		strcat(typestr_to_use, "__uniqtype_");
		strncat(typestr_to_use, typestr + prefix_len + header_name_len, 4096 - prefix_len);
		typestr = typestr_to_use;
	} // else assume it's already how we like it
	
	dlerror();
	// void *returned = dlsym(RTLD_DEFAULT, typestr);
	void *caller = __builtin_return_address(1);
	// RTLD_GLOBAL means that we don't need to get the handle
	// void *returned = dlsym(typeobj_handle_for_addr(caller), typestr);
	void *returned = dlsym(RTLD_DEFAULT, typestr);
	if (!returned) return NULL;
	if (!__libcrunch_uniqtype_void && strcmp(typestr, "__uniqtype__void") == 0)
	{
		__libcrunch_uniqtype_void = (struct rec *) returned;
	}
	return (struct rec *) returned;
}
/* Optimised version, for when you already know the uniqtype address. */
int __is_aU(const void *obj, const struct rec *test_uniqtype)
{
	const char *reason = NULL; // if we abort, set this to a string lit
	const void *reason_ptr = NULL; // if we abort, set this to a useful address
	_Bool suppress_warning = 0;
	
	// NOTE: we handle separately the path where __is_a fails because of init failure
	++__libcrunch_begun;
	
	/* A null pointer always satisfies is_a. */
	if (!obj) { ++__libcrunch_trivially_succeeded_null; return 1; }
	/* Any pointer satisfies void. We do this both here and in typestr_to_uniqtype,
	 * in case we're not called through the __is_a typestr-based interface. */
	if (!__libcrunch_uniqtype_void)
	{
		if (strcmp(test_uniqtype->name, "void") == 0)
		{
			__libcrunch_uniqtype_void = test_uniqtype;
		}
	}
	if (__libcrunch_uniqtype_void && test_uniqtype == __libcrunch_uniqtype_void)
	{ ++__libcrunch_trivially_succeeded_void; return 1; }
	
	/* It's okay to assume we're inited, otherwise how did the caller
	 * get the uniqtype in the first place? */
	
	/* To get the uniqtype for obj, we need to determine its memory
	 * location. x86-64 only! */
	void *object_start;
	unsigned offset;
	unsigned block_element_count = 1;
	struct rec *alloc_uniqtype = (struct rec *)0;

/* HACK: pasted from heap.cpp in libpmirror */
/* Do I want to pad to 4, 8 or (=== 4 (mod 8)) bytes? 
 * Try 4 mod 8. */
#define PAD_TO_NBYTES(s, n) (((s) % (n) == 0) ? (s) : ((((s) / (n)) + 1) * (n)))
#define PAD_TO_MBYTES_MOD_N(s, n, m) (((s) % (n) <= (m)) \
? ((((s) / (n)) * (n)) + (m)) \
: (((((s) / (n)) + 1) * (n)) + (m)))
#define USABLE_SIZE_FROM_OBJECT_SIZE(s) (PAD_TO_MBYTES_MOD_N( ((s) + sizeof (struct trailer)) , 8, 4))
#define HEAPSZ_ONE(t) (USABLE_SIZE_FROM_OBJECT_SIZE(sizeof ((t))))

	switch(get_object_memory_kind(obj))
	{
		case STACK:
		{
#ifdef UNW_TARGET_X86
#define BEGINNING_OF_STACK 0xbfffffff
#else // assume X86_64 for now
#define BEGINNING_OF_STACK (STACK_BEGIN - 1)
#endif
			// we want to walk a sequence of vaddrs!
			// how do we know which is the one we want?
			// we can get a uniqtype for each one, including maximum posoff and negoff
			// -- yes, use those
			// begin pasted-then-edited from stack.cpp in pmirror
			/* We declare all our variables up front, in the hope that we can rely on
			 * the stack pointer not moving between getcontext and the sanity check.
			 * FIXME: better would be to write this function in C90 and compile with
			 * special flags. */
			unw_cursor_t cursor, saved_cursor, prev_saved_cursor;
			unw_word_t higherframe_sp = 0, sp, higherframe_ip = 0, callee_ip;
			int unw_ret;
			unw_word_t check_higherframe_sp;
			unw_context_t unw_context;

			// sanity check
#ifdef UNW_TARGET_X86
			__asm__ ("movl %%esp, %0\n" :"=r"(check_higherframe_sp));
#else // assume X86_64 for now
			__asm__("movq %%rsp, %0\n" : "=r"(check_higherframe_sp));
#endif
			unw_ret = unw_getcontext(&unw_context);
			unw_init_local(&cursor, /*this->unw_as,*/ &unw_context);

			unw_ret = unw_get_reg(&cursor, UNW_REG_SP, &higherframe_sp);
			assert(check_higherframe_sp == higherframe_sp);
			fprintf(stderr, "Initial sp=%p\n", (void *) higherframe_sp);
			unw_ret = unw_get_reg(&cursor, UNW_REG_IP, &higherframe_ip);

			unw_word_t ip = 0;
			int step_ret;

			do
			{
				callee_ip = ip;
				prev_saved_cursor = saved_cursor;	// prev_saved_cursor is the cursor into the callee's frame 
													// FIXME: will be garbage if callee_ip == 0
				saved_cursor = cursor; // saved_cursor is the *current* frame's cursor
					// and cursor, later, becomes the *next* (i.e. caller) frame's cursor

				/* First get the ip, sp and symname of the current stack frame. */
				unw_ret = unw_get_reg(&cursor, UNW_REG_IP, &ip); assert(unw_ret == 0);
				unw_ret = unw_get_reg(&cursor, UNW_REG_SP, &sp); assert(unw_ret == 0); // sp = higherframe_sp

				/* Now get the sp of the next higher stack frame, 
				 * i.e. the bp of the current frame. N

				 * NOTE: we're still
				 * processing the stack frame ending at sp, but we
				 * hoist the unw_step call to here so that we can get
				 * the bp of the next higher frame (without demanding that
				 * libunwind provides bp, e.g. for code compiled with
				 * -fomit-frame-pointer -- FIXME: does this work?). 
				 * This means "cursor" is no longer current -- use 
				 * saved_cursor for the remainder of this iteration!
				 * saved_cursor points to the deeper stack frame. */
				int step_ret = unw_step(&cursor);
				if (step_ret > 0)
				{
					unw_ret = unw_get_reg(&cursor, UNW_REG_SP, &higherframe_sp); assert(unw_ret == 0);
					unw_ret = unw_get_reg(&cursor, UNW_REG_IP, &higherframe_ip); assert(unw_ret == 0);
				}
				else if (step_ret == 0)
				{
					higherframe_sp = BEGINNING_OF_STACK;
					higherframe_ip = 0x0;
				}
				else
				{
					// return value <1 means error
					// We return without calling the handler. This does mean that
					// the very top frame will not get handler'd
					break;
				}

// 				ret = handler(
// 				/* process_image *image			 */ this,
// 				/* unw_word_t frame_sp			  */ sp,
// 				/* unw_word_t frame_ip			  */ ip,
// 				/* const char *frame_proc_name	  */ name,
// 				/* unw_word_t frame_caller_sp	   */ higherframe_sp,
// 				/* unw_word_t frame_caller_ip	   */ higherframe_ip,
// 				/* unw_word_t frame_callee_ip	   */ callee_ip,
// 				/* unw_cursor_t frame_cursor		*/ saved_cursor,
// 				/* unw_cursor_t frame_callee_cursor */ prev_saved_cursor,
// 				/* void *arg						*/ handler_arg
// 				);

				// now do the stuff
				// 1. get the frame uniqtype for frame_ip
				/* NOTE: here we are doing one vaddr_to_uniqtype per frame.
				 * Can we optimise this, by ruling out some frames just by
				 * their bounding sps? YES, I'm sure we can. FIXME: do this!
				 */
				struct rec *frame_desc = vaddr_to_uniqtype((void *) ip);
				if (!frame_desc)
				{
					// no frame descriptor for this frame; that's okay!
					continue;
				}
				// 2. what's the frame base? it's the higherframe stack pointer
				unsigned char *frame_base = (unsigned char *) higherframe_sp;
				// 3. is our candidate addr between frame-base - negoff and frame_base + posoff?
				if ((unsigned char *) obj >= frame_base + frame_desc->neg_maxoff  // is -ve, so 
					&& (unsigned char *) obj < frame_base + frame_desc->pos_maxoff)
				{
					object_start = frame_base;
					alloc_uniqtype = frame_desc;
					break;
				}
				else
				{
					// have we gone too far? we are going upwards in memory...
					// ... so if our lowest addr is still too high
					if (frame_base + frame_desc->neg_maxoff > (unsigned char *) obj)
					{
						reason = "stack walk reached higher frame";
						goto abort_stack;
					}
				}

				assert(step_ret > 0 || higherframe_sp == BEGINNING_OF_STACK);
			} while (higherframe_sp != BEGINNING_OF_STACK);
			// if we hit the termination condition, we've failed
			if (higherframe_sp == BEGINNING_OF_STACK)
			{
				reason = "stack walk reached top-of-stack";
				goto abort_stack; //std::shared_ptr<dwarf::spec::basic_die>();
			}
		#undef BEGINNING_OF_STACK
		// end pasted from pmirror stack.cpp
		break; // end case STACK
		abort_stack:
			reason = reason ? reason : "stack object";
			reason_ptr = obj;
			suppress_warning = 1;
			++__libcrunch_aborted_stack;
			goto abort;
			//void *uniqtype = stack_frame_to_uniqtype(frame_base, file_relative_ip);
		} // end case STACK
		case HEAP:
		{
			/* For heap allocations, we look up the allocation site.
			 * (This also yields an offset within a toplevel object.)
			 * Then we translate the allocation site to a uniqtypes rec location.
			 * (For direct calls in eagerly-loaded code, we can cache this information
			 * within uniqtypes itself. How? Make uniqtypes include a hash table with
			 * initial contents mapping allocsites to uniqtype recs. This hash table
			 * is initialized during load, but can be extended as new allocsites
			 * are discovered, e.g. indirect ones.)
			 */			
			struct trailer *heap_info = lookup_object_info(obj, &object_start);
			if (!heap_info)
			{
				reason = "unindexed heap object";
				reason_ptr = obj;
				++__libcrunch_aborted_unindexed_heap;
				goto abort;
			}

			// now we have an allocsite
			alloc_uniqtype = allocsite_to_uniqtype((void*)(intptr_t)heap_info->alloc_site);
			if (!alloc_uniqtype) 
			{
				reason = "unrecognised allocsite";
				reason_ptr = (void*)(intptr_t)heap_info->alloc_site;
				++__libcrunch_aborted_unrecognised_allocsite;
				goto abort;
			}
			
			/* FIXME: do we want to write the uniqtype directly into the heap trailer?
			 * PROBABLY, but do this LATER once we can MEASURE the BENEFIT!
			 * -- we can scrounge the union tag bits as follows:
			 *    on 32-bit x86, exploit that code is not loaded in top half of AS;
			 *    on 64-bit x86, exploit that certain bits of an addr are always 0. 
			 */
			 
			unsigned chunk_size = malloc_usable_size(object_start);
			unsigned padded_trailer_size = USABLE_SIZE_FROM_OBJECT_SIZE(0);
			block_element_count = (chunk_size - padded_trailer_size) / alloc_uniqtype->pos_maxoff;
			//__libcrunch_private_assert(chunk_size % alloc_uniqtype->pos_maxoff == 0,
			//	"chunk size should be multiple of element size", __FILE__, __LINE__, __func__);
			break;
		}
		case STATIC:
		{
			//void *uniqtype = static_obj_to_uniqtype(object_start);
			reason = "static object";
			reason_ptr = obj;
			++__libcrunch_aborted_static;
			goto abort;
		}
		case UNKNOWN:
		default:
		{
			reason = "object of unknown storage";
			reason_ptr = obj;
			++__libcrunch_aborted_unknown_storage;
			goto abort;
		}
	}
		
	/* Now search iteratively for a match at the offset within the toplevel
	 * object. Nonzero offsets "recurse" immediately, using binary search. */
	assert(alloc_uniqtype);
	/* If we're searching in an array, we need to take the offset modulo the 
	 * element size. */
	int modulo; 
	if (alloc_uniqtype->pos_maxoff != 0 && alloc_uniqtype->neg_maxoff == 0)
	{	
		modulo = alloc_uniqtype->pos_maxoff;
	} else modulo = 1;
	unsigned target_offset = ((char*) obj - (char*) object_start) % modulo; 
	
	struct rec *cur_obj_uniqtype = alloc_uniqtype;
	signed descend_to_ind;
	do
	{
		/* If we have offset == 0, we can check at this uniqtype. */
		if (cur_obj_uniqtype == test_uniqtype) 
		{
		temp_label: // HACK: remove this printout once stable
			warnx("Check __is_aU(%p, %p a.k.a. \"%s\") succeeded at %p.\n", 
				obj, test_uniqtype, test_uniqtype->name, &&temp_label);
			++__libcrunch_succeeded;
			return 1;
		}
	
		/* calculate the offset to descend to, if any 
		 * FIXME: refactor into find_subobject_spanning(offset) */
		unsigned num_contained = cur_obj_uniqtype->nmemb;
		int lower_ind = 0;
		int upper_ind = num_contained;
		while (lower_ind + 1 < upper_ind) // difference of >= 2
		{
			/* Bisect the interval */
			int bisect_ind = (upper_ind + lower_ind) / 2;
			__libcrunch_private_assert(bisect_ind > lower_ind, "bisection progress", 
				__FILE__, __LINE__, __func__);
			if (cur_obj_uniqtype->contained[bisect_ind].offset > target_offset)
			{
				/* Our solution lies in the lower half of the interval */
				upper_ind = bisect_ind;
			} else lower_ind = bisect_ind;
		}
		if (lower_ind + 1 == upper_ind)
		{
			/* We found one offset */
			__libcrunch_private_assert(cur_obj_uniqtype->contained[lower_ind].offset <= target_offset,
				"offset underapproximates", __FILE__, __LINE__, __func__);
			descend_to_ind = lower_ind;
		}
		else /* lower_ind >= upper_ind */
		{
			// this should mean num_contained == 0
			__libcrunch_private_assert(num_contained == 0,
				"no contained objects", __FILE__, __LINE__, __func__);
			descend_to_ind = -1;
		}
		
		/* Terminate or recurse. */
	} while (descend_to_ind != -1 
	    && (cur_obj_uniqtype = cur_obj_uniqtype->contained[descend_to_ind].ptr,
	        target_offset = target_offset - cur_obj_uniqtype->contained[descend_to_ind].offset,
	        1));
	// if we got here, the check failed
	goto check_failed;
	
	__assert_fail("unreachable", __FILE__, __LINE__, __func__);
check_failed:
	++__libcrunch_failed;
	warnx("Failed check __is_aU(%p, %p a.k.a. \"%s\") at %p, allocation was a %p (a.k.a. \"%s\")\n", 
		obj, test_uniqtype, test_uniqtype->name,
		&&check_failed /* we are inlined, right? */,
		alloc_uniqtype, alloc_uniqtype->name);
	return 1;

abort:
	// if (!suppress_warning) warnx("Aborted __is_aU(%p, %p) at %p, reason: %s (%p)\n", obj, uniqtype,
	//	&&abort /* we are inlined, right? */, reason, reason_ptr); 
	return 1; // so that the program will continue
}

/* Force instantiation of __is_a3 in libcrunch, by repeating its prototype.
 * We do this so that linking an application -lcrunch is sufficient to
 * generate a weak *dynamic* reference to __is_a3. */
int __is_a(const void *obj, const char *typestr);
int __is_a3(const void *obj, const char *typestr, const struct rec **maybe_uniqtype);
// DON'T instantiate __is_aU -- this is an internal function and should be inlined
// int __is_aU(const void *obj, const struct rec *uniqtype);

// same for initialization and, in fact, everything....
int __libcrunch_check_init(void);
enum object_memory_kind get_object_memory_kind(const void *obj);
struct rec *allocsite_to_uniqtype(const void *allocsite);
struct rec *vaddr_to_uniqtype(const void *allocsite);
struct rec *typestr_to_uniqtype(const char *typestr);

