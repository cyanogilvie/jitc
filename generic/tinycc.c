#include "tinyccInt.h"
#include "tip445.h"
#include <sys/stat.h>

Tcl_Mutex g_tcc_mutex = NULL;

typedef int (cdef_init)(Tcl_Interp* interp);
typedef int (cdef_release)(Tcl_Interp* interp);

static void free_tinycc_internal_rep(Tcl_Obj* obj);
static void dup_tinycc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup);

Tcl_ObjType tinycc_objtype = {
	"Tinycc",
	free_tinycc_internal_rep,
	dup_tinycc_internal_rep,
	NULL,
	NULL
};

static void free_tinycc_internal_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjIntRep*			ir = Tcl_FetchIntRep(obj, &tinycc_objtype);
	struct tinycc_intrep*	r = ir->twoPtrValue.ptr1;

	replace_tclobj(&r->symbols, NULL);
	if (r->s) {
		Tcl_MutexLock(&g_tcc_mutex);
		cdef_release*	release = tcc_get_symbol(r->s, "release");
		if (release) (release)(r->interp);
		tcc_delete(r->s);
		r->s = NULL;
		Tcl_MutexUnlock(&g_tcc_mutex);
	}
	r->interp = NULL;
	replace_tclobj(&r->cdef, NULL);
	if (r->debugfiles) {
		Tcl_Obj**	fv;
		int			fc;

		if (TCL_OK == Tcl_ListObjGetElements(NULL, r->debugfiles, &fc, &fv)) {
			for (int i=0; i<fc; i++) {
				if (TCL_OK != Tcl_FSDeleteFile(fv[i])) {
					// TODO: what?
				}
			}
		}
	}
	replace_tclobj(&r->debugfiles, NULL);
	replace_tclobj((Tcl_Obj**)&ir->twoPtrValue.ptr2, NULL);

	ckfree(r);
	r = NULL;
}

//}}}
static void dup_tinycc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup) //{{{
{
	Tcl_ObjIntRep*			ir = Tcl_FetchIntRep(src, &tinycc_objtype);
	struct tinycc_intrep*	r = ir->twoPtrValue.ptr1;
	Tcl_ObjIntRep			newir = {0};

	// Shouldn't ever need to happen, but if it does we have to recompile from source.
	// Set the dup's intrep to a dup of the cdef list instead
	replace_tclobj((Tcl_Obj**)&newir.twoPtrValue.ptr2, r->cdef);

	Tcl_StoreIntRep(dup, &tinycc_objtype, &newir);
}

//}}}

// Internal API {{{
static void errfunc(void* cdata, const char* msg) //{{{
{
	Tcl_Obj**	compile_errors = (Tcl_Obj**)cdata;
	if (*compile_errors == NULL) {
		replace_tclobj(compile_errors, Tcl_NewStringObj(msg, -1));
	} else {
		Tcl_AppendStringsToObj(*compile_errors, "\n", msg, NULL);
	}
}

//}}}
static void list_symbols_dict(void* ctx, const char* name, const void* val) //{{{
{
	Tcl_Obj*	symbolsdict = ctx;
	Tcl_Obj*	nameobj = NULL;
	Tcl_Obj*	valobj = NULL;
	Tcl_WideInt	w = (Tcl_WideInt)val;
	int			failed = 1;

	replace_tclobj(&nameobj, Tcl_NewStringObj(name, -1));
	replace_tclobj(&valobj,  Tcl_NewWideIntObj(w));
	if (TCL_OK != Tcl_DictObjPut(NULL, symbolsdict, nameobj, valobj)) goto finally;
	failed = 0;

finally:
	replace_tclobj(&nameobj, NULL);
	replace_tclobj(&valobj,  NULL);

	if (failed)
		Tcl_Panic("Failed to append symbol to list: %s: %p", name, val);
}

