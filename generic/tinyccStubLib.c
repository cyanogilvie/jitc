#undef USE_TCL_STUBS
#undef USE_TINYCC_STUBS
#define USE_TCL_STUBS 1
#define USE_TINYCC_STUBS 1

#include "tinycc.h"

MODULE_SCOPE const TinyccStubs*	tinyccStubsPtr;
const TinyccStubs*					tinyccStubsPtr = NULL;

#undef TinyccInitializeStubs
MODULE_SCOPE const char* TinyccInitializeStubs(Tcl_Interp* interp)
{
	const char*	got = NULL;
	got = Tcl_PkgRequireEx(interp, PACKAGE_NAME, PACKAGE_VERSION, 0, &tinyccStubsPtr);
	return got;
}
