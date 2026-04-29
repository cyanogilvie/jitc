namespace eval ::jitclib {
	variable load_cdef	[list options {-Wall -Werror} code {//@begin=c@<<<
		#include <stdio.h>

		typedef int (cdef_init)(Tcl_Interp* interp);
		typedef int (cdef_release)(Tcl_Interp* interp);

		OBJCMD(capply) {
			/*
			fprintf(stderr, "In load_cdef capply, args:\n");
			for (int i=0; i<objc; i++) fprintf(stderr, "\t(%s)\n", Tcl_GetString(objv[i]));
			*/
			int				code = TCL_OK;
			enum {A_cmd, A_FILE, A_SYM, A_args};
			CHECK_MIN_ARGS("file symbol ?arg ...?");

			const char*const symbols[2] = {
				Tcl_GetString(objv[A_SYM]),
				NULL
			};
			Tcl_ObjCmdProc*	procs[1];
			cdef_init*		init = NULL;
			cdef_release*	release = NULL;
			Tcl_LoadHandle	handle = NULL;

			fprintf(stderr, "Loading %s, looking for symbol: %s\n", Tcl_GetString(objv[A_FILE]), Tcl_GetString(objv[A_SYM]));
			TEST_OK_LABEL(done, code, Tcl_LoadFile(interp, objv[1],
				symbols,
				0,
				procs,
				&handle));
			init = Tcl_FindSymbol(NULL, handle, "init");
			release = Tcl_FindSymbol(NULL, handle, "init");

			fprintf(stderr, "Loaded %s, init: %p, release: %p, %s: %p\n", Tcl_GetString(objv[A_FILE]), init, release, Tcl_GetString(objv[A_SYM]), procs[0]);

			fprintf(stderr, "Calling init (if set)\n");
			if (init)
				TEST_OK_LABEL(done, code, (init)(interp));

			fprintf(stderr, "Calling %s\n", Tcl_GetString(objv[A_SYM]));
			TEST_OK_LABEL(done, code, (procs[0])(NULL, interp, objc-2, objv+2));

		done:
			fprintf(stderr, "Calling release (if set)\n");
			if (release)
				TEST_OK_LABEL(done, code, (release)(interp));

			if (handle) {
				fprintf(stderr, "Unloading %s\n", Tcl_GetString(objv[A_FILE]));
				Tcl_FSUnloadFile(interp, handle);
				handle = NULL;
			}

			return code;
		}
	//@end=c@}]
}
