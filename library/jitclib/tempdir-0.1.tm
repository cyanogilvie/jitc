namespace eval ::jitclib {
	variable tempdir {options {-Wall -Werror -gdwarf-5} code {//@begin=c@
		#include <stdio.h>
		#include <stdlib.h>

		OBJCMD(tempdir) {
			int			code = TCL_OK;
			Tcl_DString	template;

			CHECK_ARGS(1, "template");

			int preflen;
			const char*	pref = Tcl_GetStringFromObj(objv[1], &preflen);

			Tcl_DStringInit(&template);

			if (preflen && pref[0] != '/') {
				Tcl_DStringAppend(&template, P_tmpdir, -1);
				Tcl_DStringAppend(&template, "/", 1);
			}
			//@end=c@@begin=c@
			Tcl_DStringAppend(&template, pref, preflen);
			Tcl_DStringAppend(&template, "XXXXXX", 6);

			const char* tmpdir = mkdtemp(Tcl_DStringValue(&template));
			if (tmpdir == NULL)
				THROW_POSIX_LABEL(done, code, "Error creating temporary directory");

			fprintf(stderr, "Created tempdir: (%s)\n", tmpdir);
			Tcl_SetObjResult(interp, Tcl_NewStringObj(tmpdir, -1));

		done:
			Tcl_DStringFree(&template);
			return code;
		}
	//@end=c@}}
}
