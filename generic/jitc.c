#include <jitcInt.h>
#include <tip445.h>
#include <sys/stat.h>
#include <names.h>

TCL_DECLARE_MUTEX(g_pkgdir_mutex)
static Tcl_Obj* g_pkgdir = NULL;

TCL_DECLARE_MUTEX(g_tcc_mutex)

typedef const char* (cdef_initstubs)(Tcl_Interp* interp, const char* ver);
typedef int (cdef_init)(Tcl_Interp* interp);
typedef int (cdef_release)(Tcl_Interp* interp);

static void free_jitc_internal_rep(Tcl_Obj* obj);
static void dup_jitc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup);
static void update_jitc_string_rep(Tcl_Obj* obj);
static void jit_register_obj(struct jitc_intrep* r, void* buf, unsigned long size);
static void jit_unregister_obj(struct jitc_intrep* r);

Tcl_ObjType jitc_objtype = {
	"Jitc",
	free_jitc_internal_rep,
	dup_jitc_internal_rep,
	update_jitc_string_rep,
	NULL
};

static void free_jitc_internal_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(obj, &jitc_objtype);
	struct jitc_intrep*		r = ir->twoPtrValue.ptr1;
	struct jitc_instance*	instance = ir->twoPtrValue.ptr2;

	instance->next->prev = instance->prev;
	instance->prev->next = instance->next;
	//*instance = (struct jitc_instance){};
	ckfree(instance);  instance = NULL;  ir->twoPtrValue.ptr2 = NULL;

	jit_unregister_obj(r);

	if (r->tcc) {
		if (r->symbols && r->interp) {
			struct interp_cx*	l = Tcl_GetAssocData(r->interp, "jitc", NULL);
			Tcl_Obj*	releasename = NULL;
			Tcl_Obj*	releasesymboladdr = NULL;

			// l can be NULL here if we're here because the interp is being deleted (and so free_interp_cx has been called)
			replace_tclobj(&releasename, l ? l->lit[LIT_RELEASE] : Tcl_NewStringObj("release", -1));
			if (TCL_OK == Tcl_DictObjGet(r->interp, r->symbols, releasename, &releasesymboladdr) && releasesymboladdr) {
				cdef_release*	release = NULL;
				void*			addr = tcc_get_symbol(r->tcc, "release");
				memcpy(&release, &addr, sizeof release);
				if (release) (release)(r->interp);
			}
			replace_tclobj(&releasename, NULL);
		}

		Tcl_MutexLock(&g_tcc_mutex);
		tcc_delete(r->tcc);
		Tcl_MutexUnlock(&g_tcc_mutex);
		r->tcc = NULL;
	}

	replace_tclobj(&r->symbols, NULL);

	r->interp = NULL;
	replace_tclobj(&r->cdef, NULL);
	if (r->debugfiles) {
		Tcl_Obj**	fv;
		Tcl_Size	fc;

		if (TCL_OK == Tcl_ListObjGetElements(NULL, r->debugfiles, &fc, &fv)) {
			for (Tcl_Size i=0; i<fc; i++) {
				if (TCL_OK != Tcl_FSDeleteFile(fv[i])) {
					// TODO: what?
				}
			}
		}
	}
	replace_tclobj(&r->debugfiles, NULL);
	if (r->debugdir) {
		Tcl_Obj* errstr = NULL;
		Tcl_FSRemoveDirectory(r->debugdir, 0, &errstr);	// best-effort; jitc-owned dir
		if (errstr) {
			// Tcl_FSRemoveDirectory sets errstr to an unowned Tcl_Obj,
			// bounce the refcount here to release it
			Tcl_IncrRefCount(errstr);
			Tcl_DecrRefCount(errstr);
		}
		replace_tclobj(&r->debugdir, NULL);
	}
	replace_tclobj((Tcl_Obj**)&ir->twoPtrValue.ptr2, NULL);
	replace_tclobj(&r->exported_symbols, NULL);
	replace_tclobj(&r->exported_headers, NULL);
	replace_tclobj(&r->used, NULL);

	ckfree(r);
	r = NULL;
}

//}}}
static void dup_jitc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup) //{{{
{
	Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(src, &jitc_objtype);
	struct jitc_intrep*		r = ir->twoPtrValue.ptr1;
	struct interp_cx*		l = Tcl_GetAssocData(r->interp, "jitc", NULL);
	Tcl_ObjInternalRep		newir = {.twoPtrValue = {}}; // defend against gcc 15.2's broken treatment of unions
	struct jitc_instance*	instance = NULL;

	// Shouldn't ever need to happen, but if it does we have to recompile from source.
	// Set the dup's intrep to a dup of the cdef list instead
	replace_tclobj((Tcl_Obj**)&newir.twoPtrValue.ptr2, r->cdef);

	instance = ckalloc(sizeof *instance);
	*instance = (struct jitc_instance){
		.next	= l->instance_head.next,
		.prev	= &l->instance_head,
		.obj	= dup
	};
	l->instance_head.next = instance;
	instance->next->prev = instance;

	newir.twoPtrValue.ptr2 = instance;

	Tcl_StoreInternalRep(dup, &jitc_objtype, &newir);
}

//}}}
void update_jitc_string_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &jitc_objtype);
	struct jitc_intrep*	r = ir->twoPtrValue.ptr1;
	Tcl_Size			newstring_len;
	const char*			newstring = Tcl_GetStringFromObj(r->cdef, &newstring_len);

	Tcl_InvalidateStringRep(obj);	// Just in case, panic below if obj->bytes != NULL
	Tcl_InitStringRep(obj, newstring, newstring_len);
}
//}}}

// Internal API {{{
// Interface with GDB JIT API {{{
TCL_DECLARE_MUTEX(gdb_jit_mutex)

/* GDB puts a breakpoint in this function.  */
void __attribute__((noinline)) __jit_debug_register_code() { }

/* Make sure to specify the version statically, because the
   debugger may check the version before we can set it.  */
struct jit_descriptor __jit_debug_descriptor = { 1, 0, 0, 0 };

// Register an in-memory ELF .o (as produced by elf_output_obj_to_mem)
// with any attached debugger via the GDB JIT interface. Takes ownership
// of `buf` — the descriptor entry holds it until jit_unregister_obj()
// frees it with libc free() (which matches open_memstream's allocator).
//
// The descriptor list, action_flag and relevant_entry must be mutated
// atomically with respect to the breakpoint hit in
// __jit_debug_register_code(), so the whole sequence is held under
// gdb_jit_mutex.
static void jit_register_obj(struct jitc_intrep* r, void* buf, unsigned long size) //{{{
{
	r->jit_symbols.symfile_addr = buf;
	r->jit_symbols.symfile_size = (uint64_t)size;

	Tcl_MutexLock(&gdb_jit_mutex);
	r->jit_symbols.prev_entry = NULL;
	r->jit_symbols.next_entry = __jit_debug_descriptor.first_entry;
	if (__jit_debug_descriptor.first_entry)
		__jit_debug_descriptor.first_entry->prev_entry = &r->jit_symbols;
	__jit_debug_descriptor.first_entry    = &r->jit_symbols;
	__jit_debug_descriptor.relevant_entry = &r->jit_symbols;
	__jit_debug_descriptor.action_flag    = JIT_REGISTER_FN;
	__jit_debug_register_code();
	Tcl_MutexUnlock(&gdb_jit_mutex);
}