//}}}
int compile(Tcl_Interp* interp, Tcl_Obj* cdef, struct tinycc_intrep** rPtr) //{{{
{
	int						code = TCL_OK;
	static const char* parts[] = {
		"code",
		"file",
		"debug",
		"options",
		"include_path",
		"sysinclude_path",
		"symbol",
		"library_path",
		"library",
		"tccdir",
		"define",
		"undefine",
		NULL
	};
	enum {
		PART_CODE,
		PART_FILE,
		PART_DEBUG,
		PART_OPTIONS,
		PART_INCLUDE_PATH,
		PART_SYSINCLUDE_PATH,
		PART_SYMBOL,
		PART_LIBRARY_PATH,
		PART_LIBRARY,
		PART_TCCDIR,
		PART_DEFINE,
		PART_UNDEFINE
	};
	Tcl_Obj**				ov;
	int						oc;
	int						i;
	Tcl_Obj*				debugpath = NULL;
	struct TCCState*		tcc = NULL;
	int						mutexheld = 0;
	Tcl_Obj*				debugfiles = NULL;
	Tcl_Obj*				debugfile = NULL;
	Tcl_StatBuf*			stat = NULL;
	int						codeseq = 1;
	Tcl_Obj*				pathelements = NULL;
	Tcl_Channel				chan = NULL;
	Tcl_Obj*				compile_errors = NULL;
	struct tinycc_intrep*	r = NULL;

	TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, cdef, &oc, &ov));
	if (oc % 2 == 1)
		THROW_ERROR_LABEL(finally, code, "cdef must be a list with an even number of elements");

	// First pass through the parts to check for a debugpath setting
	for (i=0; i<oc; i+=2) {
		int			part;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &part));
		if (part == PART_DEBUG)
			replace_tclobj(&debugpath, ov[i+1]);
	}

	if (debugpath) {
		stat = Tcl_AllocStatBuf();
		if (
				-1 == Tcl_FSStat(debugpath, stat) ||	// Doesn't exist
				Tcl_GetModeFromStat(stat) != S_IFDIR	// Not a directory
		) THROW_ERROR_LABEL(finally, code, "debugpath \"", Tcl_GetString(debugpath), "\" doesn't exist");
	}

	Tcl_MutexLock(&g_tcc_mutex); mutexheld = 1;

	tcc = tcc_new();
	tcc_set_error_func(tcc, &compile_errors, errfunc);
	tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);

	replace_tclobj(&debugfiles, Tcl_NewListObj(0, NULL));
	for (i=0; i<oc; i+=2) {
		int			part;
		Tcl_Obj*	v = ov[i+1];

		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &part));
		switch (part) {
			case PART_CODE:
				if (debugpath) { // Write out to a temporary file instead, and try to arrange for it for be unlinked when intrep is freed {{{
					replace_tclobj(&pathelements, Tcl_NewListObj(2, (Tcl_Obj*[]){
						debugpath,
						Tcl_ObjPrintf("%p_%d", tcc, codeseq++)	// TODO: use name(tcc) for a friendly name instead?
					}));
					replace_tclobj(&debugfile, Tcl_FSJoinPath(pathelements, 2));
					TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, debugfiles, debugfile));
					chan = Tcl_FSOpenFileChannel(interp, debugfile, "w", 0400);
					if (chan == NULL) {
						code = TCL_ERROR;
						goto finally;
					}
					TEST_OK_LABEL(finally, code, Tcl_WriteObj(chan, v));
					TEST_OK_LABEL(finally, code, Tcl_Close(interp, chan));
					chan = NULL;

					if (-1 == tcc_add_file(tcc, Tcl_GetString(debugfile)))
						THROW_ERROR_LABEL(finally, code, "Error compiling file", Tcl_GetString(debugfile));
					//}}}
				} else {
					if (-1 == tcc_compile_string(tcc, Tcl_GetString(v)))
						THROW_ERROR_LABEL(finally, code, "Error compiling code", Tcl_GetString(v));
				}
				break;

			case PART_FILE:
				if (-1 == tcc_add_file(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error compiling file", Tcl_GetString(v));
				break;

			case PART_DEBUG: /* Handled above */ break;

			case PART_OPTIONS:			tcc_set_options        (tcc, Tcl_GetString(v)); break;
			case PART_INCLUDE_PATH:		tcc_add_include_path   (tcc, Tcl_GetString(v)); break;
			case PART_SYSINCLUDE_PATH:	tcc_add_sysinclude_path(tcc, Tcl_GetString(v)); break;
			case PART_TCCDIR:			tcc_set_lib_path       (tcc, Tcl_GetString(v)); break;
			case PART_UNDEFINE:			tcc_undefine_symbol    (tcc, Tcl_GetString(v)); break;

			case PART_SYMBOL:
				{
					// treat this as another code object+symbol name to retrieve
					Tcl_Obj**	sv;
					int			sc;
					void*		val = NULL;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc != 2)
						THROW_ERROR_LABEL(finally, code, "Symbol definition must be a list: cdef symbol", Tcl_GetString(v));
					TEST_OK_LABEL(finally, code, Tinycc_GetSymbolFromObj(interp, sv[0], sv[1], &val));

					tcc_define_symbol(tcc, Tcl_GetString(sv[1]), val);
				}
				break;

			case PART_LIBRARY_PATH:
				if (-1 == tcc_add_library_path(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error adding library path", Tcl_GetString(v));
				break;

			case PART_LIBRARY:
				if (-1 == tcc_add_library(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error adding library", Tcl_GetString(v));
				break;

			case PART_DEFINE:
				{
					Tcl_Obj**	sv;
					int			sc;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc != 2)
						THROW_ERROR_LABEL(finally, code, "Definition must be a list: name value", Tcl_GetString(v));
					tcc_define_symbol(tcc, Tcl_GetString(sv[0]), Tcl_GetString(sv[1]));
				}
				break;

			default:
				THROW_ERROR_LABEL(finally, code, "Invalid part id");
		}
	}

	if (compile_errors) {
		if (code == TCL_OK)
			THROW_ERROR_LABEL(finally, code, Tcl_GetString(compile_errors));

		// Already have error info, add to it
		code = TCL_ERROR;
		Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s\nCompile errors:\n%s", Tcl_GetString(Tcl_GetObjResult(interp)), Tcl_GetString(compile_errors)));
		replace_tclobj(&compile_errors, NULL);
		goto finally;
	}

	r = ckalloc(sizeof *r);
	*r = (struct tinycc_intrep){0};

	r->s = tcc;
	r->interp = interp;

	tcc_relocate(tcc, TCC_RELOCATE_AUTO);

	replace_tclobj(&r->symbols, Tcl_NewDictObj());
	tcc_list_symbols(tcc, r->symbols, list_symbols_dict);

	// Avoid a circular reference between cdef and our new tinycc intrep obj
	replace_tclobj(&r->cdef, Tcl_DuplicateObj(cdef));

	cdef_init*	init = tcc_get_symbol(tcc, "init");
	if (init)
		TEST_OK_LABEL(finally, code, (init)(interp));

	replace_tclobj(&r->debugfiles, debugfiles);
	replace_tclobj(&debugfiles, NULL);

	*rPtr = r;
	r = NULL;
	tcc = NULL;

finally:
	if (compile_errors) {
		if (code == TCL_OK) {
			Tcl_SetObjResult(interp, compile_errors);
		} else {
			// Already have error info, add to it
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s\nCompile errors:\n%s", Tcl_GetString(Tcl_GetObjResult(interp)), Tcl_GetString(compile_errors)));
		}
		code = TCL_ERROR;
	}

	if (chan) {
		Tcl_Close(interp, chan);
		chan = NULL;
	}

	if (debugfiles) {
		Tcl_Obj**	fv;
		int			fc;
		if (TCL_OK == Tcl_ListObjGetElements(NULL, debugfiles, &fc, &fv)) {
			for (int i=0; i<fc; i++) {
				if (TCL_OK != Tcl_FSDeleteFile(fv[i]) && code != TCL_ERROR) {
					// TODO: elaborate on the error via errno?
					Tcl_SetObjResult(interp, Tcl_ObjPrintf("Error deleting debug file: \"%s\"", Tcl_GetString(fv[i])));
					code = TCL_ERROR;
				}
			}
		}
	}
	replace_tclobj(&debugfiles,		NULL);
	replace_tclobj(&debugpath,		NULL);
	replace_tclobj(&debugfile,		NULL);
	replace_tclobj(&pathelements,	NULL);
	replace_tclobj(&compile_errors,	NULL);

	if (stat) {
		ckfree(stat);
		stat = NULL;
	}

	if (r) {
		replace_tclobj(&r->symbols, NULL);
		replace_tclobj(&r->cdef, NULL);
		replace_tclobj(&r->debugfiles, NULL);
		r->interp = NULL;
		ckfree(r);
		r = NULL;
	}

	if (tcc) {
		tcc_delete(tcc);
		tcc = NULL;
	}

	if (mutexheld) {
		mutexheld = 0;
		Tcl_MutexUnlock(&g_tcc_mutex);
	}

	return code;
}

//}}}
int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct tinycc_intrep** rPtr) //{{{
{
	int						code = TCL_OK;
	Tcl_ObjIntRep*			ir = Tcl_FetchIntRep(obj, &tinycc_objtype);
	struct tinycc_intrep*	r = NULL;

	if (ir == NULL) {
		Tcl_ObjIntRep	newir = {0};

		TEST_OK_LABEL(finally, code, compile(interp, obj, (struct tinycc_intrep **)&newir.twoPtrValue.ptr1));

		Tcl_FreeIntRep(obj);
		Tcl_StoreIntRep(obj, &tinycc_objtype, &newir);
		ir = Tcl_FetchIntRep(obj, &tinycc_objtype);
	}

	r = ir->twoPtrValue.ptr1;
	if (r == NULL) {
		// Duplicated intrep, recompile from the cdef copy
		TEST_OK_LABEL(finally, code, compile(interp, (Tcl_Obj*)ir->twoPtrValue.ptr2, &r));
		replace_tclobj((Tcl_Obj**)&ir->twoPtrValue.ptr2, NULL);
	}

	*rPtr = r;

finally:
	return code;
}

