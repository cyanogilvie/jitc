#include "jitcInt.h"
#include "tip445.h"
#include <sys/stat.h>

Tcl_Mutex g_tcc_mutex = NULL;

typedef int (cdef_init)(Tcl_Interp* interp);
typedef int (cdef_release)(Tcl_Interp* interp);

static void free_jitc_internal_rep(Tcl_Obj* obj);
static void dup_jitc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup);
static void update_jitc_string_rep(Tcl_Obj* obj);

Tcl_ObjType jitc_objtype = {
	"Jitc",
	free_jitc_internal_rep,
	dup_jitc_internal_rep,
	update_jitc_string_rep,
	NULL
};

static void free_jitc_internal_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjIntRep*		ir = Tcl_FetchIntRep(obj, &jitc_objtype);
	struct jitc_intrep*	r = ir->twoPtrValue.ptr1;

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
	replace_tclobj(&r->exported_symbols, NULL);
	replace_tclobj(&r->exported_headers, NULL);

	ckfree(r);
	r = NULL;
}

//}}}
static void dup_jitc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup) //{{{
{
	Tcl_ObjIntRep*		ir = Tcl_FetchIntRep(src, &jitc_objtype);
	struct jitc_intrep*	r = ir->twoPtrValue.ptr1;
	Tcl_ObjIntRep		newir = {0};

	// Shouldn't ever need to happen, but if it does we have to recompile from source.
	// Set the dup's intrep to a dup of the cdef list instead
	replace_tclobj((Tcl_Obj**)&newir.twoPtrValue.ptr2, r->cdef);

	Tcl_StoreIntRep(dup, &jitc_objtype, &newir);
}

//}}}
void update_jitc_string_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjIntRep*		ir = Tcl_FetchIntRep(obj, &jitc_objtype);
	struct jitc_intrep*	r = ir->twoPtrValue.ptr1;
	int					newstring_len;
	const char*			newstring = Tcl_GetStringFromObj(r->cdef, &newstring_len);

	Tcl_InvalidateStringRep(obj);	// Just in case, panic below if obj->bytes != NULL
	Tcl_InitStringRep(obj, newstring, newstring_len);
}
//}}}

