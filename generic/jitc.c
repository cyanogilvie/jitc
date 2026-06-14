#include <jitcInt.h>
#include <tip445.h>
#include <sys/stat.h>
#include <names.h>

TCL_DECLARE_MUTEX(g_pkgdir_mutex)
static Tcl_Obj* g_pkgdir = NULL;

// Serializes the whole compile pipeline. libslimcc keeps global compiler state
// (it's the stock slimcc front-end with a reset-between-runs model, not a
// context struct), so concurrent slimcc_compile() calls are unsafe; this mutex
// also guards the file-scope link-error longjmp target below.
TCL_DECLARE_MUTEX(g_compile_mutex)

typedef const char* (cdef_initstubs)(Tcl_Interp* interp, const char* ver);
typedef int (cdef_init)(Tcl_Interp* interp);
typedef int (cdef_release)(Tcl_Interp* interp);

static void free_jitc_internal_rep(Tcl_Obj* obj);
static void dup_jitc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup);
static void update_jitc_string_rep(Tcl_Obj* obj);
static void jitc_free_backend(struct jitc_intrep* r);

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

	if (r->ctx) {
		// Run the cdef's release() (if any) while its code is still mapped.
		if (r->symbols && r->interp) {
			struct interp_cx*	l = Tcl_GetAssocData(r->interp, "jitc", NULL);
			Tcl_Obj*	releasename = NULL;
			Tcl_Obj*	releasesymboladdr = NULL;

			// l can be NULL here if we're here because the interp is being deleted (and so free_interp_cx has been called)
			replace_tclobj(&releasename, l ? l->lit[LIT_RELEASE] : Tcl_NewStringObj("release", -1));
			if (TCL_OK == Tcl_DictObjGet(r->interp, r->symbols, releasename, &releasesymboladdr) && releasesymboladdr) {
				cdef_release*	release = NULL;
				void*			addr = jitc_get_symbol(r, "release");
				memcpy(&release, &addr, sizeof release);
				if (release) (release)(r->interp);
			}
			replace_tclobj(&releasename, NULL);
		}

		Tcl_MutexLock(&g_compile_mutex);
		jitc_free_backend(r);	// MIR_finish frees all the code; dlclose libraries
		Tcl_MutexUnlock(&g_compile_mutex);
	}

	replace_tclobj(&r->symbols, NULL);

	r->interp = NULL;
	replace_tclobj(&r->cdef, NULL);
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
// MIR link/codegen glue {{{

// MIR_link()'s import resolver has no client-data argument, but it doesn't
// need one: every symbol the cdef defines itself, imports from a sibling cdef
// (via `use`/`symbols`) or pulls from the slimcc runtime is registered with
// MIR_load_external() before linking and never reaches here. What's left are
// genuinely external symbols — libc, the Tcl core already loaded in this
// process, and any package libraries we dlopen()'d with RTLD_GLOBAL — all
// reachable through the global symbol scope. An unresolved import is not a
// compile-time error (MIR can't tell whether it's ever reached), so it
// resolves to a trap that fires only if the code actually calls it.
static void unresolved_symbol_trap(void) //{{{
{
	Tcl_Panic("jitc: call to a symbol that could not be resolved in JIT'd code");
}

//}}}
static void* import_resolver(const char* name) //{{{
{
	void*	addr = dlsym(RTLD_DEFAULT, name);
	// dlsym already hands back a function as void* (POSIX guarantees the round
	// trip); the trap fallback needs the same conversion, which -Wpedantic
	// flags as an ISO C function->object pointer cast. Deliberate and portable.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	return addr ? addr : (void*)unresolved_symbol_trap;
#pragma GCC diagnostic pop
}

//}}}

// MIR raises link/codegen errors (duplicate definitions, malformed modules
// migrated in, ...) through a context error function whose default aborts the
// process. Route them into the compile-error path instead. Only touched while
// holding g_compile_mutex, so the file-scope longjmp target is safe.
static jmp_buf		g_link_jmp;
static Tcl_Obj**	g_link_errors = NULL;	// where the active compile collects MIR diagnostics

static void MIR_NO_RETURN link_error_func(MIR_error_type_t error_type, const char* format, ...) //{{{
{
	char	buf[1024];
	va_list	ap;
	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	va_end(ap);

	if (g_link_errors) {
		if (*g_link_errors == NULL)
			replace_tclobj(g_link_errors, Tcl_ObjPrintf("MIR error %d: %s", (int)error_type, buf));
		else
			Tcl_AppendStringsToObj(*g_link_errors, "\nMIR error: ", buf, NULL);
	}
	longjmp(g_link_jmp, 1);
}

//}}}
void* jitc_get_symbol(struct jitc_intrep* r, const char* name) //{{{
{
	Tcl_Obj*	key = NULL;		defer { replace_tclobj(&key, NULL); };
	Tcl_Obj*	val = NULL;
	Tcl_WideInt	w;

	if (!r->symbols) return NULL;
	replace_tclobj(&key, Tcl_NewStringObj(name, -1));
	if (TCL_OK != Tcl_DictObjGet(NULL, r->symbols, key, &val) || val == NULL) return NULL;
	if (TCL_OK != Tcl_GetWideIntFromObj(NULL, val, &w)) return NULL;
	return (void*)(intptr_t)w;
}