//}}}
static void jit_unregister_obj(struct jitc_intrep* r) //{{{
{
	if (!r->jit_symbols.symfile_addr) return;	// never registered

	Tcl_MutexLock(&gdb_jit_mutex);
	if (r->jit_symbols.prev_entry)
		r->jit_symbols.prev_entry->next_entry = r->jit_symbols.next_entry;
	else
		__jit_debug_descriptor.first_entry    = r->jit_symbols.next_entry;
	if (r->jit_symbols.next_entry)
		r->jit_symbols.next_entry->prev_entry = r->jit_symbols.prev_entry;

	__jit_debug_descriptor.relevant_entry = &r->jit_symbols;
	__jit_debug_descriptor.action_flag    = JIT_UNREGISTER_FN;
	__jit_debug_register_code();
	Tcl_MutexUnlock(&gdb_jit_mutex);

	free((void*)r->jit_symbols.symfile_addr);
	r->jit_symbols.symfile_addr = NULL;
	r->jit_symbols.symfile_size = 0;
}

//}}}
// Interface with GDB JIT API }}}

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
	"_initstubs",
	"init",
	"release",
	"return -level 0 tclstub[if {![package vsatisfies [info tclversion] 9.0-]} {info tclversion}]",
	"return -level 0 tcl[info tclversion]",
	"info tclversion",
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
	Tcl_Obj*	nameobj = NULL;		defer { replace_tclobj(&nameobj, NULL); };
	Tcl_Obj*	valobj = NULL;		defer { replace_tclobj(&valobj, NULL); };
	Tcl_WideInt	w = (Tcl_WideInt)val;

	//fprintf(stderr, "list_symbols_dict: \"%s\"\n", name);
	replace_tclobj(&nameobj, Tcl_NewStringObj(name, -1));
	replace_tclobj(&valobj,  Tcl_NewWideIntObj(w));

	if (TCL_OK != Tcl_DictObjPut(NULL, symbolsdict, nameobj, valobj))
		Tcl_Panic("Failed to append symbol to list: %s: %p", name, val);
}

//}}}
int compile(Tcl_Interp* interp, Tcl_Obj* cdef, struct interp_cx* l, struct jitc_intrep** rPtr) //{{{
{
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
	} mode = MODE_TCL;

	Tcl_Obj**	ov;
	Tcl_Size	oc;
	Tcl_Size	i;
	Tcl_Obj*	debugpath = NULL;			defer { replace_tclobj(&debugpath, NULL); };
	Tcl_Obj*	compile_errors = NULL;		defer { replace_tclobj(&compile_errors, NULL); };
	Tcl_Obj*	filter = NULL;				defer { replace_tclobj(&filter, NULL); };
	Tcl_Obj*	exported_headers = NULL;	defer { replace_tclobj(&exported_headers, NULL); };
	Tcl_Obj*	exported_symbols = NULL;	defer { replace_tclobj(&exported_symbols, NULL); };
	Tcl_Obj*	compileerror_code = NULL;	defer { replace_tclobj(&compileerror_code, NULL); };
	Tcl_Obj*	add_library_queue = NULL;	defer { replace_tclobj(&add_library_queue, NULL); };
	Tcl_Obj*	add_symbol_queue = NULL;	defer { replace_tclobj(&add_symbol_queue, NULL); };
	Tcl_Obj*	used = NULL;				defer { replace_tclobj(&used, NULL); };
	Tcl_Obj*	extra_errormsg = NULL;		defer { replace_tclobj(&extra_errormsg, NULL); };

	Tcl_DString	preamble; Tcl_DStringInit(&preamble);		defer { Tcl_DStringFree(&preamble); };

	Tcl_Obj*	debugfiles = NULL;
	Tcl_Obj*	debugdir   = NULL;	// non-NULL: jitc-owned mkdtemp() dir to rmdir after debugfiles cleanup
	defer {
		if (debugfiles) {
			Tcl_Obj**	fv;
			Tcl_Size	fc;
			if (TCL_OK == Tcl_ListObjGetElements(NULL, debugfiles, &fc, &fv)) {
				for (Tcl_Size i=0; i<fc; i++) {
					if (TCL_OK != Tcl_FSDeleteFile(fv[i])) {
						// TODO: what?
					}
				}
			}
			replace_tclobj(&debugfiles, NULL);
		}
		if (debugdir) {
			Tcl_FSRemoveDirectory(debugdir, 0, NULL);	// best-effort
			replace_tclobj(&debugdir, NULL);
		}
	}

	struct jitc_intrep*	r = NULL;
	defer {
		if (r) {
			jit_unregister_obj(r);
			replace_tclobj(&r->symbols,		NULL);
			replace_tclobj(&r->cdef,		NULL);
			replace_tclobj(&r->debugfiles,	NULL);
			replace_tclobj(&r->debugdir,	NULL);
			r->interp = NULL;
			if (r->tcc) {
				tcc_delete(r->tcc);
				r->tcc = NULL;
			}
			ckfree(r);
		}
	}

#define CHECK_TCC_SIMPLE(tccfunc) \
	do { \
		if (-1 == (tccfunc) || compile_errors) goto compile_error; \
	} while(0)
#define CHECK_TCC_PRINTF(tccfunc, ...) \
	do { \
		if (-1 == (tccfunc) || compile_errors) { \
			replace_tclobj(&extra_errormsg, Tcl_ObjPrintf(__VA_ARGS__)); \
			goto compile_error; \
		} \
	} while(0)
