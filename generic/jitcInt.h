#include <config.h>

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <defer.h>
#include <tclstuff.h>
// libslimcc.h pulls in mir.h, whose DEF_DLIST/DEF_VARR macros expand to
// declarations followed by a ';' at file scope — legal C, but -Wpedantic
// (jitc builds with -Werror -Wpedantic) flags the trailing semicolons. It's a
// third-party header; silence pedantic diagnostics just across these includes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "libslimcc.h"
#include "mir-gen.h"
#pragma GCC diagnostic pop
#include <jitc.h>
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

// The GDB JIT interface (see gdb's "Registering Code" docs). A debugger sets
// a breakpoint on __jit_debug_register_code; we publish a linked list of
// in-memory object files through __jit_debug_descriptor and trip the
// breakpoint to notify it. MIR emits no DWARF, so the objects libslimcc builds
// for us carry only a .symtab (function-granularity symbolization: named
// frames, `break funcname`, labeled disas) — not line tables.
enum {
	JIT_NOACTION = 0,
	JIT_REGISTER_FN,
	JIT_UNREGISTER_FN
};

struct jit_code_entry {
	struct jit_code_entry*	next_entry;
	struct jit_code_entry*	prev_entry;
	const char*				symfile_addr;
	uint64_t				symfile_size;
};

struct jit_descriptor {
	uint32_t				version;
	uint32_t				action_flag;	// a JIT_* action for the debugger
	struct jit_code_entry*	relevant_entry;
	struct jit_code_entry*	first_entry;
};

// Each cdef owns its own MIR_context_t: it holds the loaded modules and the
// machine code MIR_gen produced for them. MIR has no per-module unload, so the
// whole context is the unit of teardown — MIR_finish() frees all the code at
// once when the intrep dies (the analog of tcc_delete()). Function/data
// pointers handed out via the symbols dict stay valid for the context's life.
//
// When the cdef requests debug (a `debug` part or -g in options) the JIT'd
// functions are registered with any attached debugger via the GDB JIT
// interface above; jit_symbols holds this cdef's descriptor-list entry (and
// owns the malloc'd ELF symbol object until teardown unregisters it).
struct jitc_intrep {
	Tcl_Obj*				symbols;			// dict: symbol name -> code/data address (Tcl_WideInt)
	Tcl_Obj*				cdef;
	Tcl_Interp*				interp;
	Tcl_Obj*				exported_symbols;
	Tcl_Obj*				exported_headers;
	Tcl_Obj*				used;				// Hold references to the foreign cdefs to prevent them from being freed under us
	MIR_context_t			ctx;				// Owns the loaded modules + generated code; MIR_finish() frees it
	int						gen_inited;			// MIR_gen_init() has run on ctx (so teardown must MIR_gen_finish())
	int						opt_level;			// MIR codegen optimization level (-O<n>); -1 = use jitc's default (O1)
	int						debug;				// cdef requested debug symbols (a `debug` part or -g in options)
	struct jit_code_entry	jit_symbols;		// this cdef's GDB JIT-interface entry (debug only)
	Tcl_Obj*				debugdir;			// temp dir holding per-block source files for gdb (debug only)
	Tcl_Obj*				debugfiles;			// list of those source file paths (unlinked on teardown)
	void**					dlhandles;			// Libraries dlopen()'d so this cdef's code can resolve their symbols
	int						n_dlhandles;
};

// Look up a defined symbol's address in a compiled cdef. Returns NULL if the
// name isn't present (mirrors tcc_get_symbol()).
void* jitc_get_symbol(struct jitc_intrep* r, const char* name);

struct jitc_instance {
	struct jitc_instance*	next;
	struct jitc_instance*	prev;
	Tcl_Obj*				obj;
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
	LIT_INITSTUBS,
	LIT_INIT,
	LIT_RELEASE,
#if STUBSMODE
	LIT_TCLSTUBLIB_CMD,
#else
	LIT_TCLLIB_CMD,
#endif
	LIT_TCLVER_CMD,
	LIT_SIZE
};
extern const char*	lit_str[];

struct interp_cx {
	Tcl_Obj*				lit[LIT_SIZE];
#if STUBSMODE
	Tcl_Obj*				tclstublib;
#else
	Tcl_Obj*				tcllib;
#endif
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