//}}}
// Walk every item of every module loaded into the cdef's context and record
// name -> address for each defined function and named data/bss object, the
// analog of tcc_list_symbols(). Addresses are final after MIR_link() with the
// eager gen interface (item->addr is the callable code / loaded data); a func
// whose addr wasn't filled is generated on demand.
static void build_symbols_dict(struct jitc_intrep* r) //{{{
{
	replace_tclobj(&r->symbols, Tcl_NewDictObj());

	for (
		MIR_module_t m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(r->ctx));
		m; m = DLIST_NEXT(MIR_module_t, m)
	) {
		for (
			MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items);
			it; it = DLIST_NEXT(MIR_item_t, it)
		) {
			const char*	name = NULL;
			void*		addr = it->addr;

			switch (it->item_type) {
				case MIR_func_item:
					name = it->u.func->name;
					if (!addr) addr = MIR_gen(r->ctx, it);
					break;
				case MIR_data_item:	name = it->u.data->name;	break;
				case MIR_bss_item:	name = it->u.bss->name;		break;
				default: continue;
			}
			if (!name || !addr) continue;

			Tcl_Obj*	k = NULL;	defer { replace_tclobj(&k, NULL); };
			Tcl_Obj*	v = NULL;	defer { replace_tclobj(&v, NULL); };
			replace_tclobj(&k, Tcl_NewStringObj(name, -1));
			replace_tclobj(&v, Tcl_NewWideIntObj((Tcl_WideInt)(intptr_t)addr));
			Tcl_DictObjPut(NULL, r->symbols, k, v);
		}
	}
}

//}}}
// Load the compiled modules into the cdef's context and generate machine code.
// MIR's error function is noreturn (it aborts the process by default); a setjmp
// here converts a link/codegen failure (duplicate definition, malformed module)
// into a TCL_ERROR carrying the message in *errors. Kept in its own small frame
// so setjmp's clobber hazard (and -Wclobbered) stays out of compile()'s large
// frame; only the unmodified parameters are live across the setjmp.
static int jitc_finish_link(struct jitc_intrep* r, MIR_module_t* mods, int nmods, Tcl_Obj** errors) //{{{
{
	g_link_errors = errors;
	if (setjmp(g_link_jmp)) { g_link_errors = NULL; return TCL_ERROR; }

	for (int i=0; i<nmods; i++)
		MIR_load_module(r->ctx, mods[i]);

	MIR_gen_init(r->ctx);
	r->gen_inited = 1;
	// MIR's levels: 0 fast RA, 1 +combiner, 2 +GVN/CCP (MIR's own default), 3+
	// everything. jitc's workload is dominated by re2c-generated lexers, which
	// are branch/frontend-bound: -O2/-O3 produce ~14% fewer instructions but the
	// same cycle count, so the extra compile latency (the GVN/CCP pass roughly
	// doubles codegen time) buys no runtime. Default to 1 — keeps register
	// allocation + the combiner (so compute-bound cdefs aren't pessimized) at
	// near-O0 compile cost. An explicit options -O<n> overrides this.
	MIR_gen_set_optimize_level(r->ctx, r->opt_level >= 0 ? (unsigned)r->opt_level : 1u);
	MIR_link(r->ctx, MIR_set_gen_interface, import_resolver);

	build_symbols_dict(r);

	g_link_errors = NULL;
	return TCL_OK;
}

//}}}
// Tear down a cdef's JIT state: free the machine code (MIR_finish frees all
// modules + generated code in the context at once) and drop the library
// handles its code resolved against. Safe to call on a partially-built intrep.
static void jitc_free_backend(struct jitc_intrep* r) //{{{
{
	if (r->ctx) {
		if (r->gen_inited) MIR_gen_finish(r->ctx);
		MIR_finish(r->ctx);
		r->ctx = NULL;
		r->gen_inited = 0;
	}
	// dlclose after the code that referenced these libraries is gone.
	for (int i=0; i<r->n_dlhandles; i++)
		if (r->dlhandles[i]) dlclose(r->dlhandles[i]);
	if (r->dlhandles) { ckfree(r->dlhandles); r->dlhandles = NULL; }
	r->n_dlhandles = 0;
}

