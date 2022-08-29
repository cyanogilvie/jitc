#include <stddef.h>
#include <string.h>
#include "tclstuff.h"
#include <libtcc.h>
#include "tinycc.h"

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

extern Tcl_Mutex g_tcc_mutex;

struct tinycc_intrep {
	TCCState*				s;
	Tcl_Obj*				symbols;
	Tcl_Obj*				cdef;
	Tcl_Obj*				debugfiles;
	Tcl_Interp*				interp;
};

int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct tinycc_intrep** rPtr);