#define CHECK_TCC_MACRO(_1, _2, _3, _4, _5, NAME, ...) NAME
#define CHECK_TCC(...) CHECK_TCC_MACRO(__VA_ARGS__, \
	CHECK_TCC_PRINTF, CHECK_TCC_PRINTF, CHECK_TCC_PRINTF, CHECK_TCC_PRINTF, \
	CHECK_TCC_SIMPLE)(__VA_ARGS__)

	TEST_OK(Tcl_ListObjGetElements(interp, cdef, &oc, &ov));
	if (oc % 2 == 1)
		THROW_PRINTF("cdef must be a list with an even number of elements (got %" TCL_SIZE_MODIFIER "d): %s", oc, Tcl_GetString(cdef));

	// First pass through the parts to check for a debugpath setting
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		enum partenum	part = partidx;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
		switch (part) {
			case PART_DEBUG:
				replace_tclobj(&debugpath, ov[i+1]);
				break;
			case PART_MODE:
				int	modeidx;
				TEST_OK(Tcl_GetIndexFromObj(interp, ov[i+1], modes, "mode", TCL_EXACT, &modeidx));
				mode = modeidx;
				break;

			// Have to pre-flight these to ensure that the compilation (if required) happens before we lock the Mutex and create the TCCState below
			case PART_SYMBOLS:
				{
					Tcl_Obj**	sv;
					Tcl_Size	sc;
					struct jitc_intrep*	ur;

					TEST_OK(Tcl_ListObjGetElements(interp, ov[i+1], &sc, &sv));
					if (sc >= 1) {
						if (!used) replace_tclobj(&used, Tcl_NewListObj(1, NULL));
						TEST_OK(Tcl_ListObjAppendElement(interp, used, sv[0]));
						TEST_OK(get_r_from_obj(interp, sv[0], &ur));
					}
				}
				break;
			case PART_USE:
				{
					struct jitc_intrep*	ur;
					if (!used) replace_tclobj(&used, Tcl_NewListObj(1, NULL));
					TEST_OK(Tcl_ListObjAppendElement(interp, used, ov[i+1]));
					TEST_OK(get_r_from_obj(interp, ov[i+1], &ur));
				}
				break;
		}
