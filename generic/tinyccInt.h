#include <stddef.h>
#include "tclstuff.h"
#include <libtcc.h>
#include "tinycc.h"

extern Tcl_Mutex g_tcc_mutex;

struct tinycc_intrep {
	//const unsigned char*	objcode;
	TCCState*				s;
	const char*				packed_symbols;
	const char*				symbols[];
	void*					values[];
	Tcl_Obj*				cdef;
	Tcl_Obj*				debugfiles;
};

int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct tinycc_intrep** rPtr);
