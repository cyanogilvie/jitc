library tinycc
interface tinycc

declare 0 generic {
	int Tinycc_GetSymbolFromObj(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Obj* symbol, void** val)
}
declare 1 generic {
	int Tinycc_GetSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Obj** symbols)
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>>