//}}}
// Make a library's symbols resolvable by JIT'd code: dlopen it into the global
// scope (so import_resolver's dlsym(RTLD_DEFAULT) finds it) and keep the handle
// for teardown. Tries the bare SONAME first, then lib<name>.so under each
// supplied library path. Best-effort: a miss isn't fatal (an actually-needed
// symbol still traps at call time) — it mirrors the loader's own search.
static void dlopen_library(struct jitc_intrep* r, const char* name, Tcl_Obj* libpaths) //{{{
{
	void*	h = NULL;

	if (strchr(name, '/') || strstr(name, ".so")) {
		h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
	} else {
		Tcl_Obj*	soname = NULL;	defer { replace_tclobj(&soname, NULL); };
		replace_tclobj(&soname, Tcl_ObjPrintf("lib%s.so", name));
		h = dlopen(Tcl_GetString(soname), RTLD_NOW | RTLD_GLOBAL);

		if (!h && libpaths) {
			Tcl_Obj**	pv; Tcl_Size pc;
			if (TCL_OK == Tcl_ListObjGetElements(NULL, libpaths, &pc, &pv)) {
				for (Tcl_Size i=0; !h && i<pc; i++) {
					Tcl_Obj*	cand = NULL;	defer { replace_tclobj(&cand, NULL); };
					replace_tclobj(&cand, Tcl_ObjPrintf("%s/lib%s.so", Tcl_GetString(pv[i]), name));
					h = dlopen(Tcl_GetString(cand), RTLD_NOW | RTLD_GLOBAL);
				}
			}
		}
	}
	if (!h) return;

	r->dlhandles = ckrealloc(r->dlhandles, sizeof(void*) * (r->n_dlhandles + 1));
	r->dlhandles[r->n_dlhandles++] = h;
}

//}}}
// MIR link/codegen glue }}}

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
#if STUBSMODE
	"return -level 0 tclstub[if {![package vsatisfies [info tclversion] 9.0-]} {info tclversion}]",
#else
	"return -level 0 tcl[info tclversion]",
#endif
	"info tclversion",
	NULL
};

// Append the C compiler's system <...> include search dirs (detected at build
// time, see meson.build) to a list, so JIT'd code can find <stdio.h> etc.
// libslimcc ships only the freestanding headers and configures no system paths.
static void append_sys_includes(Tcl_Interp* interp, Tcl_Obj* list) //{{{
{
#ifdef JITC_SYS_INCLUDES
	const char*	dirs = JITC_SYS_INCLUDES;
	const char*	p = dirs;
	while (*p) {
		const char*	colon = strchr(p, ':');
		Tcl_Size	len = colon ? (Tcl_Size)(colon - p) : (Tcl_Size)strlen(p);
		if (len) {
			Tcl_Obj*	d = NULL;	defer { replace_tclobj(&d, NULL); };
			replace_tclobj(&d, Tcl_NewStringObj(p, len));
			Tcl_ListObjAppendElement(interp, list, d);
		}
		if (!colon) break;
		p = colon + 1;
	}
#endif
}

//}}}
// Read an entire file (through the Tcl VFS) into a fresh string object.
static int slurp_file(Tcl_Interp* interp, Tcl_Obj* path, Tcl_Obj** out) //{{{
{
	Tcl_Channel	ch = Tcl_FSOpenFileChannel(interp, path, "r", 0);
	if (ch == NULL) return TCL_ERROR;
	Tcl_Obj*	content = NULL;		defer { replace_tclobj(&content, NULL); };
	replace_tclobj(&content, Tcl_NewObj());
	if (Tcl_ReadChars(ch, content, -1, 0) < 0) {
		Tcl_Obj*	msg = Tcl_ObjPrintf("Error reading \"%s\": %s", Tcl_GetString(path), Tcl_PosixError(interp));
		Tcl_Close(NULL, ch);
		Tcl_SetObjResult(interp, msg);
		return TCL_ERROR;
	}
	Tcl_Close(NULL, ch);
	replace_tclobj(out, content);
	return TCL_OK;
}