#pragma GCC diagnostic pop
	}

	if (debugpath) {
		Tcl_StatBuf*	stat = Tcl_AllocStatBuf();	defer { ckfree(stat); };
		if (
				-1 == Tcl_FSStat(debugpath, stat) ||	// Doesn't exist
				!(S_ISDIR(Tcl_GetModeFromStat(stat)))	// Not a directory
		) THROW_ERROR("debug path \"", Tcl_GetString(debugpath), "\" doesn't exist");
	}

	Tcl_MutexLock(&g_tcc_mutex);		defer { Tcl_MutexUnlock(&g_tcc_mutex); };

	struct TCCState*	tcc = tcc_new();		defer { if (tcc) tcc_delete(tcc); }
	tcc_set_error_func(tcc, &compile_errors, errfunc);
	CHECK_TCC(tcc_set_options(tcc, "-Wl,--enable-new-dtags"));

	// Set some mode-dependent defaults
	switch (mode) {
		case MODE_TCL:
		case MODE_RAW:
			{
				Tcl_Obj*	includepath = NULL;		defer { replace_tclobj(&includepath,	NULL); }
				Tcl_Obj*	librarypath = NULL;		defer { replace_tclobj(&librarypath,	NULL); }
				Tcl_Obj*	tccpath = NULL;			defer { replace_tclobj(&tccpath,		NULL); }

				replace_tclobj(&tccpath,     Tcl_ObjGetVar2(interp, l->lit[LIT_TCC_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (tccpath == NULL) return TCL_ERROR;
				replace_tclobj(&includepath, Tcl_ObjGetVar2(interp, l->lit[LIT_INCLUDEPATH_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (includepath == NULL) return TCL_ERROR;
				replace_tclobj(&librarypath, Tcl_ObjGetVar2(interp, l->lit[LIT_LIBRARYPATH_VAR], NULL, TCL_LEAVE_ERR_MSG));
				if (librarypath == NULL) return TCL_ERROR;

				tcc_set_lib_path(tcc, Tcl_GetString(tccpath));

				Tcl_Obj**	ov;
				Tcl_Size	oc;
				TEST_OK(Tcl_ListObjGetElements(interp, includepath, &oc, &ov));
				for (Tcl_Size i=0; i<oc; i++) tcc_add_include_path(tcc, Tcl_GetString(ov[i]));
				TEST_OK(Tcl_ListObjGetElements(interp, librarypath, &oc, &ov));
				for (Tcl_Size i=0; i<oc; i++)
					CHECK_TCC(tcc_add_library_path(tcc, Tcl_GetString(ov[i])),
						"Error adding library path \"%s\"", Tcl_GetString(ov[i]));

				if (mode == MODE_TCL) {
#if STUBSMODE
					tcc_define_symbol(tcc, "USE_TCL_STUBS", "1");
#endif
					Tcl_DStringAppend(&preamble, "#include <tclstuff.h>\n", -1);
				}
			}
			break;

		default:
			THROW_ERROR("Unhandled mode");
	}

	// Second pass through the parts to process PART_PACKAGE and PART_USE directives
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		enum partenum	part = partidx;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
		switch (part) {
			case PART_OPTIONS: CHECK_TCC(tcc_set_options(tcc, Tcl_GetString(ov[i+1]))); break;	// Must be set before tcc_set_output_type
			case PART_LIBRARY_PATH:
				CHECK_TCC(tcc_add_library_path(tcc, Tcl_GetString(ov[i+1])), "Error adding library path \"%s\"", Tcl_GetString(ov[i+1]));
				break;

			case PART_PACKAGE: //{{{
				{
					Tcl_Size	pc;
					Tcl_Obj**	pv = NULL;
					Tcl_Obj*	cmd[3] = {};	defer { for (int i=0; i<3; i++) replace_tclobj(&cmd[i], NULL); };

					// TODO: Cache these lookups
					TEST_OK(Tcl_ListObjGetElements(interp, ov[i+1], &pc, &pv));
					if (pc < 1) THROW_ERROR("At least package name is required");
					TEST_OK(Tcl_PkgRequireProc(interp, Tcl_GetString(pv[0]), pc-1, pv+1, NULL));
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
					Tcl_Obj*	vals[KEY_END] = {};	defer { for (int i=0; i<KEY_END; i++) replace_tclobj(&vals[i], NULL); }

					for (int i=0; keys[i]; i++) {
						Tcl_InterpState	state = Tcl_SaveInterpState(interp, 0);
						replace_tclobj(&cmd[2], Tcl_NewStringObj(keys[i], -1));
						if (TCL_OK == Tcl_EvalObjv(interp, 3, cmd, TCL_EVAL_GLOBAL))
							replace_tclobj(&vals[i], Tcl_GetObjResult(interp));
						Tcl_RestoreInterpState(interp, state);
					}

					if (vals[KEY_HEADER]) {
						Tcl_DStringAppend(&preamble, "\n#include <", -1);
						Tcl_DStringAppend(&preamble, Tcl_GetString(vals[KEY_HEADER]), -1);
						Tcl_DStringAppend(&preamble, ">\n", -1);
					}

					if (vals[KEY_INCLUDEDIR_RUNTIME])
						tcc_add_include_path(tcc, Tcl_GetString(vals[KEY_INCLUDEDIR_RUNTIME]));
					if (vals[KEY_INCLUDEDIR_INSTALL])
						tcc_add_include_path(tcc, Tcl_GetString(vals[KEY_INCLUDEDIR_INSTALL]));
					if (vals[KEY_LIBDIR_RUNTIME]) {
						if (-1 == tcc_add_library_path(tcc, Tcl_GetString(vals[KEY_LIBDIR_RUNTIME]))) {
							// TODO: what?
						} else {
							Tcl_Obj*	runpath_opt = NULL;
							replace_tclobj(&runpath_opt, Tcl_ObjPrintf("-Wl,-rpath=%s", Tcl_GetString(vals[KEY_LIBDIR_RUNTIME])));
							CHECK_TCC(tcc_set_options(tcc, Tcl_GetString(runpath_opt)));
							replace_tclobj(&runpath_opt, NULL);
						}
					}
					if (vals[KEY_LIBDIR_INSTALL]) {
						if (-1 == tcc_add_library_path(tcc, Tcl_GetString(vals[KEY_LIBDIR_INSTALL]))) {
							// TODO: what?
						} else {
							Tcl_Obj*	runpath_opt = NULL;
							replace_tclobj(&runpath_opt, Tcl_ObjPrintf("-Wl,-rpath=%s", Tcl_GetString(vals[KEY_LIBDIR_INSTALL])));
							CHECK_TCC(tcc_set_options(tcc, Tcl_GetString(runpath_opt)));
							replace_tclobj(&runpath_opt, NULL);
						}
					}
					if (vals[KEY_LIBRARY]) {
						const char* libstr = Tcl_GetString(vals[KEY_LIBRARY]);
						if (strncmp("lib", libstr, 3) == 0) libstr += 3;
						if (!add_library_queue) replace_tclobj(&add_library_queue, Tcl_NewListObj(1, NULL));
						TEST_OK(Tcl_ListObjAppendElement(interp, add_library_queue, Tcl_NewStringObj(libstr, -1)));
					}
				}
				break;
				//}}}
			case PART_USE: //{{{
				{
					Tcl_Obj*	useobj = ov[i+1];
					Tcl_Obj*	use_headers = NULL;		defer { replace_tclobj(&use_headers, NULL); };
					Tcl_Obj*	use_symbols = NULL;		defer { replace_tclobj(&use_symbols, NULL); };

					TEST_OK(Jitc_GetExportHeadersFromObj(interp, useobj, &use_headers));
					TEST_OK(Jitc_GetExportSymbolsFromObj(interp, useobj, &use_symbols));

					if (use_headers) {
						Tcl_Size	headerstrlen;
						const char*	headerstr = Tcl_GetStringFromObj(use_headers, &headerstrlen);
						Tcl_DStringAppend(&preamble, headerstr, headerstrlen);
					}
					if (use_symbols) {
						if (!add_symbol_queue) replace_tclobj(&add_symbol_queue, Tcl_NewListObj(2, NULL));
						TEST_OK(Tcl_ListObjAppendElement(interp, add_symbol_queue, useobj));
						TEST_OK(Tcl_ListObjAppendElement(interp, add_symbol_queue, use_symbols));
					}
				}
				break;
				//}}}
		}
#pragma GCC diagnostic pop
	}

	// Compile-to-memory: tcc_relocate places code in malloc'd, mprotect'd
	// memory (no .so on disk, no dlopen). Symbols are resolved via
	// tcc_get_symbol against TCC's own symbol table. This avoids both
	// musl's dlclose-leaks-mappings behavior and the dlsym _init/init
	// name-mangling that broke cross-module symbol resolution on musl.
	if (debugpath)
		CHECK_TCC(tcc_set_options(tcc, "-g"));

	// If the user enabled debug via `options` (-g, -gdwarf-N, ...) but
	// didn't supply a debug path, allocate a per-cdef tempdir for the
	// source files tcc_compile_string_file writes — gdb opens those via
	// the path embedded in DWARF when stepping into JIT'd code. The .o
	// itself goes straight from tinycc into a malloc'd buffer registered
	// with the GDB JIT interface; nothing about it touches disk.
	if (!debugpath && tcc_get_debug(tcc)) {
		char tmpl[] = P_tmpdir "/jitc_dbg_XXXXXX";
		if (mkdtemp(tmpl) == NULL) THROW_POSIX("Could not create debug temp directory");
		replace_tclobj(&debugpath, Tcl_NewStringObj(tmpl, -1));
		replace_tclobj(&debugdir, debugpath);
	}

	tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);

	if (add_symbol_queue) {
		Tcl_Size	qc;
		Tcl_Obj**	qv = NULL;

		TEST_OK(Tcl_ListObjGetElements(interp, add_symbol_queue, &qc, &qv));

		if (qc % 2 != 0)
			THROW_ERROR("add_symbol_queue must have an even number of elements");

		for (Tcl_Size i=0; i<qc; i+=2) {
			Tcl_Size	sc;
			Tcl_Obj**	sv = NULL;
			Tcl_Obj*	useobj		= qv[i];
			Tcl_Obj*	use_symbols	= qv[i+1];

			TEST_OK(Tcl_ListObjGetElements(interp, use_symbols, &sc, &sv));
			for (Tcl_Size s=0; s<sc; s++) {
				void*	val = NULL;
				TEST_OK(Jitc_GetSymbolFromObj(interp, useobj, sv[s], &val));
				tcc_add_symbol(tcc, Tcl_GetString(sv[s]), val);
			}
		}

		replace_tclobj(&add_symbol_queue, NULL);
	}

	// Third pass through the parts to process PART_EXPORT directives	(export headers must be appended to preamble after use ones)
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		enum partenum	part = partidx;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
		switch (part) {
			case PART_EXPORT: //{{{
				{
					Tcl_Obj**	ev = NULL;
					Tcl_Size	ec;

					TEST_OK(Tcl_ListObjGetElements(interp, ov[i+1], &ec, &ev));
					for (Tcl_Size ei=0; ei<ec; ei+=2) {
						static const char* exportkeys[] = {
							"symbols",
							"header",
							NULL
						};
						enum exportkeyenum {
							EXPORT_SYMBOLS,
							EXPORT_HEADER
						} exportkey;
						int exportkeyidx;

						TEST_OK(Tcl_GetIndexFromObj(interp, ev[ei], exportkeys, "key", TCL_EXACT, &exportkeyidx));
						exportkey = exportkeyidx;
						switch (exportkey) {
							case EXPORT_SYMBOLS:
								replace_tclobj(&exported_symbols, ev[ei+1]);
								break;
							case EXPORT_HEADER:
								{
									Tcl_Size	headerstrlen;
									const char*	headerstr = Tcl_GetStringFromObj(ev[ei+1], &headerstrlen);

									replace_tclobj(&exported_headers, ev[ei+1]);
									Tcl_DStringAppend(&preamble, headerstr, headerstrlen);
								}
								break;
						}
					}
				}
				break;
				//}}}
		}
#pragma GCC diagnostic pop
	}

	// Hack around wchar confusion on musl / aarch64
	tcc_define_symbol(tcc, "__DEFINED_wchar_t", "");

	replace_tclobj(&debugfiles, Tcl_NewListObj(0, NULL));

	unsigned codeseq = 1;
	for (i=0; i<oc; i+=2) {
		Tcl_Obj*	v = ov[i+1];

		int partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		enum partenum	part = partidx;

		switch (part) {
			case PART_OPTIONS:
			case PART_MODE:
			case PART_DEBUG:
			case PART_PACKAGE:
			case PART_USE:
			case PART_LIBRARY_PATH:
				/* Handled above */
				break;

			case PART_CODE: //{{{
				{
					Tcl_Size	len;
					const char*	str = Tcl_GetStringFromObj(v, &len);

					Tcl_DString		c;
					Tcl_DStringInit(&c);	defer { Tcl_DStringFree(&c); };
					Tcl_DStringAppend(&c, Tcl_DStringValue(&preamble), Tcl_DStringLength(&preamble));
					Tcl_DStringAppend(&c, str, len);

					if (filter) {
						Tcl_Obj*	in = NULL;			defer { replace_tclobj(&in,			NULL); };
						Tcl_Obj*	filtercmd = NULL;	defer { replace_tclobj(&filtercmd,	NULL); };

						replace_tclobj(&filtercmd, Tcl_DuplicateObj(filter));
						replace_tclobj(&in, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
						TEST_OK(Tcl_ListObjAppendElement(interp, filtercmd, in));
						TEST_OK(Tcl_EvalObjEx(interp, filtercmd, 0));
						Tcl_DStringSetLength(&c, 0);
						Tcl_Size filtered_len;
						const char* filtered_str = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &filtered_len);
						Tcl_DStringAppend(&c, filtered_str, filtered_len);
						//fprintf(stderr, "// transformed code (with %s): %.*s", Tcl_GetString(filter), filtered_len, filtered_str);
						Tcl_ResetResult(interp);
					}

#if STUBSMODE
					if (mode == MODE_TCL)
						CHECK_TCC(tcc_compile_string(tcc, "#include <tcl.h>\nconst char* _initstubs(Tcl_Interp* interp, const char* ver) {return Tcl_InitStubs(interp, ver, 0);}"),
									"%s", "Error compiling _initstubs");
#endif

					if (debugpath) { // tcc_compile_string_file writes the source to debugfile (because -g is set) for gdb to attribute lines to {{{
						Tcl_Obj*	pathelements = NULL;	defer { replace_tclobj(&pathelements, NULL); };
						Tcl_Obj*	debugfile = NULL;		defer { replace_tclobj(&debugfile, NULL); };

						replace_tclobj(&pathelements, Tcl_NewListObj(2, (Tcl_Obj*[]){
							debugpath,
							Tcl_ObjPrintf("0x%" PRIxPTR "_%d.c", (uintptr_t)tcc, codeseq++)	// TODO: use name(tcc) for a friendly name instead?
							//Tcl_ObjPrintf("%s_%d.c", name(tcc), codeseq++)
						}));

						replace_tclobj(&debugfile, Tcl_FSJoinPath(pathelements, 2));
						TEST_OK(Tcl_ListObjAppendElement(interp, debugfiles, debugfile));

						int rc = tcc_compile_string_file(tcc, Tcl_DStringValue(&c), Tcl_GetString(debugfile));
						if (rc == -1 || compile_errors)
							replace_tclobj(&compileerror_code, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
						CHECK_TCC(rc, "Error compiling file \"%s\"", Tcl_GetString(debugfile));
						//}}}
					} else {
						int rc = tcc_compile_string(tcc, Tcl_DStringValue(&c));
						if (rc == -1 || compile_errors)
							replace_tclobj(&compileerror_code, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
						CHECK_TCC(rc, "Error compiling code");
					}
					break;
				}
				break;
				//}}}

			case PART_FILE:
				CHECK_TCC(tcc_add_file(tcc, Tcl_GetString(v)), "Error compiling file \"%s\"", Tcl_GetString(v));
				break;

			case PART_INCLUDE_PATH:		CHECK_TCC(tcc_add_include_path   (tcc, Tcl_GetString(v))); break;
			case PART_SYSINCLUDE_PATH:	CHECK_TCC(tcc_add_sysinclude_path(tcc, Tcl_GetString(v))); break;
			case PART_TCCPATH:			tcc_set_lib_path    (tcc, Tcl_GetString(v));               break;
			case PART_UNDEFINE:			tcc_undefine_symbol (tcc, Tcl_GetString(v));               break;

			case PART_SYMBOLS: //{{{
				{
					// treat this as another code object+symbols name to retrieve
					Tcl_Obj**	sv;
					Tcl_Size	sc;

					TEST_OK(Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc < 1)
						THROW_ERROR("Symbol definition must be a list: cdef symbol: \"", Tcl_GetString(v), "\"");

					for (Tcl_Size i=1; i<sc; i++) {
						void*	val = NULL;
						TEST_OK(Jitc_GetSymbolFromObj(interp, sv[0], sv[i], &val));
						tcc_add_symbol(tcc, Tcl_GetString(sv[i]), val);
					}
				}
				break;
				//}}}

			case PART_EXPORT: break;

			case PART_LIBRARY:
				CHECK_TCC(tcc_add_library(tcc, Tcl_GetString(v)), "Error adding library \"%s\"", Tcl_GetString(v));
				break;

			case PART_DEFINE:
				{
					Tcl_Obj**	sv;
					Tcl_Size	sc;

					TEST_OK(Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc < 1 || sc > 2)
						THROW_ERROR("Definition must be a list: name value: \"", Tcl_GetString(v), "\"");
					if (sc == 1) {
						tcc_define_symbol(tcc, Tcl_GetString(sv[0]), "");
					} else {
						tcc_define_symbol(tcc, Tcl_GetString(sv[0]), Tcl_GetString(sv[1]));
					}
				}
				break;

			case PART_FILTER:
				{
					Tcl_Size	len;
					Tcl_GetStringFromObj(v, &len);

					replace_tclobj(&filter, len ? v : NULL);
				}
				break;

			default:
				THROW_ERROR("Invalid part id");
		}
	}

	if (add_library_queue) { // Can only happen after tcc_set_output_type (tcc_add_library)
		Tcl_Size	qc;
		Tcl_Obj**	qv = NULL;

		TEST_OK(Tcl_ListObjGetElements(interp, add_library_queue, &qc, &qv));

		for (Tcl_Size i=0; i<qc; i++)
			CHECK_TCC(tcc_add_library(tcc, Tcl_GetString(qv[i])), "Error adding library \"%s\"", Tcl_GetString(qv[i]));

		replace_tclobj(&add_library_queue, NULL);
	}

	if (mode == MODE_TCL)
#if STUBSMODE
		CHECK_TCC(tcc_add_library(tcc, Tcl_GetString(l->tclstublib)));
#else
		CHECK_TCC(tcc_add_library(tcc, Tcl_GetString(l->tcllib)));
#endif

	r = ckalloc(sizeof *r);
	*r = (struct jitc_intrep){
		.interp				= interp,
		.used				= used,
		.exported_symbols	= exported_symbols,
		.exported_headers	= exported_headers,
	};
	used = exported_headers = exported_symbols = NULL;	// Transfer their refs (if any) to r->export_*

	CHECK_TCC(tcc_relocate(tcc), "Error relocating compiled code into memory");

	if (debugpath) {
		// Compose the post-relocate ELF (incl. .debug_*) directly into a
		// malloc'd buffer and hand it to the GDB JIT interface — no
		// tempfile, no slurp, no unlink. The buffer's lifetime is the
		// intrep's; jit_unregister_obj() frees it on teardown. The source
		// file (tracked in debugfiles) stays on disk: gdb opens it via
		// the path embedded in DWARF when stepping into JIT'd code.
		void*			obj_buf  = NULL;
		unsigned long	obj_size = 0;
		CHECK_TCC(elf_output_obj_to_mem(tcc, &obj_buf, &obj_size), "Error composing debug object");
		jit_register_obj(r, obj_buf, obj_size);
	}

	replace_tclobj(&r->symbols, Tcl_NewDictObj());
	tcc_list_symbols(tcc, r->symbols, list_symbols_dict);

	// Avoid a circular reference between cdef and our new jitc intrep obj
	replace_tclobj(&r->cdef, Tcl_DuplicateObj(cdef));

#if STUBSMODE
	{
		cdef_initstubs*	initstubs = NULL;
		void*			initstubsaddr = tcc_get_symbol(tcc, "_initstubs");
		memcpy(&initstubs, &initstubsaddr, sizeof initstubs);
		if (initstubs)
			if (NULL == (initstubs)(interp, Tcl_GetString(l->tclver)))
				THROW_ERROR("Could not init Tcl stubs");
	}
#endif

	{
		cdef_init*	init = NULL;
		void*		initaddr = tcc_get_symbol(tcc, "init");
		memcpy(&init, &initaddr, sizeof init);
		if (init) {
			//fprintf(stderr, "cdef defines init, calling: %p, symbols: (%s)\n", init, Tcl_GetString(r->symbols));
			TEST_OK((init)(interp));
		}
	}

	replace_tclobj(&r->debugfiles, debugfiles);
	replace_tclobj(&debugfiles, NULL);

	replace_tclobj(&r->debugdir, debugdir);
	replace_tclobj(&debugdir, NULL);

	// Hand ownership of the TCCState to the intrep — it must outlive any
	// function pointers we hand out via tcc_get_symbol.
	r->tcc = tcc;
	tcc = NULL;

	*rPtr = r;
	r = NULL;

	return TCL_OK;

compile_error:
	{
		Tcl_InterpState	state = Tcl_SaveInterpState(interp, TCL_OK);	defer { if (state) Tcl_DiscardInterpState(state); };
		const int	cmdc = extra_errormsg ? 5 : 3;
		Tcl_Obj*	cmd[5] = {};		defer { for (int i=0; i<5; i++) replace_tclobj(&cmd[i], NULL); };
		Tcl_Obj*	res = NULL;			defer { replace_tclobj(&res,		NULL); };
		Tcl_Obj*	errorcode = NULL;	defer { replace_tclobj(&errorcode,	NULL); };
		Tcl_Obj*	errormsg = NULL;	defer { replace_tclobj(&errormsg,	NULL); };

		if (!compileerror_code) replace_tclobj(&compileerror_code, l->lit[LIT_BLANK]);

		replace_tclobj(&cmd[0], l->lit[LIT_COMPILEERROR]);
		replace_tclobj(&cmd[1], compileerror_code);
		replace_tclobj(&cmd[2], compile_errors);
		if (extra_errormsg) {
			replace_tclobj(&cmd[3], extra_errormsg);
			replace_tclobj(&cmd[4], Tcl_GetReturnOptions(interp, TCL_OK));
		}
		TEST_OK(Tcl_EvalObjv(interp, cmdc, cmd, TCL_EVAL_DIRECT | TCL_EVAL_GLOBAL));
		replace_tclobj(&res, Tcl_GetObjResult(interp));

		Tcl_Obj**	resv;
		Tcl_Size	resc;
		TEST_OK(Tcl_ListObjGetElements(interp, res, &resc, &resv));
		replace_tclobj(&errorcode, resv[0]);
		replace_tclobj(&errormsg,  resv[1]);

		(void)Tcl_RestoreInterpState(interp, state); state = NULL;
		Tcl_SetObjErrorCode(interp, errorcode);
		Tcl_SetObjResult(interp, errormsg);
		return TCL_ERROR;
	}
#undef CHECK_TCC
#undef CHECK_TCC_MACRO
#undef CHECK_TCC_PRINTF
#undef CHECK_TCC_SIMPLE
}

//}}}
int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct jitc_intrep** rPtr) //{{{
{
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &jitc_objtype);
	struct jitc_intrep*	r = NULL;

	if (ir == NULL) {
		struct interp_cx*	l = Tcl_GetAssocData(interp, "jitc", NULL);
		Tcl_ObjInternalRep	newir = {};

		TEST_OK(compile(interp, obj, l, (struct jitc_intrep **)&newir.twoPtrValue.ptr1));

		struct jitc_instance* instance = ckalloc(sizeof *instance);
		*instance = (struct jitc_instance){
			.next	= l->instance_head.next,
			.prev	= &l->instance_head,
			.obj	= obj
		};
		l->instance_head.next = instance;
		instance->next->prev = instance;

		newir.twoPtrValue.ptr2 = instance;

		//Tcl_FreeInternalRep(obj);
		Tcl_StoreInternalRep(obj, &jitc_objtype, &newir);
		ir = Tcl_FetchInternalRep(obj, &jitc_objtype);
	}

	r = ir->twoPtrValue.ptr1;
	if (r == NULL) {
		// Duplicated intrep, recompile from the cdef copy
		struct interp_cx*	l = Tcl_GetAssocData(interp, "jitc", NULL);
		TEST_OK(compile(interp, (Tcl_Obj*)ir->twoPtrValue.ptr2, l, &r));
		replace_tclobj((Tcl_Obj**)&ir->twoPtrValue.ptr2, NULL);
	}

	*rPtr = r;

	return TCL_OK;
}

//}}}
static void free_interp_cx(ClientData cdata, Tcl_Interp* interp) //{{{
{
	struct interp_cx*	l = cdata;

	while (l->instance_head.next != &l->instance_tail) {
		struct jitc_instance*	instance = l->instance_head.next;

		if (!Tcl_HasStringRep(instance->obj)) Tcl_GetString(instance->obj);	// Regenerate the string rep
		Tcl_FreeInternalRep(instance->obj);									// Free the intrep (which references pointers we're about to invalidate by unloading our lib)
	}

	for (int i=0; i<LIT_SIZE; i++)
		replace_tclobj(&l->lit[i], NULL);

#if STUBSMODE
	replace_tclobj(&l->tclstublib, NULL);
#else
	replace_tclobj(&l->tcllib, NULL);
#endif
	replace_tclobj(&l->tclver, NULL);

	ckfree(l);
	l = NULL;
}

//}}}
int pkgdir_path(Tcl_Interp* interp, const char* tail, Tcl_Obj** res) //{{{
{
	Tcl_Obj*	tailobj = NULL;			defer { replace_tclobj(&tailobj, NULL); };
	Tcl_MutexLock(&g_pkgdir_mutex);		defer { Tcl_MutexUnlock(&g_pkgdir_mutex); };

	if (!g_pkgdir) THROW_ERROR("Package directory not set.");

	replace_tclobj(&tailobj,	Tcl_NewStringObj(tail, -1));
	replace_tclobj(res,			Tcl_FSJoinToPath(g_pkgdir, 1, &tailobj));

	return TCL_OK;
}

//}}}
// Internal API }}}
// Stubs API {{{
int Jitc_GetSymbolFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj* symbol, void** val) //{{{
{
	struct jitc_intrep*	r = NULL;

	TEST_OK(get_r_from_obj(interp, cdef, &r));

	const char*	symstr = Tcl_GetString(symbol);
	*val = tcc_get_symbol(r->tcc, symstr);
	if (*val == NULL) {
		Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "LOAD_SYMBOL", symstr, NULL);
		Tcl_SetObjResult(interp, Tcl_ObjPrintf("cannot find symbol \"%s\"", symstr));
		return TCL_ERROR;
	}

	return TCL_OK;
}