//}}}
// Internal API }}}
// Stubs API {{{
int Tinycc_GetSymbolFromObj(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Obj* symbol, void** val) //{{{
{
	int						code = TCL_OK;
	struct tinycc_intrep*	r = NULL;
	Tcl_Obj*				valobj = NULL;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, obj, &r));
	TEST_OK_LABEL(finally, code, Tcl_DictObjGet(interp, r->symbols, symbol, &valobj));
	if (valobj) {
		Tcl_WideInt	w;
		TEST_OK_LABEL(finally, code, Tcl_GetWideIntFromObj(interp, valobj, &w));
		*val = UINT2PTR(w);
	} else {
		THROW_ERROR_LABEL(finally, code, "Symbol not found");
	}


finally:
	return code;
}

//}}}
int Tinycc_GetSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* obj, Tcl_Obj** symbols) //{{{
{
	int						code = TCL_OK;
	struct tinycc_intrep*	r = NULL;
	Tcl_Obj*				lsymbols = NULL;
	Tcl_DictSearch			search;
	Tcl_Obj*				k = NULL;
	Tcl_Obj*				v = NULL;
	int						searching = 0;
	int						done;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, obj, &r));

	replace_tclobj(&lsymbols, Tcl_NewListObj(0, NULL));
	TEST_OK_LABEL(finally, code, Tcl_DictObjFirst(interp, r->symbols, &search, &k, &v, &done));
	searching = 1;
	while (!done) {
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, lsymbols, k));
		Tcl_DictObjNext(&search, &k, &v, &done);
	}

	replace_tclobj(symbols, lsymbols);