//}}}
// Pull -I<dir>, -D<name>[=val] and -O<n> out of a tcc-style options string:
// the first two feed slimcc's preprocessor, -O<n> sets the MIR codegen
// optimization level (*opt_level, last one wins). Everything else (warning
// flags, -g, linker options, ...) has no equivalent and is silently ignored.
static int parse_options_string(Tcl_Interp* interp, const char* opts, Tcl_Obj* inc_list, Tcl_Obj* def_list, int* opt_level) //{{{
{
	Tcl_Obj*	toks = NULL;	defer { replace_tclobj(&toks, NULL); };
	Tcl_Obj**	tv; Tcl_Size tc;

	replace_tclobj(&toks, Tcl_NewStringObj(opts, -1));
	TEST_OK(Tcl_ListObjGetElements(interp, toks, &tc, &tv));
	for (Tcl_Size i=0; i<tc; i++) {
		const char*	t = Tcl_GetString(tv[i]);
		if (strncmp(t, "-I", 2) == 0 && t[2]) {
			Tcl_Obj*	d = NULL;	defer { replace_tclobj(&d, NULL); };
			replace_tclobj(&d, Tcl_NewStringObj(t+2, -1));
			TEST_OK(Tcl_ListObjAppendElement(interp, inc_list, d));
		} else if (strncmp(t, "-D", 2) == 0 && t[2]) {
			Tcl_Obj*	d = NULL;	defer { replace_tclobj(&d, NULL); };
			replace_tclobj(&d, Tcl_NewStringObj(t+2, -1));
			TEST_OK(Tcl_ListObjAppendElement(interp, def_list, d));
		} else if (strncmp(t, "-O", 2) == 0) {
			// -O<n> selects the MIR codegen optimization level. Bare -O means
			// -O1 (gcc convention); -Os/-Oz/-Ofast aren't MIR levels, ignore.
			const char*	a = t+2;
			if (*a == '\0') {
				*opt_level = 1;
			} else if (a[1] == '\0' && *a >= '0' && *a <= '9') {
				*opt_level = *a - '0';
			}
		}
		// -U<name> has no libslimcc API equivalent; ignore.
	}
	return TCL_OK;
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
	int			opt_level = -1;	// MIR codegen -O<n>; -1 = MIR's default (set via options -O<n>)

	// Accumulated compiler inputs (gathered with no compile lock held, since
	// resolving sibling-cdef symbols may recursively compile them).
	Tcl_Obj*	inc_list     = NULL;	defer { replace_tclobj(&inc_list,     NULL); };
	Tcl_Obj*	def_list     = NULL;	defer { replace_tclobj(&def_list,     NULL); };
	Tcl_Obj*	libpath_list = NULL;	defer { replace_tclobj(&libpath_list, NULL); };
	Tcl_Obj*	lib_list     = NULL;	defer { replace_tclobj(&lib_list,     NULL); };
	Tcl_Obj*	host_syms    = NULL;	defer { replace_tclobj(&host_syms,    NULL); };	// name, addr, name, addr, ...
	Tcl_Obj*	code_list    = NULL;	defer { replace_tclobj(&code_list,    NULL); };	// assembled source strings
	Tcl_Obj*	file_list    = NULL;	defer { replace_tclobj(&file_list,    NULL); };	// PART_FILE paths

	Tcl_Obj*	filter = NULL;				defer { replace_tclobj(&filter,            NULL); };
	Tcl_Obj*	exported_headers = NULL;	defer { replace_tclobj(&exported_headers,  NULL); };
	Tcl_Obj*	exported_symbols = NULL;	defer { replace_tclobj(&exported_symbols,  NULL); };
	Tcl_Obj*	compileerror_code = NULL;	defer { replace_tclobj(&compileerror_code, NULL); };
	Tcl_Obj*	compile_errors = NULL;		defer { replace_tclobj(&compile_errors,    NULL); };
	Tcl_Obj*	extra_errormsg = NULL;		defer { replace_tclobj(&extra_errormsg,    NULL); };
	Tcl_Obj*	used = NULL;				defer { replace_tclobj(&used,              NULL); };

	Tcl_DString	preamble; Tcl_DStringInit(&preamble);	defer { Tcl_DStringFree(&preamble); };

	replace_tclobj(&inc_list,     Tcl_NewListObj(0, NULL));
	replace_tclobj(&def_list,     Tcl_NewListObj(0, NULL));
	replace_tclobj(&libpath_list, Tcl_NewListObj(0, NULL));
	replace_tclobj(&lib_list,     Tcl_NewListObj(0, NULL));
	replace_tclobj(&host_syms,    Tcl_NewListObj(0, NULL));
	replace_tclobj(&code_list,    Tcl_NewListObj(0, NULL));
	replace_tclobj(&file_list,    Tcl_NewListObj(0, NULL));

	struct jitc_intrep*	r = NULL;
	defer {
		if (r) {
			jitc_free_backend(r);
			replace_tclobj(&r->symbols,          NULL);
			replace_tclobj(&r->cdef,             NULL);
			replace_tclobj(&r->used,             NULL);
			replace_tclobj(&r->exported_symbols, NULL);
			replace_tclobj(&r->exported_headers, NULL);
			r->interp = NULL;
			ckfree(r);
		}
	}

	TEST_OK(Tcl_ListObjGetElements(interp, cdef, &oc, &ov));
	if (oc % 2 == 1)
		THROW_PRINTF("cdef must be a list with an even number of elements (got %" TCL_SIZE_MODIFIER "d): %s", oc, Tcl_GetString(cdef));

	// Pass 1: mode (it gates the Tcl preamble + default include/library paths).
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		if ((enum partenum)partidx == PART_MODE) {
			int	modeidx;
			TEST_OK(Tcl_GetIndexFromObj(interp, ov[i+1], modes, "mode", TCL_EXACT, &modeidx));
			mode = modeidx;
		}
	}

	// Default include + library search paths from the package's configured
	// lists, plus (for finding <stdio.h> etc.) the host compiler's system
	// include dirs. libslimcc supplies only freestanding headers itself.
	{
		Tcl_Obj*	includepath = NULL;	defer { replace_tclobj(&includepath, NULL); };
		Tcl_Obj*	librarypath = NULL;	defer { replace_tclobj(&librarypath, NULL); };
		Tcl_Obj**	pv; Tcl_Size pc;

		replace_tclobj(&includepath, Tcl_ObjGetVar2(interp, l->lit[LIT_INCLUDEPATH_VAR], NULL, TCL_LEAVE_ERR_MSG));
		if (includepath == NULL) return TCL_ERROR;
		replace_tclobj(&librarypath, Tcl_ObjGetVar2(interp, l->lit[LIT_LIBRARYPATH_VAR], NULL, TCL_LEAVE_ERR_MSG));
		if (librarypath == NULL) return TCL_ERROR;

		TEST_OK(Tcl_ListObjGetElements(interp, includepath, &pc, &pv));
		for (Tcl_Size j=0; j<pc; j++) TEST_OK(Tcl_ListObjAppendElement(interp, inc_list, pv[j]));
		TEST_OK(Tcl_ListObjGetElements(interp, librarypath, &pc, &pv));
		for (Tcl_Size j=0; j<pc; j++) TEST_OK(Tcl_ListObjAppendElement(interp, libpath_list, pv[j]));
	}
	append_sys_includes(interp, inc_list);

	if (mode == MODE_TCL) {
#if STUBSMODE
		TEST_OK(Tcl_ListObjAppendElement(interp, def_list, Tcl_NewStringObj("USE_TCL_STUBS=1", -1)));
#endif
		Tcl_DStringAppend(&preamble, "#include <tclstuff.h>\n", -1);
	}

	// Pass 2: packages and `use` — these contribute headers (to the preamble),
	// include/library paths, libraries to dlopen, and sibling-cdef symbols to
	// resolve. Resolving sibling symbols may recursively compile them, so this
	// must happen before we take the compile lock.
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		enum partenum	part = partidx;
		Tcl_Obj*		v = ov[i+1];

		if (part == PART_PACKAGE) { //{{{
			Tcl_Size	pc;
			Tcl_Obj**	pv = NULL;
			Tcl_Obj*	cmd[3] = {};	defer { for (int k=0; k<3; k++) replace_tclobj(&cmd[k], NULL); };

			TEST_OK(Tcl_ListObjGetElements(interp, v, &pc, &pv));
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
			Tcl_Obj*	vals[KEY_END] = {};	defer { for (int k=0; k<KEY_END; k++) replace_tclobj(&vals[k], NULL); }

			for (int k=0; keys[k]; k++) {
				Tcl_InterpState	state = Tcl_SaveInterpState(interp, 0);
				replace_tclobj(&cmd[2], Tcl_NewStringObj(keys[k], -1));
				if (TCL_OK == Tcl_EvalObjv(interp, 3, cmd, TCL_EVAL_GLOBAL))
					replace_tclobj(&vals[k], Tcl_GetObjResult(interp));
				Tcl_RestoreInterpState(interp, state);
			}

			if (vals[KEY_HEADER]) {
				Tcl_DStringAppend(&preamble, "\n#include <", -1);
				Tcl_DStringAppend(&preamble, Tcl_GetString(vals[KEY_HEADER]), -1);
				Tcl_DStringAppend(&preamble, ">\n", -1);
			}
			if (vals[KEY_INCLUDEDIR_RUNTIME])
				TEST_OK(Tcl_ListObjAppendElement(interp, inc_list, vals[KEY_INCLUDEDIR_RUNTIME]));
			if (vals[KEY_INCLUDEDIR_INSTALL])
				TEST_OK(Tcl_ListObjAppendElement(interp, inc_list, vals[KEY_INCLUDEDIR_INSTALL]));
			if (vals[KEY_LIBDIR_RUNTIME])
				TEST_OK(Tcl_ListObjAppendElement(interp, libpath_list, vals[KEY_LIBDIR_RUNTIME]));
			if (vals[KEY_LIBDIR_INSTALL])
				TEST_OK(Tcl_ListObjAppendElement(interp, libpath_list, vals[KEY_LIBDIR_INSTALL]));
			if (vals[KEY_LIBRARY]) {
				const char* libstr = Tcl_GetString(vals[KEY_LIBRARY]);
				if (strncmp("lib", libstr, 3) == 0) libstr += 3;
				TEST_OK(Tcl_ListObjAppendElement(interp, lib_list, Tcl_NewStringObj(libstr, -1)));
			}
		} //}}}
		else if (part == PART_USE) { //{{{
			Tcl_Obj*	useobj = v;
			Tcl_Obj*	use_headers = NULL;	defer { replace_tclobj(&use_headers, NULL); };
			Tcl_Obj*	use_symbols = NULL;	defer { replace_tclobj(&use_symbols, NULL); };

			TEST_OK(Jitc_GetExportHeadersFromObj(interp, useobj, &use_headers));
			TEST_OK(Jitc_GetExportSymbolsFromObj(interp, useobj, &use_symbols));

			if (use_headers) {
				Tcl_Size	hl;
				const char*	hs = Tcl_GetStringFromObj(use_headers, &hl);
				Tcl_DStringAppend(&preamble, hs, hl);
			}
			if (use_symbols) {
				Tcl_Obj**	sv; Tcl_Size sc;
				if (!used) replace_tclobj(&used, Tcl_NewListObj(0, NULL));
				TEST_OK(Tcl_ListObjAppendElement(interp, used, useobj));
				TEST_OK(Tcl_ListObjGetElements(interp, use_symbols, &sc, &sv));
				for (Tcl_Size s=0; s<sc; s++) {
					void*	val = NULL;
					TEST_OK(Jitc_GetSymbolFromObj(interp, useobj, sv[s], &val));
					TEST_OK(Tcl_ListObjAppendElement(interp, host_syms, sv[s]));
					TEST_OK(Tcl_ListObjAppendElement(interp, host_syms, Tcl_NewWideIntObj((Tcl_WideInt)(intptr_t)val)));
				}
			}
		} //}}}
	}

	// Pass 3: exported headers (appended after `use` headers so they can build
	// on them) and the export declarations recorded on the intrep.
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		if ((enum partenum)partidx != PART_EXPORT) continue;

		Tcl_Obj**	ev = NULL; Tcl_Size ec;
		TEST_OK(Tcl_ListObjGetElements(interp, ov[i+1], &ec, &ev));
		for (Tcl_Size ei=0; ei<ec; ei+=2) {
			static const char* exportkeys[] = { "symbols", "header", NULL };
			enum exportkeyenum { EXPORT_SYMBOLS, EXPORT_HEADER } exportkey;
			int exportkeyidx;
			TEST_OK(Tcl_GetIndexFromObj(interp, ev[ei], exportkeys, "key", TCL_EXACT, &exportkeyidx));
			exportkey = exportkeyidx;
			switch (exportkey) {
				case EXPORT_SYMBOLS:
					replace_tclobj(&exported_symbols, ev[ei+1]);
					break;
				case EXPORT_HEADER: {
					Tcl_Size	hl;
					const char*	hs = Tcl_GetStringFromObj(ev[ei+1], &hl);
					replace_tclobj(&exported_headers, ev[ei+1]);
					Tcl_DStringAppend(&preamble, hs, hl);
					break;
				}
			}
		}
	}

	// Pass 4: everything else, in document order (so a `filter` applies to the
	// `code` blocks that follow it). The preamble is complete by now, so each
	// code block can be assembled with it prepended.
	for (i=0; i<oc; i+=2) {
		int	partidx;
		TEST_OK(Tcl_GetIndexFromObj(interp, ov[i], parts, "part", TCL_EXACT, &partidx));
		enum partenum	part = partidx;
		Tcl_Obj*		v = ov[i+1];

		switch (part) {
			case PART_MODE:
			case PART_DEBUG:		// no DWARF from MIR: debug paths are unsupported, ignored
			case PART_TCCPATH:		// libslimcc has no external lib-path concept
			case PART_UNDEFINE:		// no libslimcc API to undefine a macro
			case PART_PACKAGE:
			case PART_USE:
			case PART_EXPORT:
				break;

			case PART_OPTIONS:
				TEST_OK(parse_options_string(interp, Tcl_GetString(v), inc_list, def_list, &opt_level));
				break;

			case PART_INCLUDE_PATH:
			case PART_SYSINCLUDE_PATH:
				TEST_OK(Tcl_ListObjAppendElement(interp, inc_list, v));
				break;

			case PART_LIBRARY_PATH:
				TEST_OK(Tcl_ListObjAppendElement(interp, libpath_list, v));
				break;

			case PART_LIBRARY:
				TEST_OK(Tcl_ListObjAppendElement(interp, lib_list, v));
				break;

			case PART_DEFINE: {
				Tcl_Obj**	sv; Tcl_Size sc;
				TEST_OK(Tcl_ListObjGetElements(interp, v, &sc, &sv));
				if (sc < 1 || sc > 2)
					THROW_ERROR("Definition must be a list: name value: \"", Tcl_GetString(v), "\"");
				Tcl_Obj*	def = NULL;	defer { replace_tclobj(&def, NULL); };
				if (sc == 1)	replace_tclobj(&def, Tcl_DuplicateObj(sv[0]));
				else			replace_tclobj(&def, Tcl_ObjPrintf("%s=%s", Tcl_GetString(sv[0]), Tcl_GetString(sv[1])));
				TEST_OK(Tcl_ListObjAppendElement(interp, def_list, def));
				break;
			}

			case PART_SYMBOLS: { // sibling cdef + symbol names to link against
				Tcl_Obj**	sv; Tcl_Size sc;
				TEST_OK(Tcl_ListObjGetElements(interp, v, &sc, &sv));
				if (sc < 1)
					THROW_ERROR("Symbol definition must be a list: cdef symbol: \"", Tcl_GetString(v), "\"");
				if (sc >= 2) {
					if (!used) replace_tclobj(&used, Tcl_NewListObj(0, NULL));
					TEST_OK(Tcl_ListObjAppendElement(interp, used, sv[0]));
				}
				for (Tcl_Size s=1; s<sc; s++) {
					void*	val = NULL;
					TEST_OK(Jitc_GetSymbolFromObj(interp, sv[0], sv[s], &val));
					TEST_OK(Tcl_ListObjAppendElement(interp, host_syms, sv[s]));
					TEST_OK(Tcl_ListObjAppendElement(interp, host_syms, Tcl_NewWideIntObj((Tcl_WideInt)(intptr_t)val)));
				}
				break;
			}

			case PART_FILTER: {
				Tcl_Size	len;
				Tcl_GetStringFromObj(v, &len);
				replace_tclobj(&filter, len ? v : NULL);
				break;
			}

			case PART_FILE:
				TEST_OK(Tcl_ListObjAppendElement(interp, file_list, v));
				break;

			case PART_CODE: { //{{{
				Tcl_Size	len;
				const char*	str = Tcl_GetStringFromObj(v, &len);

				Tcl_DString	c; Tcl_DStringInit(&c);	defer { Tcl_DStringFree(&c); };
				Tcl_DStringAppend(&c, Tcl_DStringValue(&preamble), Tcl_DStringLength(&preamble));
				Tcl_DStringAppend(&c, str, len);

				if (filter) {
					Tcl_Obj*	in = NULL;			defer { replace_tclobj(&in,        NULL); };
					Tcl_Obj*	filtercmd = NULL;	defer { replace_tclobj(&filtercmd, NULL); };

					replace_tclobj(&filtercmd, Tcl_DuplicateObj(filter));
					replace_tclobj(&in, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
					TEST_OK(Tcl_ListObjAppendElement(interp, filtercmd, in));
					TEST_OK(Tcl_EvalObjEx(interp, filtercmd, 0));
					Tcl_DStringSetLength(&c, 0);
					Tcl_Size		fl;
					const char*		fs = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &fl);
					Tcl_DStringAppend(&c, fs, fl);
					Tcl_ResetResult(interp);
				}

				Tcl_Obj*	codeobj = NULL;	defer { replace_tclobj(&codeobj, NULL); };
				replace_tclobj(&codeobj, Tcl_NewStringObj(Tcl_DStringValue(&c), Tcl_DStringLength(&c)));
				TEST_OK(Tcl_ListObjAppendElement(interp, code_list, codeobj));
				break;
			} //}}}

			default:
				THROW_ERROR("Invalid part id");
		}
	}

