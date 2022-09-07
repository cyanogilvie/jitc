namespace eval ::jitclib {
	variable tcltime {
		export {
			symbols now
			header { //@begin=c@
				#include <stdint.h>
				uint64_t now();		// Return the current (Tcl_Time) in microseconds since 1970-01-01 00:00:00 UTC
				//@end=c@
			}
		}
		code {//@begin=c@
			uint64_t now()
			{
				Tcl_Time	t;	
				Tcl_GetTime(&t);

				return t.sec * 100000 + t.usec;
			}
			//@end=c@
		}
	}
}
# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