finally:
	replace_tclobj(&lsymbols, NULL);
	if (searching) Tcl_DictObjDone(&search);

	return code;
}

//}}}
// Stubs API }}}
// Script API {{{
static int capply_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	int				code = TCL_OK;
	Tcl_ObjCmdProc*	proc = NULL;

	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 1, objv, "cdef symbol args");
		code = TCL_ERROR;
		goto finally;
	}

	TEST_OK_LABEL(finally, code, Tinycc_GetSymbolFromObj(interp, objv[1], objv[2], (void**)&proc));

	TEST_OK_LABEL(finally, code, (proc)(NULL, interp, objc-2, objv+2));

finally:
	return code;
}

//}}}
static int symbols_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	int				code = TCL_OK;
	Tcl_Obj*		symbols = NULL;

	CHECK_ARGS(1, "cdef");

	TEST_OK_LABEL(finally, code, Tinycc_GetSymbolsFromObj(interp, objv[1], &symbols));
	Tcl_SetObjResult(interp, symbols);
	replace_tclobj(&symbols, NULL);

finally:
	return code;
}

//}}}

#define NS	"::tinycc"
static struct cmd {
	char*			name;
	Tcl_ObjCmdProc*	proc;
} cmds[] = {
	{NS "::capply",		capply_cmd},
	{NS "::symbols",	symbols_cmd},
	{NULL,				NULL}
};
// Script API }}}

extern const TinyccStubs* const tinyccConstStubsPtr;

#ifdef __cplusplus
extern "C" {
#endif
DLLEXPORT int Tinycc_Init(Tcl_Interp* interp)
{
	int				code = TCL_OK;
	Tcl_Namespace*	ns = NULL;
	struct cmd*		c = cmds;

#if USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL)
		return TCL_ERROR;
#endif

	ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	while (c->name) {
		if (NULL == Tcl_CreateObjCommand(interp, c->name, c->proc, NULL, NULL)) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("Could not create command %s", c->name));
			code = TCL_ERROR;
			goto finally;
		}
		c++;
	}

	TEST_OK_LABEL(finally, code, Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION, tinyccConstStubsPtr));

finally:
	return code;
}

#ifdef __cplusplus
}
#endif