#if STUBSMODE
	// In stubs mode the JIT'd code reaches Tcl through the stubs table; compile
	// a tiny bootstrap that calls Tcl_InitStubs (resolved + the table pointers
	// shared from this process below).
	if (mode == MODE_TCL)
		TEST_OK(Tcl_ListObjAppendElement(interp, code_list, Tcl_NewStringObj(
			"#include <tcl.h>\nconst char* _initstubs(Tcl_Interp* interp, const char* ver) {return Tcl_InitStubs(interp, ver, 0);}", -1)));
#endif

	// --- Everything is gathered; now the serialized compile/link/codegen. ---
	Tcl_MutexLock(&g_compile_mutex);
	defer { g_link_errors = NULL; Tcl_MutexUnlock(&g_compile_mutex); };

	r = ckalloc(sizeof *r);
	*r = (struct jitc_intrep){ .interp = interp, .ctx = MIR_init(), .opt_level = opt_level };
	slimcc_register_helpers(r->ctx);
	MIR_set_error_func(r->ctx, link_error_func);

	// Symbols this code links against: the slimcc runtime helpers (registered
	// above) plus host/sibling-cdef symbols. These take priority over the
	// import resolver.
	{
		Tcl_Obj**	hv; Tcl_Size hc;
		TEST_OK(Tcl_ListObjGetElements(interp, host_syms, &hc, &hv));
		for (Tcl_Size h=0; h+1<hc; h+=2) {
			Tcl_WideInt	w;
			TEST_OK(Tcl_GetWideIntFromObj(interp, hv[h+1], &w));
			MIR_load_external(r->ctx, Tcl_GetString(hv[h]), (void*)(intptr_t)w);
		}
	}
