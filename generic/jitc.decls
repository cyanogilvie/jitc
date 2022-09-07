library jitc
interface jitc

declare 0 generic {
	int Jitc_GetSymbolFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj* symbol, void** val)
}
declare 1 generic {
	int Jitc_GetSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** symbols)
}
declare 2 generic {
	int Jitc_GetExportHeadersFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** headers)
}
declare 3 generic {
	int Jitc_GetExportSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** symbols)
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>>
