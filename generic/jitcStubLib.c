#undef USE_TCL_STUBS
#undef USE_JITC_STUBS
#define USE_TCL_STUBS 1
#define USE_JITC_STUBS 1

#include "jitc.h"

MODULE_SCOPE const JitcStubs*	jitcStubsPtr;
const JitcStubs*					jitcStubsPtr = NULL;

#undef JitcInitializeStubs
MODULE_SCOPE const char* JitcInitializeStubs(Tcl_Interp* interp)
{
	const char*	got = NULL;
	got = Tcl_PkgRequireEx(interp, PACKAGE_NAME, PACKAGE_VERSION, 0, &jitcStubsPtr);
	return got;
}