#if STUBSMODE
	MIR_load_external(r->ctx, "Tcl_InitStubs",      (void*)Tcl_InitStubs);
	MIR_load_external(r->ctx, "tclStubsPtr",        &tclStubsPtr);
	MIR_load_external(r->ctx, "tclPlatStubsPtr",    &tclPlatStubsPtr);
	MIR_load_external(r->ctx, "tclIntStubsPtr",     &tclIntStubsPtr);
	MIR_load_external(r->ctx, "tclIntPlatStubsPtr", &tclIntPlatStubsPtr);
#endif

	// slimcc_options: build flat char* arrays from the gathered lists. The
	// strings point into the Tcl_Obj string reps, which outlive the compile.
	Tcl_Obj**	incv; Tcl_Size incc;
	Tcl_Obj**	defv; Tcl_Size defc;
	TEST_OK(Tcl_ListObjGetElements(interp, inc_list, &incc, &incv));
	TEST_OK(Tcl_ListObjGetElements(interp, def_list, &defc, &defv));
	const char**	incp = ckalloc(sizeof(char*) * (incc ? incc : 1));	defer { ckfree(incp); };
	const char**	defp = ckalloc(sizeof(char*) * (defc ? defc : 1));	defer { ckfree(defp); };
	for (Tcl_Size j=0; j<incc; j++) incp[j] = Tcl_GetString(incv[j]);
	for (Tcl_Size j=0; j<defc; j++) defp[j] = Tcl_GetString(defv[j]);

	slimcc_options	opts = {
		.include_paths   = incp, .n_include_paths = (int)incc,
		.defines         = defp, .n_defines       = (int)defc,
	};

	// Compile each code block / file into its own module in r->ctx, collecting
	// the handles to load + link together below. slimcc_compile reports failures
	// through its return value (its own scratch context catches MIR build
	// errors), so no MIR longjmp can fire in this gathering phase.
	Tcl_Obj**	cv; Tcl_Size cc;
	Tcl_Obj**	fv; Tcl_Size fc;
	TEST_OK(Tcl_ListObjGetElements(interp, code_list, &cc, &cv));
	TEST_OK(Tcl_ListObjGetElements(interp, file_list, &fc, &fv));
	MIR_module_t*	mods = ckalloc(sizeof(MIR_module_t) * ((cc + fc) ? (cc + fc) : 1));	defer { ckfree(mods); };
	int				nmods = 0;

	for (Tcl_Size c=0; c<cc; c++) {
		char*			err = NULL;
		MIR_module_t	mod = slimcc_compile(r->ctx, "cdef", Tcl_GetString(cv[c]), &opts, &err);
		if (!mod) {
			if (err) { replace_tclobj(&compile_errors, Tcl_NewStringObj(err, -1)); free(err); }
			else replace_tclobj(&compile_errors, Tcl_NewStringObj("compilation failed", -1));
			replace_tclobj(&compileerror_code, cv[c]);
			goto compile_error;
		}
		mods[nmods++] = mod;
	}
	for (Tcl_Size f=0; f<fc; f++) {
		Tcl_Obj*	content = NULL;	defer { replace_tclobj(&content, NULL); };
		if (TCL_OK != slurp_file(interp, fv[f], &content)) {
			replace_tclobj(&extra_errormsg, Tcl_ObjPrintf("Error reading file \"%s\"", Tcl_GetString(fv[f])));
			replace_tclobj(&compile_errors, Tcl_GetObjResult(interp));
			goto compile_error;
		}
		char*			err = NULL;
		MIR_module_t	mod = slimcc_compile(r->ctx, Tcl_GetString(fv[f]), Tcl_GetString(content), &opts, &err);
		if (!mod) {
			if (err) { replace_tclobj(&compile_errors, Tcl_NewStringObj(err, -1)); free(err); }
			else replace_tclobj(&compile_errors, Tcl_NewStringObj("compilation failed", -1));
			replace_tclobj(&extra_errormsg, Tcl_ObjPrintf("Error compiling file \"%s\"", Tcl_GetString(fv[f])));
			goto compile_error;
		}
		mods[nmods++] = mod;
	}

	// Make library symbols resolvable, then load + link + generate machine code.
	{
		Tcl_Obj**	lv; Tcl_Size lc;
		TEST_OK(Tcl_ListObjGetElements(interp, lib_list, &lc, &lv));
		for (Tcl_Size j=0; j<lc; j++)
			dlopen_library(r, Tcl_GetString(lv[j]), libpath_list);
	}

	if (TCL_OK != jitc_finish_link(r, mods, nmods, &compile_errors))
		goto compile_error;

	// Avoid a circular reference between cdef and our new jitc intrep obj.
	replace_tclobj(&r->cdef, Tcl_DuplicateObj(cdef));