//}}}
int Jitc_GetSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** symbols) //{{{
{
	struct jitc_intrep*	r = NULL;
	Tcl_Obj*			lsymbols = NULL;	defer { replace_tclobj(&lsymbols, NULL); };
	int					done;

	TEST_OK(get_r_from_obj(interp, cdef, &r));

	replace_tclobj(&lsymbols, Tcl_NewListObj(0, NULL));

	Tcl_Obj*			k = NULL;
	Tcl_Obj*			v = NULL;
	Tcl_DictSearch		search;
	TEST_OK(Tcl_DictObjFirst(interp, r->symbols, &search, &k, &v, &done));
	defer { Tcl_DictObjDone(&search); };
	while (!done) {
		TEST_OK(Tcl_ListObjAppendElement(interp, lsymbols, k));
		Tcl_DictObjNext(&search, &k, &v, &done);
	}

	replace_tclobj(symbols, lsymbols);

	return TCL_OK;
}

//}}}
int Jitc_GetExportHeadersFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** headers) //{{{
{
	struct jitc_intrep*	r = NULL;

	TEST_OK(get_r_from_obj(interp, cdef, &r));

	replace_tclobj(headers, r->exported_headers);

	return TCL_OK;
}

//}}}
int Jitc_GetExportSymbolsFromObj(Tcl_Interp* interp, Tcl_Obj* cdef, Tcl_Obj** symbols) //{{{
{
	struct jitc_intrep*	r = NULL;

	TEST_OK(get_r_from_obj(interp, cdef, &r));

	replace_tclobj(symbols, r->exported_symbols);

	return TCL_OK;
}