// Internal API {{{
const char* lit_str[] = {
	"",
	"include",
	"generic",
	"lib",
	"::jitc::tccpath",
	"::jitc::includepath",
	"::jitc::librarypath",
	"::jitc::packagedir",
	"::jitc::prefix",
	"::jitc::_build_compile_error",
	NULL
};

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
int compile(Tcl_Interp* interp, Tcl_Obj* cdef, struct jitc_intrep** rPtr) //{{{
{
	int					code = TCL_OK;
	struct interp_cx*	l = Tcl_GetAssocData(interp, "jitc", NULL);
	static const char* parts[] = {
		"mode",
		"code",
		"file",
		"debug",
		"options",
		"include_path",
		"sysinclude_path",
		"symbols",
		"library_path",
		"library",
		"tccpath",
		"define",
		"undefine",
		"package",
		"filter",
		"export",
		"use",
		NULL
	};
	enum partenum {
		PART_MODE,
		PART_CODE,
		PART_FILE,
		PART_DEBUG,
		PART_OPTIONS,
		PART_INCLUDE_PATH,
		PART_SYSINCLUDE_PATH,
		PART_SYMBOLS,
		PART_LIBRARY_PATH,
		PART_LIBRARY,
		PART_TCCPATH,
		PART_DEFINE,
		PART_UNDEFINE,
		PART_PACKAGE,
		PART_FILTER,
		PART_EXPORT,
		PART_USE
	};
	static const char* modes[] = {
		"tcl",
		"raw",
		NULL
	};
	enum {
		MODE_TCL,
		MODE_RAW
	};
	int					mode = MODE_TCL;
	Tcl_Obj**			ov;
	int					oc;
	int					i;
	Tcl_Obj*			debugpath = NULL;
	struct TCCState*	tcc = NULL;
	int					mutexheld = 0;
	Tcl_Obj*			debugfiles = NULL;
	Tcl_Obj*			debugfile = NULL;
	Tcl_StatBuf*		stat = NULL;
	int					codeseq = 1;
	Tcl_Obj*			pathelements = NULL;
	Tcl_Channel			chan = NULL;
	Tcl_Obj*			compile_errors = NULL;
	struct jitc_intrep*	r = NULL;
	Tcl_DString			preamble;
	Tcl_Obj*			filter = NULL;
	Tcl_Obj*			exported_headers = NULL;
	Tcl_Obj*			exported_symbols = NULL;
	Tcl_Obj*			compileerror_code = NULL;

	Tcl_DStringInit(&preamble);

	TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, cdef, &oc, &ov));
	if (oc % 2 == 1)
		THROW_ERROR_LABEL(finally, code, "cdef must be a list with an even number of elements");

	// First pass through the parts to check for a debugpath setting
	for (i=0; i<oc; i+=2) {
		enum partenum	part;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &part));
		_Pragma("GCC diagnostic push")
		_Pragma("GCC diagnostic ignored \"-Wswitch\"")
		switch (part) {
			case PART_DEBUG:
				replace_tclobj(&debugpath, ov[i+1]);
				break;
			case PART_MODE:
				TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i+1], modes, "mode", TCL_EXACT, &mode));
				break;

			// Have to pre-flight these to ensure that the compilation (if required) happens before we lock the Mutex and create the TCCState below
			case PART_SYMBOLS:
				{
					Tcl_Obj**	sv;
					int			sc;
					struct jitc_intrep*	r;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, ov[i+1], &sc, &sv));
					if (sc >= 1) TEST_OK_LABEL(finally, code, get_r_from_obj(interp, sv[0], &r));
				}
			case PART_USE:
				TEST_OK_LABEL(finally, code, get_r_from_obj(interp, ov[i+1], &r));
				break;
		}
		_Pragma("GCC diagnostic pop")
	}

	if (debugpath) {
		stat = Tcl_AllocStatBuf();
		if (
				-1 == Tcl_FSStat(debugpath, stat) ||	// Doesn't exist
				!(S_ISDIR(Tcl_GetModeFromStat(stat)))	// Not a directory
		) THROW_ERROR_LABEL(finally, code, "debug path \"", Tcl_GetString(debugpath), "\" doesn't exist");
	}

	Tcl_MutexLock(&g_tcc_mutex); mutexheld = 1;

	tcc = tcc_new();
	tcc_set_error_func(tcc, &compile_errors, errfunc);

	// Set some mode-dependent defaults
	switch (mode) {
		case MODE_TCL:
			{
				Tcl_Obj**			ov;
				int					oc;
				Tcl_Obj*			includepath = NULL;
				Tcl_Obj*			librarypath = NULL;
				Tcl_Obj*			tccpath = NULL;

				replace_tclobj(&tccpath,     Tcl_ObjGetVar2(interp, l->lit[LIT_TCC_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (tccpath == NULL) {code = TCL_ERROR; goto modetclfinally;}
				replace_tclobj(&includepath, Tcl_ObjGetVar2(interp, l->lit[LIT_INCLUDEPATH_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (includepath == NULL) {code = TCL_ERROR; goto modetclfinally;}
				replace_tclobj(&librarypath, Tcl_ObjGetVar2(interp, l->lit[LIT_LIBRARYPATH_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (librarypath == NULL) {code = TCL_ERROR; goto modetclfinally;}

				tcc_set_lib_path(tcc, Tcl_GetString(tccpath));

				TEST_OK_LABEL(modetclfinally, code, Tcl_ListObjGetElements(interp, includepath, &oc, &ov));
				for (int i=0; i<oc; i++) tcc_add_include_path(tcc, Tcl_GetString(ov[i]));
				TEST_OK_LABEL(modetclfinally, code, Tcl_ListObjGetElements(interp, librarypath, &oc, &ov));
				for (int i=0; i<oc; i++)
					if (-1 == tcc_add_library_path(tcc, Tcl_GetString(ov[i])))
						THROW_PRINTF_LABEL(modetclfinally, code, "Error adding library path \"%s\"", Tcl_GetString(ov[i]));

				//tcc_add_library(tcc, Tcl_GetString(l->tcllib));	// Tcl symbols are reverse exported, this doesn't seem to be necessary

			modetclfinally:
				replace_tclobj(&includepath, NULL);
				replace_tclobj(&librarypath, NULL);
				replace_tclobj(&tccpath, NULL);
				if (code != TCL_OK) goto finally;
				Tcl_DStringAppend(&preamble, "#include <tclstuff.h>\n", -1);
			}
			break;

		case MODE_RAW:
			{
				Tcl_Obj*	tccpath = NULL;
				Tcl_Obj*	tccinclude = NULL;

				replace_tclobj(&tccpath, Tcl_ObjGetVar2(interp, l->lit[LIT_TCC_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (tccpath == NULL) {code = TCL_ERROR; goto moderawfinally;}
				replace_tclobj(&tccinclude, Tcl_FSJoinToPath(tccpath, 1, (Tcl_Obj*[]){
					l->lit[LIT_INCLUDE]
				}));
				//fprintf(stderr, "mode raw, setting tcc dir to %s, adding lib path %s and include path %s\n", Tcl_GetString(tccpath), Tcl_GetString(tccpath), Tcl_GetString(tccinclude));
				tcc_set_lib_path(tcc, Tcl_GetString(tccpath));
				tcc_add_include_path(tcc, Tcl_GetString(tccinclude));
				if (-1 == tcc_add_library_path(tcc, Tcl_GetString(tccpath)))
					THROW_PRINTF_LABEL(moderawfinally, code, "Error adding library path \"%s\"", Tcl_GetString(tccpath));

			moderawfinally:
				replace_tclobj(&tccpath, NULL);
				replace_tclobj(&tccinclude, NULL);
				if (code != TCL_OK) goto finally;
			}
			break;

		default:
			THROW_ERROR_LABEL(finally, code, "Unhandled mode");
	}

	// Second pass through the parts to process PART_PACKAGE and PART_USE directives
	for (i=0; i<oc; i+=2) {
		enum partenum		part;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &part));
		_Pragma("GCC diagnostic push")
		_Pragma("GCC diagnostic ignored \"-Wswitch\"")
		switch (part) {
			case PART_OPTIONS: tcc_set_options(tcc, Tcl_GetString(ov[i+1])); break;	// Must be set before tcc_set_output_type
			case PART_PACKAGE: //{{{
				{
					int			pc;
					Tcl_Obj**	pv = NULL;
					Tcl_Obj*	cmd[3] = {0};

					// TODO: Cache these lookups
					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, ov[i+1], &pc, &pv));
					if (pc < 1) THROW_ERROR_LABEL(finally, code, "At least package name is required");
					TEST_OK_LABEL(finally, code, Tcl_PkgRequireProc(interp, Tcl_GetString(pv[0]), pc-1, pv+1, NULL));
					replace_tclobj(&cmd[0], Tcl_ObjPrintf("%s::pkgconfig", Tcl_GetString(pv[0])));
					replace_tclobj(&cmd[1], Tcl_NewStringObj("get", 3));
					const char* keys[] = {
						"header",
						"includedir,runtime",
						"includedir,install",
						"libdir,runtime",
						"libdir,install",
						"library",
						NULL
					};
					enum {
						KEY_HEADER,
						KEY_INCLUDEDIR_RUNTIME,
						KEY_INCLUDEDIR_INSTALL,
						KEY_LIBDIR_RUNTIME,
						KEY_LIBDIR_INSTALL,
						KEY_LIBRARY,
						KEY_END
					};
					Tcl_Obj*	vals[KEY_END] = {0};

					for (int i=0; keys[i]; i++) {
						Tcl_InterpState	state = Tcl_SaveInterpState(interp, 0);
						replace_tclobj(&cmd[2], Tcl_NewStringObj(keys[i], -1));
						code = Tcl_EvalObjv(interp, 3, cmd, TCL_EVAL_GLOBAL);
						if (code == TCL_OK)
							replace_tclobj(&vals[i], Tcl_GetObjResult(interp));
						Tcl_RestoreInterpState(interp, state);
						code = TCL_OK;
					}

					if (vals[KEY_HEADER]) {
						Tcl_DStringAppend(&preamble, "#include <", -1);
						Tcl_DStringAppend(&preamble, Tcl_GetString(vals[KEY_HEADER]), -1);
						Tcl_DStringAppend(&preamble, ">\n", -1);
					}

					if (vals[KEY_INCLUDEDIR_RUNTIME])
						tcc_add_include_path(tcc, Tcl_GetString(vals[KEY_INCLUDEDIR_RUNTIME]));
					if (vals[KEY_INCLUDEDIR_INSTALL])
						tcc_add_include_path(tcc, Tcl_GetString(vals[KEY_INCLUDEDIR_INSTALL]));
					if (vals[KEY_LIBDIR_RUNTIME])
						if (-1 == tcc_add_library_path(tcc, Tcl_GetString(vals[KEY_LIBDIR_RUNTIME]))) {
							// TODO: what?
						}
					if (vals[KEY_LIBDIR_INSTALL])
						if (-1 == tcc_add_library_path(tcc, Tcl_GetString(vals[KEY_LIBDIR_INSTALL]))) {
							// TODO: what?
						}
					if (vals[KEY_LIBRARY]) {
						const char* libstr = Tcl_GetString(vals[KEY_LIBRARY]);
						if (strncmp("lib", libstr, 3) == 0) {
							libstr += 3;
						}
						if (-1 == tcc_add_library(tcc, libstr))
							THROW_ERROR_LABEL(freevals, code, "Error adding library \"", Tcl_GetString(vals[KEY_LIBRARY]), "\"");
					}

				freevals:
					for (int i=0; i<3; i++)
						replace_tclobj(&cmd[i], NULL);
					for (int i=0; i<KEY_END; i++)
						replace_tclobj(&vals[i], NULL);

					if (code != TCL_OK) goto finally;
				}
				break;
				//}}}
			case PART_USE: //{{{
				{
					Tcl_Obj*	useobj = ov[i+1];
					Tcl_Obj*	use_headers = NULL;
					Tcl_Obj*	use_symbols = NULL;

					TEST_OK_LABEL(usedone, code, Jitc_GetExportHeadersFromObj(interp, useobj, &use_headers));
					TEST_OK_LABEL(usedone, code, Jitc_GetExportSymbolsFromObj(interp, useobj, &use_symbols));

					if (use_headers) {
						int			headerstrlen;
						const char*	headerstr = Tcl_GetStringFromObj(use_headers, &headerstrlen);
						Tcl_DStringAppend(&preamble, headerstr, headerstrlen);
					}
					if (use_symbols) {
						Tcl_Obj**		sv = NULL;
						int				sc;

						TEST_OK_LABEL(usedone, code, Tcl_ListObjGetElements(interp, use_symbols, &sc, &sv));
						for (int s=0; s<sc; s++) {
							void*	val = NULL;
							TEST_OK_LABEL(finally, code, Jitc_GetSymbolFromObj(interp, useobj, sv[s], &val));
							tcc_add_symbol(tcc, Tcl_GetString(sv[s]), val);
						}
					}

				usedone:
					replace_tclobj(&use_headers, NULL);
					replace_tclobj(&use_symbols, NULL);
					if (code != TCL_OK) goto finally;
				}
				break;
				//}}}
		}
		_Pragma("GCC diagnostic pop")
	}

	// Third pass through the parts to process PART_EXPORT directives	(export headers must be appended to preamble after use ones)
	for (i=0; i<oc; i+=2) {
		enum partenum		part;
		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &part));
		_Pragma("GCC diagnostic push")
		_Pragma("GCC diagnostic ignored \"-Wswitch\"")
		switch (part) {
			case PART_EXPORT: //{{{
				{
					Tcl_Obj**	ev = NULL;
					int			ec;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, ov[i+1], &ec, &ev));
					for (int ei=0; ei<ec; ei+=2) {
						static const char* exportkeys[] = {
							"symbols",
							"header",
							NULL
						};
						enum exportkeyenum {
							EXPORT_SYMBOLS,
							EXPORT_HEADER
						} exportkey;

						TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ev[ei], exportkeys, "key", TCL_EXACT, &exportkey));
						_Pragma("GCC diagnostic push")
						_Pragma("GCC diagnostic warning \"-Wswitch\"")
						switch (exportkey) {
							case EXPORT_SYMBOLS:
								replace_tclobj(&exported_symbols, ev[ei+1]);
								break;
							case EXPORT_HEADER:
								{
									int			headerstrlen;
									const char*	headerstr = Tcl_GetStringFromObj(ev[ei+1], &headerstrlen);

									replace_tclobj(&exported_headers, ev[ei+1]);
									Tcl_DStringAppend(&preamble, headerstr, headerstrlen);
								}
								break;
						}
						_Pragma("GCC diagnostic pop")
					}
				}
				break;
				//}}}
		}
		_Pragma("GCC diagnostic pop")
	}

	tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
	replace_tclobj(&debugfiles, Tcl_NewListObj(0, NULL));
	for (i=0; i<oc; i+=2) {
		enum partenum	part;
		Tcl_Obj*		v = ov[i+1];

		TEST_OK_LABEL(finally, code, Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &part));
		switch (part) {
			case PART_OPTIONS:
			case PART_MODE:
			case PART_DEBUG:
			case PART_PACKAGE:
			case PART_USE:
				/* Handled above */
				break;

			case PART_CODE: //{{{
				{
					Tcl_DString		c;
					int				len;
					const char*		str = Tcl_GetStringFromObj(v, &len);

					Tcl_DStringInit(&c);
					Tcl_DStringAppend(&c, Tcl_DStringValue(&preamble), Tcl_DStringLength(&preamble));
					Tcl_DStringAppend(&c, str, len);

					if (filter) {
						Tcl_Obj*	in = NULL;
						Tcl_Obj*	filtercmd = NULL;

						replace_tclobj(&filtercmd, Tcl_DuplicateObj(filter));
						replace_tclobj(&in, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
						TEST_OK_LABEL(filtererror, code, Tcl_ListObjAppendElement(interp, filtercmd, in));
						TEST_OK_LABEL(filtererror, code, Tcl_EvalObjEx(interp, filtercmd, 0));
						Tcl_DStringTrunc(&c, 0);
						int filtered_len;
						const char* filtered_str = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &filtered_len);
						Tcl_DStringAppend(&c, filtered_str, filtered_len);
						//fprintf(stderr, "// transformed code (with %s): %.*s", Tcl_GetString(filter), filtered_len, filtered_str);
					filtererror:
						replace_tclobj(&in, NULL);
						replace_tclobj(&filtercmd, NULL);
						if (code != TCL_OK) goto codeerror;
						Tcl_ResetResult(interp);
					}

					if (debugpath) { // Write out to a temporary file instead, and try to arrange for it for be unlinked when intrep is freed {{{
						replace_tclobj(&pathelements, Tcl_NewListObj(2, (Tcl_Obj*[]){
							debugpath,
							Tcl_ObjPrintf("%p_%d.c", tcc, codeseq++)	// TODO: use name(tcc) for a friendly name instead?
						}));
						replace_tclobj(&debugfile, Tcl_FSJoinPath(pathelements, 2));
						TEST_OK_LABEL(codeerror, code, Tcl_ListObjAppendElement(interp, debugfiles, debugfile));
						chan = Tcl_FSOpenFileChannel(interp, debugfile, "w", 0400);
						if (chan == NULL) {
							code = TCL_ERROR;
							goto finally;
						}
						const int c_len = Tcl_DStringLength(&c);
						const int wrote = Tcl_WriteChars(chan, Tcl_DStringValue(&c), c_len);
						if (wrote != c_len)
							THROW_PRINTF_LABEL(codeerror, code, "Tried to write %d characters to %s, only managed %d", c_len, Tcl_GetString(debugfile), wrote);
						TEST_OK_LABEL(codeerror, code, Tcl_Close(interp, chan));
						chan = NULL;

						if (-1 == tcc_add_file(tcc, Tcl_GetString(debugfile)))
							THROW_ERROR_LABEL(codeerror, code, "Error compiling file \"", Tcl_GetString(debugfile), "\"");
						//}}}
					} else {
						if (-1 == tcc_compile_string(tcc, Tcl_DStringValue(&c))) {
							replace_tclobj(&compileerror_code, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
							THROW_ERROR_LABEL(codeerror, code, "Error compiling code:\n", Tcl_DStringValue(&c));
						}
					}
					Tcl_DStringFree(&c);
					break;
				codeerror:
					Tcl_DStringFree(&c);
					goto finally;
				}
				break;
				//}}}

			case PART_FILE:
				if (-1 == tcc_add_file(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error compiling file \"", Tcl_GetString(v), "\"");
				break;

			case PART_INCLUDE_PATH:		tcc_add_include_path   (tcc, Tcl_GetString(v)); break;
			case PART_SYSINCLUDE_PATH:	tcc_add_sysinclude_path(tcc, Tcl_GetString(v)); break;
			case PART_TCCPATH:			tcc_set_lib_path       (tcc, Tcl_GetString(v)); break;
			case PART_UNDEFINE:			tcc_undefine_symbol    (tcc, Tcl_GetString(v)); break;

			case PART_SYMBOLS: //{{{
				{
					// treat this as another code object+symbols name to retrieve
					Tcl_Obj**	sv;
					int			sc;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc < 1)
						THROW_ERROR_LABEL(finally, code, "Symbol definition must be a list: cdef symbol: \"", Tcl_GetString(v), "\"");

					for (int i=1; i<sc; i++) {
						void*	val = NULL;
						TEST_OK_LABEL(finally, code, Jitc_GetSymbolFromObj(interp, sv[0], sv[i], &val));
						tcc_add_symbol(tcc, Tcl_GetString(sv[i]), val);
					}
				}
				break;
				//}}}

			case PART_EXPORT: break;

			case PART_LIBRARY_PATH:
				if (-1 == tcc_add_library_path(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error adding library path \"", Tcl_GetString(v), "\"");
				break;

			case PART_LIBRARY:
				if (-1 == tcc_add_library(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error adding library \"", Tcl_GetString(v), "\"");
				break;

			case PART_DEFINE:
				{
					Tcl_Obj**	sv;
					int			sc;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc != 2)
						THROW_ERROR_LABEL(finally, code, "Definition must be a list: name value: \"", Tcl_GetString(v), "\"");
					tcc_define_symbol(tcc, Tcl_GetString(sv[0]), Tcl_GetString(sv[1]));
				}
				break;

			case PART_FILTER:
				int			len;
				Tcl_GetStringFromObj(v, &len);

				replace_tclobj(&filter, len ? v : NULL);
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
	*r = (struct jitc_intrep){
		.s = tcc,
		.interp = interp,
		.exported_symbols = exported_symbols,
		.exported_headers = exported_headers
	};
	exported_headers = exported_symbols = NULL;	// Transfer their refs (if any) to r->export_*

	tcc_relocate(tcc, TCC_RELOCATE_AUTO);

	if (compile_errors) goto finally;
	replace_tclobj(&r->symbols, Tcl_NewDictObj());
	tcc_list_symbols(tcc, r->symbols, list_symbols_dict);

	// Avoid a circular reference between cdef and our new jitc intrep obj
	replace_tclobj(&r->cdef, Tcl_DuplicateObj(cdef));

	cdef_init*	init = tcc_get_symbol(tcc, "init");
	if (init)
		TEST_OK_LABEL(finally, code, (init)(interp));

	replace_tclobj(&r->debugfiles, debugfiles);
	replace_tclobj(&debugfiles, NULL);

finally:
	if (mutexheld) {
		mutexheld = 0;
		Tcl_MutexUnlock(&g_tcc_mutex);
	}

	Tcl_DStringFree(&preamble);

	if (compile_errors) {
		Tcl_InterpState	state = Tcl_SaveInterpState(interp, code);
		Tcl_Obj*	cmd[5] = {0};
		int			cmdc = 3;
		Tcl_Obj*	res = NULL;
		Tcl_Obj*	errorcode = NULL;
		Tcl_Obj*	errormsg = NULL;
		Tcl_Obj**	resv;
		int			resc;

		if (!compileerror_code) replace_tclobj(&compileerror_code, l->lit[LIT_BLANK]);

		replace_tclobj(&cmd[0], l->lit[LIT_COMPILEERROR]);
		replace_tclobj(&cmd[1], compileerror_code);
		replace_tclobj(&cmd[2], compile_errors);
		if (code != TCL_OK) {
			replace_tclobj(&cmd[3], Tcl_GetObjResult(interp));
			replace_tclobj(&cmd[4], Tcl_GetReturnOptions(interp, code));
			cmdc += 2;
		}
		TEST_OK_LABEL(done_compileerror, code, Tcl_EvalObjv(interp, cmdc, cmd, TCL_EVAL_DIRECT | TCL_EVAL_GLOBAL));
		replace_tclobj(&res, Tcl_GetObjResult(interp));
		TEST_OK_LABEL(done_compileerror, code, Tcl_ListObjGetElements(interp, res, &resc, &resv));
		replace_tclobj(&errorcode, resv[0]);
		replace_tclobj(&errormsg, resv[1]);
		code = Tcl_RestoreInterpState(interp, state);
		Tcl_SetObjErrorCode(interp, errorcode);
		Tcl_SetObjResult(interp, errormsg);
		code = TCL_ERROR;
	done_compileerror:
		replace_tclobj(&errorcode, NULL);
		replace_tclobj(&errormsg, NULL);
		replace_tclobj(&res, NULL);
	}

	if (code == TCL_OK) {
		*rPtr = r;
		r = NULL;
		tcc = NULL;
	}

	if (tcc) {
		tcc_delete(tcc);
		tcc = NULL;
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
	replace_tclobj(&filter,			NULL);
	replace_tclobj(&exported_symbols,	NULL);
	replace_tclobj(&exported_headers,	NULL);
	replace_tclobj(&compileerror_code,	NULL);

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

	return code;
}

//}}}
int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct jitc_intrep** rPtr) //{{{
{
	int					code = TCL_OK;
	Tcl_ObjIntRep*		ir = Tcl_FetchIntRep(obj, &jitc_objtype);
	struct jitc_intrep*	r = NULL;

	if (ir == NULL) {
		Tcl_ObjIntRep	newir = {0};

		TEST_OK_LABEL(finally, code, compile(interp, obj, (struct jitc_intrep **)&newir.twoPtrValue.ptr1));

		Tcl_FreeIntRep(obj);
		Tcl_StoreIntRep(obj, &jitc_objtype, &newir);
		ir = Tcl_FetchIntRep(obj, &jitc_objtype);
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
static void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //{{{
{
	struct interp_cx*	l = cdata;

	for (int i=0; i<LIT_SIZE; i++)
		replace_tclobj(&l->lit[i], NULL);

	ckfree(l);
	l = NULL;
}

//}}}
// Internal API }}}
// Stubs API {{{
int Jitc_GetSymbolFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj* symbol, void** val) //{{{
{
	int					code = TCL_OK;
	struct jitc_intrep*	r = NULL;
	Tcl_Obj*			valobj = NULL;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, cdef, &r));
	TEST_OK_LABEL(finally, code, Tcl_DictObjGet(interp, r->symbols, symbol, &valobj));
	if (valobj) {
		Tcl_WideInt	w;
		TEST_OK_LABEL(finally, code, Tcl_GetWideIntFromObj(interp, valobj, &w));
		*val = UINT2PTR(w);
	} else {
		Tcl_SetErrorCode(interp, "JITC", "SYMBOL", Tcl_GetString(symbol), NULL);
		THROW_ERROR_LABEL(finally, code, "Symbol not found: \"", Tcl_GetString(symbol), "\"");
	}


finally:
	return code;
}

//}}}
int Jitc_GetSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** symbols) //{{{
{
	int					code = TCL_OK;
	struct jitc_intrep*	r = NULL;
	Tcl_Obj*			lsymbols = NULL;
	Tcl_DictSearch		search;
	Tcl_Obj*			k = NULL;
	Tcl_Obj*			v = NULL;
	int					searching = 0;
	int					done;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, cdef, &r));

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
int Jitc_GetExportHeadersFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** headers) //{{{
{
	int					code = TCL_OK;
	struct jitc_intrep*	r = NULL;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, cdef, &r));

	replace_tclobj(headers, r->exported_headers);