#if STUBSMODE
	{
		cdef_initstubs*	initstubs = NULL;
		void*			a = jitc_get_symbol(r, "_initstubs");
		memcpy(&initstubs, &a, sizeof initstubs);
		if (initstubs)
			if (NULL == (initstubs)(interp, Tcl_GetString(l->tclver)))
				THROW_ERROR("Could not init Tcl stubs");
	}
#endif
	{
		cdef_init*	init = NULL;
		void*		a = jitc_get_symbol(r, "init");
		memcpy(&init, &a, sizeof init);
		if (init) TEST_OK((init)(interp));
	}

	// Hand the gathered ownership to the intrep.
	r->used             = used;				used = NULL;
	r->exported_symbols = exported_symbols;	exported_symbols = NULL;
	r->exported_headers = exported_headers;	exported_headers = NULL;

	*rPtr = r;
	r = NULL;

	return TCL_OK;

compile_error:
	{
		Tcl_InterpState	state = Tcl_SaveInterpState(interp, TCL_OK);	defer { if (state) Tcl_DiscardInterpState(state); };
		const int	cmdc = extra_errormsg ? 5 : 3;
		Tcl_Obj*	cmd[5] = {};		defer { for (int k=0; k<5; k++) replace_tclobj(&cmd[k], NULL); };
		Tcl_Obj*	res = NULL;			defer { replace_tclobj(&res,		NULL); };
		Tcl_Obj*	errorcode = NULL;	defer { replace_tclobj(&errorcode,	NULL); };
		Tcl_Obj*	errormsg = NULL;	defer { replace_tclobj(&errormsg,	NULL); };

		if (!compileerror_code) replace_tclobj(&compileerror_code, l->lit[LIT_BLANK]);
		if (!compile_errors)    replace_tclobj(&compile_errors,    l->lit[LIT_BLANK]);

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
	*val = jitc_get_symbol(r, symstr);
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
	*l = (struct interp_cx){
		.instance_head.next = &l->instance_tail,
		.instance_tail.prev = &l->instance_head,
	};
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
		// Release libslimcc's process-global arena pools — the extension is
		// detaching from the process, so nothing more will compile here.
		slimcc_shutdown();
		Tcl_MutexFinalize(&g_compile_mutex);

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