//}}}
// Stubs API }}}
// Script API {{{
static int capply_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	enum {A_cmd, A_CDEF, A_SYMBOL, A_args};
	CHECK_MIN_ARGS("cdef symbol ?arg ...?");

	Tcl_ObjCmdProc*	proc = NULL;
	TEST_OK(Jitc_GetSymbolFromObj(interp, objv[1], objv[2], (void**)&proc));
	return (proc)(NULL, interp, objc-2, objv+2);
}

//}}}
static int nrapply_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	enum {A_cmd, A_CDEF, A_SYMBOL, A_args};
	CHECK_MIN_ARGS("cdef symbol ?arg ...?");

	Tcl_ObjCmdProc*	proc = NULL;
	TEST_OK(Jitc_GetSymbolFromObj(interp, objv[1], objv[2], (void**)&proc));
	return (proc)(NULL, interp, objc-2, objv+2);
}

//}}}
static int nrapply_cmd_setup(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	return Tcl_NRCallObjProc(interp, nrapply_cmd, cdata, objc, objv);
}

//}}}
static int _bind_invoke_curried(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	struct proc_binding*	binding = cdata;
	constexpr Tcl_Size		static_args_space = 10;
	Tcl_Obj*				o_static[static_args_space];
	Tcl_Obj**				ov = o_static;	defer { if (ov != o_static) ckfree(ov); };
	Tcl_Size				oc = 0;
	Tcl_Obj**				cv = NULL;
	Tcl_Size				cc, arg = 0;

	TEST_OK(Tcl_ListObjGetElements(interp, binding->curryargs, &cc, &cv));
	oc = cc + objc;
	if (oc > static_args_space)
		ov = ckalloc(sizeof(Tcl_Obj*) * oc);

	ov[arg++] = objv[0];
	for (Tcl_Size i=0; i<cc; i++)	ov[arg++] = cv[i];
	for (int i=1; i<objc; i++)		ov[arg++] = objv[i];

	return (binding->resolved)(NULL, interp, oc, ov);
}

