library jitc
interface jitc

declare 0 generic {
	int Jitc_GetSymbolFromObj(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Obj* symbol, void** val)
}
declare 1 generic {
	int Jitc_GetSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Obj** symbols)
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>>