finally:
	return code;
}

//}}}
int Jitc_GetExportSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** symbols) //{{{
{
	int					code = TCL_OK;
	struct jitc_intrep*	r = NULL;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, cdef, &r));

	replace_tclobj(symbols, r->exported_symbols);

finally:
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

	TEST_OK_LABEL(finally, code, Jitc_GetSymbolFromObj(interp, objv[1], objv[2], (void**)&proc));
	code = (proc)(NULL, interp, objc-2, objv+2);

finally:
	return code;
}

//}}}
static int symbols_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	int				code = TCL_OK;
	Tcl_Obj*		symbols = NULL;

	CHECK_ARGS(1, "cdef");

	TEST_OK_LABEL(finally, code, Jitc_GetSymbolsFromObj(interp, objv[1], &symbols));
	Tcl_SetObjResult(interp, symbols);
	replace_tclobj(&symbols, NULL);

finally:
	return code;
}

//}}}
static int mkdtemp_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	int				code = TCL_OK;
	char*     template = NULL;

	CHECK_ARGS(1, "template");

	template = strdup(Tcl_GetString(objv[1]));

	char* dir = mkdtemp(template);
	if (dir == NULL) {
		int			err = Tcl_GetErrno();
		const char*	errstr = Tcl_ErrnoId();

		if (err == EINVAL)
			THROW_ERROR_LABEL(finally, code, "Template must end with XXXXXX");
		Tcl_SetErrorCode(interp, "POSIX", errstr, Tcl_ErrnoMsg(err), NULL);
		THROW_ERROR_LABEL(finally, code, "Could not create temporary directory: ", Tcl_ErrnoMsg(err));
	}
	Tcl_SetObjResult(interp, Tcl_NewStringObj(dir, -1));