//}}}
static int _bind_invoke_curried_setup(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	return Tcl_NRCallObjProc(interp, _bind_invoke_curried, cdata, objc, objv);
}

//}}}
static int _bind_invoke_setup(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	struct proc_binding*	binding = cdata;
	return Tcl_NRCallObjProc(interp, binding->resolved, cdata, objc, objv);
}

//}}}
static void _unbind(ClientData cdata) //{{{
{
	struct proc_binding*	binding = cdata;

	replace_tclobj(&binding->cdef, NULL);
	replace_tclobj(&binding->symbol, NULL);
	replace_tclobj(&binding->curryargs, NULL);
	binding->resolved = NULL;
	ckfree(binding);
	binding = NULL;
}

//}}}
static int bind_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	enum {A_cmd, A_NAME, A_CDEF, A_SYMBOL, A_args};
	CHECK_MIN_ARGS("name cdef symbol ?curryarg ...?");

	struct proc_binding*	binding = ckalloc(sizeof *binding);
	*binding = (struct proc_binding){};
	defer {
		if (binding) {
			replace_tclobj(&binding->cdef, NULL);
			replace_tclobj(&binding->symbol, NULL);
			replace_tclobj(&binding->curryargs, NULL);
			ckfree(binding);
		}
	}

	replace_tclobj(&binding->cdef,   objv[A_CDEF]);
	replace_tclobj(&binding->symbol, objv[A_SYMBOL]);
	TEST_OK(Jitc_GetSymbolFromObj(interp, objv[A_CDEF], objv[A_SYMBOL], (void**)&binding->resolved));
	if (objc > A_args) {
		replace_tclobj(&binding->curryargs, Tcl_NewListObj(objc-A_args, objv+A_args));
		if (Tcl_NRCreateCommand(interp, Tcl_GetString(objv[A_NAME]), _bind_invoke_curried_setup, _bind_invoke_curried, binding, _unbind) == NULL)
			THROW_ERROR("Failed to create command");
	} else {
		if (Tcl_NRCreateCommand(interp, Tcl_GetString(objv[A_NAME]), _bind_invoke_setup, binding->resolved, binding, _unbind) == NULL)
			THROW_ERROR("Failed to create command");
	}

	binding = NULL;	// Hand over to cmd registration, will be freed by _unbind

	return TCL_OK;
}

