#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "tclstuff.h"
#include <libtcc.h>
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include "jitc.h"
#include "valgrind/memcheck.h"
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

// pointer to/from int from tclInt.h
#if !defined(INT2PTR)
#   define INT2PTR(p) ((void *)(ptrdiff_t)(p))
#endif
#if !defined(PTR2INT)
#   define PTR2INT(p) ((ptrdiff_t)(p))
#endif
#if !defined(UINT2PTR)
#   define UINT2PTR(p) ((void *)(size_t)(p))
#endif
#if !defined(PTR2UINT)
#   define PTR2UINT(p) ((size_t)(p))
#endif

#if SIZEOF_VOIDP == 8
#define ELFW(type) ELF64_##type
#else
#define ELFW(type) ELF32_##type
#endif

// Interface with GDB JIT API {{{
typedef enum {
  JIT_NOACTION = 0,
  JIT_REGISTER_FN,
  JIT_UNREGISTER_FN
} jit_actions_t;

struct jit_code_entry {
  struct jit_code_entry *next_entry;
  struct jit_code_entry *prev_entry;
  const char *symfile_addr;
  uint64_t symfile_size;
};

struct jit_descriptor {
  uint32_t version;
  /* This type should be jit_actions_t, but we use uint32_t
     to be explicit about the bitwidth.  */
  uint32_t action_flag;
  struct jit_code_entry *relevant_entry;
  struct jit_code_entry *first_entry;
};

// Interface with GDB JIT API }}}

struct rsym {
	const char*			name;
	void*				addr;
	const ElfW(Sym)*	sym;
};

struct jitc_intrep {
	Tcl_Obj*				cdef;
	Tcl_Obj*				debugfiles;
	Tcl_Interp*				interp;
	Tcl_Obj*				exported_symbols;
	Tcl_Obj*				exported_headers;
	Tcl_Obj*				used;				// Hold references to the foreign cdefs to prevent them from being freed under us
	struct jit_code_entry	jce;

	void**					align_ofs;
	void*					base;
	size_t					map_size;
	uint8_t*				is_mapped;
	void**					section_base;
	void**					got;
	struct plt_entry*		plt;
	Tcl_HashTable			plt_syms;
	Tcl_Obj*				needed;
	Tcl_Obj*				lib_handles;
	Tcl_Obj*				bytesobj;
	uint8_t*				bytes;
	int						len;
	size_t					symc;
	struct rsym*			resolved_symbols;
	Tcl_HashTable			rsyms;
};

struct jitc_instance {
	struct jitc_instance*	next;
	struct jitc_instance*	prev;
	Tcl_Obj*				obj;
};

struct plt_entry {
	void*	target;
	uint8_t	insn[6];
};

enum {
	LIT_BLANK,
	LIT_INCLUDE,
	LIT_GENERIC,
	LIT_LIB,
	LIT_TCC_VAR,
	LIT_INCLUDEPATH_VAR,
	LIT_LIBRARYPATH_VAR,
	LIT_PACKAGEDIR_VAR,
	LIT_PREFIX_VAR,
	LIT_COMPILEERROR,
	LIT_TCLSTUBLIB_CMD,
	LIT_TCLVER_CMD,
	LIT_SIZE
};
extern const char*	lit_str[];

struct interp_cx {
	Tcl_Obj*				lit[LIT_SIZE];
	Tcl_Obj*				tclstublib;
	Tcl_Obj*				tclver;
	struct jitc_instance	instance_head;
	struct jitc_instance	instance_tail;
};

struct proc_binding {
	Tcl_Obj*		cdef;
	Tcl_Obj*		symbol;
	Tcl_ObjCmdProc*	resolved;
	Tcl_Obj*		curryargs;
};

int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct jitc_intrep** rPtr);

// memfs.c
int Memfs_Init(Tcl_Interp* interp);
int Memfs_Unload(Tcl_Interp* interp);

