#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "tclstuff.h"
#include <libtcc.h>
#include "jitc.h"
#include "valgrind/memcheck.h"

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

struct jitc_intrep {
	Tcl_Obj*				symbols;
	Tcl_Obj*				cdef;
	Tcl_Obj*				debugfiles;
	Tcl_Interp*				interp;
	Tcl_Obj*				exported_symbols;
	Tcl_Obj*				exported_headers;
	Tcl_LoadHandle			handle;
	void*					execmem;
	struct jit_code_entry	jit_symbols;
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
	LIT_SIZE
};
extern const char*	lit_str[];

struct interp_cx {
	Tcl_Obj*		lit[LIT_SIZE];
};

int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct jitc_intrep** rPtr);

// memfs.c
int Memfs_Init(Tcl_Interp* interp);
int Memfs_Unload(Tcl_Interp* interp);