//}}}
static int symbols_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	enum {A_cmd, A_CDEF, A_objc};
	CHECK_ARGS("cdef");

	Tcl_Obj*		symbols = NULL;		defer { replace_tclobj(&symbols, NULL); };
	TEST_OK(Jitc_GetSymbolsFromObj(interp, objv[1], &symbols));
	Tcl_SetObjResult(interp, symbols);

	return TCL_OK;
}

//}}}
static int mkdtemp_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	char*     template = NULL;

	enum {A_cmd, A_TEMPLATE, A_objc};
	CHECK_ARGS("template");

	template = strdup(Tcl_GetString(objv[A_TEMPLATE]));		defer { free(template); };

	char* dir = mkdtemp(template);
	if (dir == NULL) {
		int			err = Tcl_GetErrno();
		const char*	errstr = Tcl_ErrnoId();

		if (err == EINVAL)
			THROW_ERROR("Template must end with XXXXXX");
		Tcl_SetErrorCode(interp, "POSIX", errstr, Tcl_ErrnoMsg(err), NULL);
		THROW_ERROR("Could not create temporary directory: ", Tcl_ErrnoMsg(err));
	}
	Tcl_SetObjResult(interp, Tcl_NewStringObj(dir, -1));

	return TCL_OK;
}

//}}}

#define NS	"::jitc"
static struct cmd {
	char*			name;
	Tcl_ObjCmdProc*	proc;
	Tcl_ObjCmdProc*	nrproc;
} cmds[] = {
	{NS "::capply",		nrapply_cmd_setup,	capply_cmd},
	{NS "::bind",		bind_cmd,			NULL},
	{NS "::symbols",	symbols_cmd,		NULL},
	{NS "::mkdtemp",	mkdtemp_cmd,		NULL},
	{NULL,				NULL,				NULL}
};
// Script API }}}

extern const JitcStubs* const jitcConstStubsPtr;

#ifdef __cplusplus
extern "C" {
#endif
DLLEXPORT int Jitc_Init(Tcl_Interp* interp) //{{{
{
#if USE_TCL_STUBS
	if (!Tcl_InitStubs(interp, TCL_VERSION, 0)) return TCL_ERROR;
#endif

	if (!g_pkgdir) {
		Tcl_MutexLock(&g_pkgdir_mutex);		defer { Tcl_MutexUnlock(&g_pkgdir_mutex); };
		if (!g_pkgdir) {
			TEST_OK(Tcl_EvalEx(interp, "file dirname [file normalize [info script]]", -1, 0));

			replace_tclobj(&g_pkgdir, Tcl_DuplicateObj(Tcl_GetObjResult(interp)));
			// Paranoia: force our copy to be an unshared pure string
			Tcl_GetString(g_pkgdir);
			Tcl_FreeInternalRep(g_pkgdir);
			Tcl_ResetResult(interp);
		}
	}

	{
		Tcl_Obj*	script_fn = NULL;	defer { replace_tclobj(&script_fn, NULL); };
		TEST_OK(pkgdir_path(interp, "jitc.tcl", &script_fn));
		TEST_OK(Tcl_EvalFile(interp, Tcl_GetString(script_fn)));
	}

	//Tcl_Namespace*	ns = NULL;
	//ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	//TEST_OK(Tcl_Export(interp, ns, "*", 0));

	// Set up interp_cx {{{
	struct interp_cx*	l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){};
	Tcl_SetAssocData(interp, "jitc", free_interp_cx, l);
	defer { if (l) Tcl_DeleteAssocData(interp, "jitc"); };

	for (int i=0; i<LIT_SIZE; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(lit_str[i], -1));


#if STUBSMODE
	TEST_OK(Tcl_EvalObjEx(interp, l->lit[LIT_TCLSTUBLIB_CMD], 0));
	replace_tclobj(&l->tclstublib, Tcl_GetObjResult(interp));
#else
	TEST_OK(Tcl_EvalObjEx(interp, l->lit[LIT_TCLLIB_CMD], 0));
	replace_tclobj(&l->tcllib, Tcl_GetObjResult(interp));
#endif
	TEST_OK(Tcl_EvalObjEx(interp, l->lit[LIT_TCLVER_CMD], 0));
	replace_tclobj(&l->tclver, Tcl_GetObjResult(interp));

	l->instance_head.next = &l->instance_tail;
	l->instance_tail.prev = &l->instance_head;
	// Set up interp_cx }}}

	for (struct cmd* c = cmds; c->name; c++) {
		Tcl_Command r = NULL;

		if (c->nrproc)	r = Tcl_NRCreateCommand(interp, c->name, c->proc, c->nrproc, l, NULL);
		else			r = Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL);

		if (!r) {
			Tcl_SetObjResult(interp, Tcl_ObjPrintf("Could not create command %s", c->name));
			return TCL_ERROR;
		}
	}

	TEST_OK(Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION, jitcConstStubsPtr));

	l = NULL;

	return TCL_OK;
}

//}}}
DLLEXPORT int Jitc_Unload(Tcl_Interp* interp, int flags) //{{{
{
	Tcl_DeleteAssocData(interp, "jitc");	// Have to do this here, otherwise Tcl will try to call it after we're unloaded
	if (flags == TCL_UNLOAD_DETACH_FROM_PROCESS) {
		//fprintf(stderr, "jitc unloading, finalizing mutexes\n");
		Tcl_MutexFinalize(&gdb_jit_mutex);
		Tcl_MutexFinalize(&g_tcc_mutex);

		{
			Tcl_MutexLock(&g_pkgdir_mutex);		defer { Tcl_MutexUnlock(&g_pkgdir_mutex); };
			replace_tclobj(&g_pkgdir, NULL);
		}
		Tcl_MutexFinalize(&g_pkgdir_mutex);
	} else {
		//fprintf(stderr, "jitc detaching from interp\n");
		// TODO: remove commands
	}

	names_shutdown();

	return TCL_OK;
}

//}}}
#ifdef __cplusplus
}
#endif