finally:
	if (template) {
		free(template);
		template = NULL;
	}
	return code;
}

//}}}

#define NS	"::jitc"
static struct cmd {
	char*			name;
	Tcl_ObjCmdProc*	proc;
} cmds[] = {
	{NS "::capply",		capply_cmd},
	{NS "::symbols",	symbols_cmd},
	{NS "::mkdtemp",	mkdtemp_cmd},
	{NULL,				NULL}
};
// Script API }}}

extern const JitcStubs* const jitcConstStubsPtr;

#ifdef __cplusplus
extern "C" {
#endif
DLLEXPORT int Jitc_Init(Tcl_Interp* interp)
{
	int					code = TCL_OK;
	//Tcl_Namespace*		ns = NULL;
	struct cmd*			c = cmds;
	struct interp_cx*	l = NULL;

#if USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL)
		return TCL_ERROR;
#endif

	//ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	//TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	// Set up interp_cx {{{
	l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){0};
	Tcl_SetAssocData(interp, "jitc", free_interp_cx, l);

	for (int i=0; i<LIT_SIZE; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(lit_str[i], -1));
	// Set up interp_cx }}}

	while (c->name) {
		if (NULL == Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL)) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("Could not create command %s", c->name));
			code = TCL_ERROR;
			goto finally;
		}
		c++;
	}

	TEST_OK_LABEL(finally, code, Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION, jitcConstStubsPtr));

finally:
	if (code != TCL_OK) Tcl_DeleteAssocData(interp, "jitc");

	return code;
}

#ifdef __cplusplus
}
#endif
