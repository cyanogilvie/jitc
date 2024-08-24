#include "jitcInt.h"
#include "tip445.h"
#include <sys/stat.h>

TCL_DECLARE_MUTEX(g_tcc_mutex);

// Interface with GDB JIT API {{{
TCL_DECLARE_MUTEX(gdb_jit_mutex);

/* GDB puts a breakpoint in this function.  */
void __attribute__((noinline)) __jit_debug_register_code() { };

/* Make sure to specify the version statically, because the
   debugger may check the version before we can set it.  */
struct jit_descriptor __jit_debug_descriptor = { 1, 0, 0, 0 };
// Interface with GDB JIT API }}}

static size_t	g_pagesize;

typedef const char* (cdef_initstubs)(Tcl_Interp* interp, const char* ver);
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

static void free_jitc_intrep(struct jitc_intrep* r) //{{{
{
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
	replace_tclobj(&r->exported_symbols, NULL);
	replace_tclobj(&r->exported_headers, NULL);
	replace_tclobj(&r->used, NULL);

	if (r->jce.symfile_addr) {
		Tcl_MutexLock(&gdb_jit_mutex);
		// Unregister with GDB
		if (__jit_debug_descriptor.first_entry == &r->jce) {
			__jit_debug_descriptor.first_entry = r->jce.next_entry;
		} else {
			r->jce.prev_entry->next_entry = r->jce.next_entry;
			if (r->jce.next_entry) r->jce.next_entry->prev_entry = r->jce.prev_entry;
		}
		r->jce.prev_entry = NULL;
		r->jce.next_entry = NULL;
		__jit_debug_descriptor.relevant_entry	= &r->jce;
		__jit_debug_descriptor.action_flag		= JIT_UNREGISTER_FN;
		__jit_debug_register_code();
		Tcl_MutexUnlock(&gdb_jit_mutex);
	}
	if (r->align_ofs) {
		ckfree(r->align_ofs);
		r->align_ofs = NULL;
	}
	Tcl_DeleteHashTable(&r->plt_syms);
	Tcl_DeleteHashTable(&r->rsyms);
	if (r->base != MAP_FAILED && r->base != NULL) {
		munmap(r->base, r->map_size);
		r->base = NULL;
	}
	if (r->is_mapped) {
		ckfree(r->is_mapped);
		r->is_mapped = NULL;
	}
	if (r->section_base) {
		ckfree(r->section_base);
		r->section_base = NULL;
	}
	if (r->lib_handles) {
		Tcl_Obj**	handle_v;
		int			handle_c;

		if (TCL_OK != Tcl_ListObjGetElements(NULL, r->lib_handles, &handle_c, &handle_v))
			Tcl_Panic("Corrupt lib_handle list: (%s)", Tcl_GetString(r->lib_handles));

		for (int i=0; i<handle_c; i++) {
			Tcl_WideInt	tmp;
			if (TCL_OK != Tcl_GetWideIntFromObj(NULL, handle_v[i], &tmp))
				Tcl_Panic("Could not retrieve dl handle from element: (%s)", Tcl_GetString(handle_v[i]));
			void* handle = (void*)tmp;
			dlclose(handle);
		}
	}
	replace_tclobj(&r->lib_handles, NULL);
	r->bytes = NULL;
	replace_tclobj(&r->bytesobj, NULL);
	if (r->resolved_symbols) {
		ckfree(r->resolved_symbols);
		r->resolved_symbols = NULL;
	}

	ckfree(r);
}

//}}}
static void free_jitc_internal_rep(Tcl_Obj* obj) //{{{
{
	Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(obj, &jitc_objtype);
	struct jitc_intrep*		r = ir->twoPtrValue.ptr1;
	struct jitc_instance*	instance = ir->twoPtrValue.ptr2;

	instance->next->prev = instance->prev;
	instance->prev->next = instance->next;
	//*instance = (struct jitc_instance){0};
	ckfree(instance);  instance = NULL;  ir->twoPtrValue.ptr2 = NULL;

	if (r->resolved_symbols && r->interp) {
		Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->rsyms, "release");
		if (he) {
			struct rsym*	release_rsym = Tcl_GetHashValue(he);
			if (ELFW(ST_TYPE)(release_rsym->sym->st_info) != STT_FUNC)
				Tcl_Panic("release symbol is not a function");
			cdef_release*	release = release_rsym->addr;
			(release)(r->interp);
		}
	}

	free_jitc_intrep(r);
	r = NULL;

	replace_tclobj((Tcl_Obj**)&ir->twoPtrValue.ptr2, NULL);
}

//}}}
static void dup_jitc_internal_rep(Tcl_Obj* src, Tcl_Obj* dup) //{{{
{
	Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(src, &jitc_objtype);
	struct jitc_intrep*		r = ir->twoPtrValue.ptr1;
	struct interp_cx*		l = Tcl_GetAssocData(r->interp, "jitc", NULL);
	Tcl_ObjInternalRep		newir = {0};
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
	"return -level 0 tclstub[info tclversion]",
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
static ElfW(Shdr)* section_header(ElfW(Ehdr)* hdr, int n) //{{{
{
	return (ElfW(Shdr)*)((uint8_t*)hdr + hdr->e_shoff + n * hdr->e_shentsize);
}

//}}}
static void* align_addr(void* addr, size_t align) //{{{
{
	uintptr_t	a = ((uintptr_t)addr + g_pagesize - 1) & ~(g_pagesize - 1);	// Ensure alignment with align and pagesize
	return (void*)(((uintptr_t)a + align - 1) & ~(align - 1));
}

//}}}
static int load_elf_obj(Tcl_Interp* interp, Tcl_Obj* fn, struct jitc_intrep* r) //{{{
{
	int						code = TCL_OK;
	struct TCCState*		s = NULL;
	Tcl_Channel				obj_chan;
	Tcl_Obj**				uv = NULL;
	int						uc = 0;

	// Open library dependencies
	if (r->needed) {
		Tcl_Obj**	needed_v;
		int			needed_c;

		TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, r->needed, &needed_c, &needed_v));
		replace_tclobj(&r->lib_handles, Tcl_NewListObj(needed_c, NULL));

		for (int i=0; i<needed_c; i++) {
			Tcl_Obj* tmp = NULL;
			//replace_tclobj(&tmp, Tcl_ObjPrintf("%s.so", Tcl_GetString(needed_v[i])));
			replace_tclobj(&tmp, needed_v[i]);
			void*	handle = dlopen(Tcl_GetString(tmp), RTLD_NOW | RTLD_GLOBAL);
			replace_tclobj(&tmp, NULL);
			if (!handle) THROW_ERROR_LABEL(finally, code, dlerror());
			TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, r->lib_handles, Tcl_NewWideIntObj((intptr_t)handle)));
		}
	}

	// Read object file
	obj_chan = Tcl_OpenFileChannel(interp, Tcl_GetString(fn), "rb", 0600);
	if (NULL == obj_chan) {
		code = TCL_ERROR;
		goto finally;
	}
	replace_tclobj(&r->bytesobj, Tcl_NewByteArrayObj(NULL, 0));
	if (TCL_INDEX_NONE == Tcl_ReadChars(obj_chan, r->bytesobj, TCL_INDEX_NONE, 0))
		THROW_POSIX_LABEL(finally, code, "Tcl_ReadChars");
	TEST_OK_LABEL(finally, code, Tcl_Close(interp, obj_chan));
	obj_chan = NULL;
	r->bytes = Tcl_GetByteArrayFromObj(r->bytesobj, &r->len);

	// Unpack the use objects for symbol resolution later
	if (r->used)
		TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, r->used, &uc, &uv));

	// Load
	ElfW(Ehdr)*	ehdr = (ElfW(Ehdr)*)r->bytes;
	const ElfW(Sym)*	symtab = NULL;
	const char*			strtab = (const char*)r->bytes + section_header(ehdr, ehdr->e_shstrndx)->sh_offset;
	r->is_mapped = ckalloc(ehdr->e_shnum * sizeof(uint8_t));
	memset(r->is_mapped, 0, ehdr->e_shnum * sizeof(uint8_t));
	r->align_ofs = ckalloc(ehdr->e_shnum * sizeof(uintptr_t));
	memset(r->align_ofs, 0, ehdr->e_shnum * sizeof(uintptr_t));
	r->section_base = ckalloc(ehdr->e_shnum * sizeof(void*));
	memset(r->section_base, 0, ehdr->e_shnum * sizeof(void*));
	void*				align = NULL;
	size_t				symc = 0;
	const char*			sym_strtab = NULL;
	for (int i=1; i<ehdr->e_shnum; i++) {
		const ElfW(Shdr)*	shdr = section_header(ehdr, i);
		if (shdr->sh_flags & SHF_ALLOC && shdr->sh_size > 0) {
			align = align_addr(align, shdr->sh_addralign);
			r->align_ofs[i] = align;
			align += shdr->sh_size;
			r->is_mapped[i] = 1;
		}

		switch (shdr->sh_type) {
			case SHT_SYMTAB:
			{
				symtab = (const ElfW(Sym)*)(r->bytes + shdr->sh_offset);
				sym_strtab = shdr->sh_link == ehdr->e_shstrndx ? strtab : (const char*)r->bytes + section_header(ehdr, shdr->sh_link)->sh_offset;
				symc = shdr->sh_size / shdr->sh_entsize;
				break;
			}
		}
	}
	r->symc = symc;

	// Make PLT for all undef symbols
	size_t			plt_size = 0;
	size_t			got_size = 3;	// first 3 reserved
	for (int i=1; i<symc; i++) {
		/*
		if (symtab[i].st_shndx == SHN_UNDEF) {
		}
		*/
		switch (ELFW(ST_TYPE)(symtab[i].st_info)) {
			case STT_NOTYPE:
			case STT_FUNC:   plt_size++; break;
			case STT_OBJECT: got_size+=2; break;
		}
	}
	if (plt_size) {
		align = align_addr(align, 1);
		r->plt = align;
		align += plt_size * sizeof(struct plt_entry);
	}
	if (got_size) {
		align = align_addr(align, 1);
		r->got = align;
		align += got_size * sizeof(void*);
	}

	// Map - MAP_32BIT to ensure that R_X86_64_32 relocations are possible
	//r->base = mmap(NULL, (uintptr_t)align, PROT_READ | PROT_WRITE, MAP_32BIT | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	r->base = mmap(NULL, (uintptr_t)align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r->base == MAP_FAILED) THROW_POSIX_LABEL(finally, code, "mmap");
	for (int i=1; i<ehdr->e_shnum; i++) {
		ElfW(Shdr)*	shdr = section_header(ehdr, i);
		if (r->is_mapped[i]) {
			r->section_base[i] = r->base + (uintptr_t)r->align_ofs[i];
			if (shdr->sh_size)               memcpy(r->section_base[i], r->bytes+shdr->sh_offset, shdr->sh_size);
			if (shdr->sh_type == SHT_NOBITS) memset(r->section_base[i], 0, shdr->sh_size);
		} else {
			r->section_base[i] = r->bytes + shdr->sh_offset;
		}
		shdr->sh_addr = (ElfW(Addr))r->section_base[i];	// So that GDB knows where it's loaded
	}

	// Resolve symbols
	r->resolved_symbols = ckalloc(symc * sizeof(r->resolved_symbols[0]));
	for (int i=1; i<symc; i++) {
		const ElfW(Shdr)*	refsec = section_header(ehdr, symtab[i].st_shndx);

		/*
		fprintf(stderr, "symbol[%d]: st_shndx != SHN_UNDEF? %d, st_shndx < %d? %d, not SHF_ALLOC? %d\n",
			i,
			symtab[i].st_shndx != SHN_UNDEF,
			ehdr->e_shnum,
			symtab[i].st_shndx < ehdr->e_shnum,
			symtab[i].st_shndx != SHN_UNDEF && symtab[i].st_shndx < ehdr->e_shnum && (refsec->sh_flags & SHF_ALLOC) == 0
			);
		switch (ELFW(ST_TYPE)(symtab[i].st_info)) {
			case STT_FUNC:
			case STT_OBJECT:
			case STT_NOTYPE:
				fprintf(stderr, "symbol[%d]: \"%s\": %p\n", i, sym_strtab + symtab[i].st_name,
					symtab[i].st_shndx == SHN_UNDEF ?
						dlsym(RTLD_DEFAULT, sym_strtab + symtab[i].st_name) :
						r->section_base[symtab[i].st_shndx] + symtab[i].st_value
						);
				break;

			case STT_SECTION:
				fprintf(stderr, "symbol[%d]: STT_SECTION \"%s\": %p\n", i, strtab + refsec->sh_name, r->section_base[symtab[i].st_shndx] + symtab[i].st_value);
				break;

			case STT_FILE:
				break;

			default:
				fprintf(stderr, "symbol[%d]: unhandled type %d\n", i, ELFW(ST_TYPE)(symtab[i].st_info));
		}
		*/
		if (
			symtab[i].st_shndx != SHN_UNDEF &&
			symtab[i].st_shndx < ehdr->e_shnum &&
			(refsec->sh_flags & SHF_ALLOC) == 0
		) {
			//fprintf(stderr, "Ignoring symbol %d in non-loaded section %d\n", i, symtab[i].st_shndx);
			continue;	// Ignore symbols in non-loaded sections
		}

		r->resolved_symbols[i].sym = &symtab[i];
		switch (ELFW(ST_TYPE)(symtab[i].st_info)) {
			case STT_FUNC:
			case STT_OBJECT:
			case STT_NOTYPE:
				r->resolved_symbols[i].name = sym_strtab + symtab[i].st_name;
				if (symtab[i].st_shndx == SHN_UNDEF) {
					int found = 0;
					for (int j=0; j<uc; j++) {
						struct jitc_intrep*	ur = NULL;
						TEST_OK_LABEL(finally, code, get_r_from_obj(interp, uv[j], &ur));
						
						Tcl_HashEntry*	he = Tcl_FindHashEntry(&ur->rsyms, r->resolved_symbols[i].name);
						/*
						fprintf(stderr, "Looking for \"\%s\" in use[%d]: %p\n", r->resolved_symbols[i].name, j, he);
						fprintf(stderr, "\tUse[%d] symbols:\n", j);
						for (int k=1; k<ur->symc; k++)
							fprintf(stderr, "\t\t%s: %p\n", ur->resolved_symbols[k].name, ur->resolved_symbols[k].addr);
						fprintf(stderr, "\trsyms hash:\n");
						Tcl_HashSearch	search;
						for (Tcl_HashEntry* he=Tcl_FirstHashEntry(&ur->rsyms, &search); he; he=Tcl_NextHashEntry(&search))
							fprintf(stderr, "\t\t%s: %p\n", (const char *)Tcl_GetHashKey(&ur->rsyms, he), ((struct rsym*)Tcl_GetHashValue(he))->addr);
						*/
						if (he) {
							// Check that the matching symbol in this "use" object
							// - matches the type of the symbol we're resolving
							// - has global visibility
							// - is defined in the use object itself
							struct rsym*	ursym = Tcl_GetHashValue(he);
							/*
							fprintf(stderr, "\tconsidering match, type match? %d, is global? %d, default vis? %d, defined? %d\n",
									ELFW(ST_TYPE)(ursym->sym->st_info) == ELFW(ST_TYPE)(symtab[i].st_info),
									ELFW(ST_BIND)(ursym->sym->st_info) == STB_GLOBAL,
									ELFW(ST_VISIBILITY)(ursym->sym->st_other) == STV_DEFAULT,
									ursym->sym->st_shndx != SHN_UNDEF);
							*/
							if (
								ELFW(ST_TYPE)(ursym->sym->st_info) != ELFW(ST_TYPE)(symtab[i].st_info) ||
								ELFW(ST_BIND)(ursym->sym->st_info) != STB_GLOBAL ||
								ELFW(ST_VISIBILITY)(ursym->sym->st_other) != STV_DEFAULT ||
								ursym->sym->st_shndx == SHN_UNDEF
							) {
								//fprintf(stderr, "\tDisqualified found symbol\n");
								continue;
							}

							r->resolved_symbols[i].addr = ursym->addr;
							found = 1;
							break;
						}
					}

					// If we couldn't find it in a "use" object, fall back to resolving it
					// against our process image
					if (!found)
						r->resolved_symbols[i].addr = dlsym(RTLD_DEFAULT, r->resolved_symbols[i].name);

					if (!r->resolved_symbols[i].addr) THROW_PRINTF_LABEL(finally, code, "Symbol \"%s\" not found", r->resolved_symbols[i].name);
				} else {
					r->resolved_symbols[i].addr = r->section_base[symtab[i].st_shndx] + symtab[i].st_value;
				}
				//fprintf(stderr, "symbol %d, %s, name: (%s)\n", i, ELFW(ST_TYPE)(symtab[i].st_info) == STT_FUNC ? "func" : "object", r->resolved_symbols[i].name);
				break;

			case STT_SECTION:
				r->resolved_symbols[i].name = strtab + refsec->sh_name;
				r->resolved_symbols[i].addr = r->section_base[symtab[i].st_shndx] + symtab[i].st_value;
				//fprintf(stderr, "symbol %d, section, name: (%s)\n", i, r->resolved_symbols[i].name);
				break;

			default:
				continue;
		}
		int				isnew;
		Tcl_HashEntry*	he = Tcl_CreateHashEntry(&r->rsyms, r->resolved_symbols[i].name, &isnew);
		//if (!isnew) THROW_PRINTF_LABEL(finally, code, "Duplicate symbol: %s", r->resolved_symbols[i].name);
		if (isnew) Tcl_SetHashValue(he, &r->resolved_symbols[i]);
	}

	// Populate the PLT and GOT
	if (plt_size) {
		r->plt = r->base + (uintptr_t)r->plt;
		r->got = r->base + (uintptr_t)r->got;
		int goti = 3;
		int plti = 0;
		for (int i=1; i<symc; i++) {
			/*
			if (symtab[i].st_shndx == SHN_UNDEF) {
			}
			*/
			switch (ELFW(ST_TYPE)(symtab[i].st_info)) {
				case STT_OBJECT:
				case STT_FUNC:
				case STT_NOTYPE:
					const char*	sym_name = r->resolved_symbols[i].name;
					void*		sym_val  = r->resolved_symbols[i].addr;
					int			isnew;
					//fprintf(stderr, "Creating PLT entry for symbol %d: \"%s\": %p\n", i, sym_name, sym_val);
					Tcl_HashEntry*	he = Tcl_CreateHashEntry(&r->plt_syms, sym_name, &isnew);
					//if (!isnew) THROW_PRINTF_LABEL(finally, code, "Duplicate PLT entry: %s", sym_name);
					if (!isnew) continue;
					if (ELFW(ST_TYPE)(symtab[i].st_info) == STT_OBJECT) {
						r->got[goti] = sym_val;
						r->got[goti+1] = &r->got[goti];		// TODO: figure out why we need this indirection
						goti++;
						Tcl_SetHashValue(he, &r->got[goti]);
						//fprintf(stderr, "got[%d]: %p \"%s\"\n", goti, got[goti], sym_name);
						goti++;
					} else {
						r->plt[plti].target = sym_val;
						r->plt[plti].insn[0] = 0xff;	// jmp
						r->plt[plti].insn[1] = 0x25;	// modrm
						*(int32_t*)&r->plt[plti].insn[2] = (ptrdiff_t)(offsetof(struct plt_entry, target) - (offsetof(struct plt_entry, insn) + 6));
						Tcl_SetHashValue(he, &r->plt[plti].insn);
						//fprintf(stderr, "Populated PLT[%i]: \"%s\": %p\n", plti, sym_name, sym_val);
						plti++;
					}
			}
		}
	}

	// Relocate and link
	for (int i=1; i<ehdr->e_shnum; i++) {
		const ElfW(Shdr)*	shdr = section_header(ehdr, i);
		switch (shdr->sh_type) {
			case SHT_RELA:
			{
				const ElfW(Word)	target_section = shdr->sh_info;
				const ElfW(Rela)*	rel = (const ElfW(Rela)*)(r->bytes + shdr->sh_offset);

				//if (strcmp(".rela.text", strtab + shdr->sh_name) != 0) continue;
				const int is_exec = section_header(ehdr, target_section)->sh_flags & SHF_EXECINSTR;
				if (!is_exec) continue;
				//fprintf(stderr, "Processing rela section %s, %d entries\n", strtab + shdr->sh_name, shdr->sh_size / shdr->sh_entsize);
				const int entries = shdr->sh_size / shdr->sh_entsize;
				for (int i=0; i<entries; i++, rel++) {
					const int			si = ELFW(R_SYM)(rel->r_info);
					const char*			sym_name = NULL;
					void*				sym_val = NULL;
					if (si) {
						sym_name = r->resolved_symbols[si].name;
						sym_val  = r->resolved_symbols[si].addr;
					}

					switch (ehdr->e_machine) {
						case EM_X86_64: // x86_64 {{{
							#define S	(uintptr_t)sym_val
							#define A	(intptr_t)rel->r_addend
							#define P	(uintptr_t)dst
							#define GOT	(uintptr_t)r->got
							switch ELFW(R_TYPE)(rel->r_info) {
								case R_X86_64_32:
								case R_X86_64_32S:
								{
									uint32_t*	dst = r->section_base[target_section] + rel->r_offset;
									//fprintf(stderr, "\tR_X86_64_32 %s + %ld: %p -> %p\n", sym_name, rel->r_addend, dst, (void*)(S + A));
									*dst = (uint32_t)(S + A);
									break;
								}

								case R_X86_64_64:
								{
									uint64_t*	dst = r->section_base[target_section] + rel->r_offset;
									//fprintf(stderr, "\tR_X86_64_64 %s + %ld: %p -> %p\n", sym_name, rel->r_addend, dst, (void*)(S + A));
									*dst = (uint64_t)(S + A);
									break;
								}

								case R_X86_64_PC32:
								{
									int32_t*	dst = r->section_base[target_section] + rel->r_offset;
									int32_t	v = (int32_t)(S + A - P);
									//fprintf(stderr, "\tR_X86_64_PC32 %s + %ld: %p -> %d\n", sym_name, rel->r_addend, dst, v);
									*dst = v;
									break;
								}

								case R_X86_64_PLT32:
								{
									int32_t*		dst = r->section_base[target_section] + rel->r_offset;
									Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->plt_syms, sym_name);
									if (!he) THROW_PRINTF_LABEL(finally, code, "PLT entry not found: %d %s", si, sym_name);
									const uintptr_t	L = (uintptr_t)Tcl_GetHashValue(he);

									int32_t	v = (int32_t)(L + A - P);
									//fprintf(stderr, "\tR_X86_64_PLT32 %s: %p -> 0x%04x\n", sym_name, dst, v);
									*dst = v;
									break;
								}

								case R_X86_64_GOTPCREL:
								{
									int32_t*	dst = r->section_base[target_section] + rel->r_offset;
									Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->plt_syms, sym_name);
									if (!he) THROW_PRINTF_LABEL(finally, code, "GOT entry not found: %d %s", si, sym_name);
									const uintptr_t	G = (uintptr_t)Tcl_GetHashValue(he) - (uintptr_t)r->got;
									int32_t		v = (int32_t)(G + GOT + A - P);
									//fprintf(stderr, "\tR_X86_64_GOTPCREL %s: %p -> 0x%04x\n", sym_name, dst, v);
									*dst = v;
									break;
								}

								default:
									THROW_PRINTF_LABEL(finally, code, "Unhandled relocation: %ld", ELFW(R_TYPE)(rel->r_info));
							}
							#undef S
							#undef A
							#undef P
							#undef GOT
							break;
							//}}}
						case EM_AARCH64: // aarch64 {{{
							// TODO: implement
							THROW_PRINTF_LABEL(finally, code, "aarch64 not implemented yet: %s %d", __FILE__, __LINE__);
							#define S	(uintptr_t)sym_val
							#define A	(intptr_t)rel->r_addend
							#define P	(uintptr_t)dst
							#define GOT	(uintptr_t)got
							#define G(exp)		
							#define GDAT(exp)
							#define Page(x) ((x) & ~0xfff)
							switch ELFW(R_TYPE)(rel->r_info) {
								case R_AARCH64_ADR_GOT_PAGE:
									// Set the immediate value of an ADRP to bits [32:12] of X; check that –232 ≤ X < 232
									// Page(G(GDAT(S+A))) - Page(P)
									THROW_PRINTF_LABEL(finally, code, "R_AARCH64_ADR_GOT_PAGE not implemented yet: %s %d", __FILE__, __LINE__);
								case R_AARCH64_LD64_GOT_LO12_NC:
									// Set the LD/ST immediate field to bits [11:3] of X. No overflow check; check that X&7 = 0
									// G(GDAT(S+A))
									THROW_PRINTF_LABEL(finally, code, "R_AARCH64_LD64_GOT_LO12_NC not implemented yet: %s %d", __FILE__, __LINE__);
								case R_AARCH64_CALL26:
									// Set a CALL immediate field to bits [27:2] of X; check that -227 ≤ X < 227
									// S+A-P
									THROW_PRINTF_LABEL(finally, code, "R_AARCH64_CALL26 not implemented yet: %s %d", __FILE__, __LINE__);
								case R_AARCH64_JUMP26:
									// Set a B immediate field to bits [27:2] of X; check that -227 ≤ X < 227
									// S+A-P
									THROW_PRINTF_LABEL(finally, code, "R_AARCH64_JUMP26 not implemented yet: %s %d", __FILE__, __LINE__);

								/* copilot guesses:
								case R_AARCH64_ABS64:
								{
									uint64_t*	dst = r->section_base[target_section] + rel->r_offset;
									//fprintf(stderr, "\tR_AARCH64_ABS64 %s + %d: %p -> %p\n", sym_name, rel->r_addend, dst, (void*)(S + A));
									*dst = (uint64_t)(S + A);
									break;
								}

								case R_AARCH64_PREL32:
								{
									int32_t*	dst = r->section_base[target_section] + rel->r_offset;
									int32_t	v = (int32_t)(S + A - P);
									//fprintf(stderr, "\tR_AARCH64_PREL32 %s + %d: %p -> %d\n", sym_name, rel->r_addend, dst, v);
									*dst = v;
									break;
								}

								case R_AARCH64_CALL26:
								{
									uint32_t*	dst = r->section_base[target_section] + rel->r_offset;
									uint32_t	v = (uint32_t)(S + A - P);
									//fprintf(stderr, "\tR_AARCH64_CALL26 %s + %d: %p -> %d\n", sym_name, rel->r_addend, dst, v);
									*dst = (*dst & 0xfc000000) | (v & 0x03ffffff);
									break;
								}
								*/

								default:
									THROW_PRINTF_LABEL(finally, code, "Unhandled relocation: %ld", ELFW(R_TYPE)(rel->r_info));
							}
							#undef S
							#undef A
							#undef P
							#undef GOT
							#undef Page
							#undef G
							#undef GDAT
							break;
						//}}}
						default:
							THROW_PRINTF_LABEL(finally, code, "Unhandled machine: %d", ehdr->e_machine);
					}
				}
				break;
			}
			case SHT_REL:
			{
				THROW_ERROR_LABEL(finally, code, "SHT_REL not implemented");
				break;
			}
		}
	}

	// Set perms
	for (int i=1; i<ehdr->e_shnum; i++) {
		const ElfW(Shdr)*	shdr = section_header(ehdr, i);
		if (r->is_mapped[i]) {
			int		prot = PROT_READ;
			if (shdr->sh_flags & SHF_EXECINSTR) prot |= PROT_EXEC;
			if (shdr->sh_flags & SHF_WRITE)     prot |= PROT_WRITE;
			if (-1 == mprotect(r->section_base[i], shdr->sh_size, prot)) THROW_POSIX_LABEL(finally, code, "mprotect");
		}
	}
	if (r->plt) {
		if (-1 == mprotect(r->plt, plt_size * sizeof(struct plt_entry), PROT_READ | PROT_EXEC)) THROW_POSIX_LABEL(finally, code, "mprotect");
	}

	// Register with GDB
	Tcl_MutexLock(&gdb_jit_mutex);
	r->jce = (struct jit_code_entry){
		.symfile_addr	= (const char*)r->bytes,
		.symfile_size	= r->len,
		.next_entry		= __jit_debug_descriptor.first_entry,
	};
	if (r->jce.next_entry) r->jce.next_entry->prev_entry = &r->jce;
	__jit_debug_descriptor.first_entry		= &r->jce;
	__jit_debug_descriptor.relevant_entry	= &r->jce;
	__jit_debug_descriptor.action_flag		= JIT_REGISTER_FN;
	__jit_debug_register_code();
	Tcl_MutexUnlock(&gdb_jit_mutex);

finally:
	if (s) tcc_delete(s);
	if (obj_chan) {
		Tcl_Close(interp, obj_chan);
		obj_chan = NULL;
	}
	return code;
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
	Tcl_StatBuf*		statbuf = NULL;
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
	Tcl_Obj*			add_library_queue = NULL;
	Tcl_Obj*			add_symbol_queue = NULL;
	Tcl_Obj*			used = NULL;

	const int use_stubs = 0;

	Tcl_DStringInit(&preamble);

	TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, cdef, &oc, &ov));
	if (oc % 2 == 1)
		THROW_PRINTF_LABEL(finally, code, "cdef must be a list with an even number of elements (got %d): %s", oc, Tcl_GetString(cdef));

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
					struct jitc_intrep*	ur;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, ov[i+1], &sc, &sv));
					if (sc >= 1) {
						if (!used) replace_tclobj(&used, Tcl_NewListObj(1, NULL));
						TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, used, sv[0]));
						TEST_OK_LABEL(finally, code, get_r_from_obj(interp, sv[0], &ur));
					}
				}
				break;
			case PART_USE:
				{
					struct jitc_intrep*	ur;
					if (!used) replace_tclobj(&used, Tcl_NewListObj(1, NULL));
					TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, used, ov[i+1]));
					TEST_OK_LABEL(finally, code, get_r_from_obj(interp, ov[i+1], &ur));
				}
				break;
		}
		_Pragma("GCC diagnostic pop")
	}

	if (debugpath) {
		statbuf = Tcl_AllocStatBuf();
		if (
				-1 == Tcl_FSStat(debugpath, statbuf) ||	// Doesn't exist
				!(S_ISDIR(Tcl_GetModeFromStat(statbuf)))	// Not a directory
		) THROW_ERROR_LABEL(finally, code, "debug path \"", Tcl_GetString(debugpath), "\" doesn't exist");
	}

	Tcl_MutexLock(&g_tcc_mutex); mutexheld = 1;

	tcc = tcc_new();
	tcc_set_error_func(tcc, &compile_errors, errfunc);
	tcc_set_options(tcc, "-Wl,--enable-new-dtags");

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
				if (use_stubs) {
					tcc_define_symbol(tcc, "USE_TCL_STUBS", "1");
				}

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
			case PART_LIBRARY_PATH:
				if (-1 == tcc_add_library_path(tcc, Tcl_GetString(ov[i+1])))
					THROW_ERROR_LABEL(finally, code, "Error adding library path \"", Tcl_GetString(ov[i+1]), "\"");
				break;

			case PART_PACKAGE: //{{{
				{
					int			pc;
					Tcl_Obj**	pv = NULL;
					Tcl_Obj*	cmd[3] = {0};
					Tcl_Obj*	lib_fqpath = NULL;

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
						Tcl_DStringAppend(&preamble, "\n#include <", -1);
						Tcl_DStringAppend(&preamble, Tcl_GetString(vals[KEY_HEADER]), -1);
						Tcl_DStringAppend(&preamble, ">\n", -1);
					}

					if (vals[KEY_INCLUDEDIR_RUNTIME])
						tcc_add_include_path(tcc, Tcl_GetString(vals[KEY_INCLUDEDIR_RUNTIME]));
					if (vals[KEY_INCLUDEDIR_INSTALL])
						tcc_add_include_path(tcc, Tcl_GetString(vals[KEY_INCLUDEDIR_INSTALL]));
#if 0
					if (vals[KEY_LIBDIR_RUNTIME]) {
						if (-1 == tcc_add_library_path(tcc, Tcl_GetString(vals[KEY_LIBDIR_RUNTIME]))) {
							// TODO: what?
						} else {
							Tcl_Obj*	runpath_opt = NULL;
							replace_tclobj(&runpath_opt, Tcl_ObjPrintf("-Wl,-rpath=%s", Tcl_GetString(vals[KEY_LIBDIR_RUNTIME])));
							tcc_set_options(tcc, Tcl_GetString(runpath_opt));
							replace_tclobj(&runpath_opt, NULL);
						}
					}
					if (vals[KEY_LIBDIR_INSTALL]) {
						if (-1 == tcc_add_library_path(tcc, Tcl_GetString(vals[KEY_LIBDIR_INSTALL]))) {
							// TODO: what?
						} else {
							Tcl_Obj*	runpath_opt = NULL;
							replace_tclobj(&runpath_opt, Tcl_ObjPrintf("-Wl,-rpath=%s", Tcl_GetString(vals[KEY_LIBDIR_INSTALL])));
							tcc_set_options(tcc, Tcl_GetString(runpath_opt));
							replace_tclobj(&runpath_opt, NULL);
						}
					}
					if (vals[KEY_LIBRARY]) {
						const char* libstr = Tcl_GetString(vals[KEY_LIBRARY]);
						/*
						if (strncmp("lib", libstr, 3) == 0) {
							libstr += 3;
						}
						*/
						if (!add_library_queue) replace_tclobj(&add_library_queue, Tcl_NewListObj(1, NULL));
						TEST_OK_LABEL(freevals, code, Tcl_ListObjAppendElement(interp, add_library_queue, Tcl_NewStringObj(libstr, -1)));
					}
#else
					{
						int			resolved = 0;
						struct stat	statbuf = {0};

						if (vals[KEY_LIBDIR_RUNTIME] && vals[KEY_LIBRARY]) {
							replace_tclobj(&lib_fqpath, Tcl_ObjPrintf("%s/%s.so", Tcl_GetString(vals[KEY_LIBDIR_RUNTIME]), Tcl_GetString(vals[KEY_LIBRARY])));
							fprintf(stderr, "checking %s\n", Tcl_GetString(lib_fqpath));
							if (0 == stat(Tcl_GetString(lib_fqpath), &statbuf)) {
								if (!add_library_queue) replace_tclobj(&add_library_queue, Tcl_NewListObj(1, NULL));
								TEST_OK_LABEL(freevals, code, Tcl_ListObjAppendElement(interp, add_library_queue, lib_fqpath));
								resolved = 1;
							}
						}

						fprintf(stderr, "checking %s\n", Tcl_GetString(lib_fqpath));
						if (!resolved && vals[KEY_LIBDIR_INSTALL] && vals[KEY_LIBRARY]) {
							replace_tclobj(&lib_fqpath, Tcl_ObjPrintf("%s/%s.so", Tcl_GetString(vals[KEY_LIBDIR_INSTALL]), Tcl_GetString(vals[KEY_LIBRARY])));
							if (0 == stat(Tcl_GetString(lib_fqpath), &statbuf)) {
								if (!add_library_queue) replace_tclobj(&add_library_queue, Tcl_NewListObj(1, NULL));
								TEST_OK_LABEL(freevals, code, Tcl_ListObjAppendElement(interp, add_library_queue, lib_fqpath));
								resolved = 1;
							}
						}

						if (!resolved) {
							THROW_PRINTF_LABEL(freevals, code, "Unable to resolve library: \"%s\"", Tcl_GetString(vals[KEY_LIBRARY]));
						}
					}
#endif

				freevals:
					for (int i=0; i<3; i++)
						replace_tclobj(&cmd[i], NULL);
					for (int i=0; i<KEY_END; i++)
						replace_tclobj(&vals[i], NULL);
					replace_tclobj(&lib_fqpath, NULL);

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
						if (!add_symbol_queue) replace_tclobj(&add_symbol_queue, Tcl_NewListObj(2, NULL));
						TEST_OK_LABEL(freevals, code, Tcl_ListObjAppendElement(interp, add_symbol_queue, useobj));
						TEST_OK_LABEL(freevals, code, Tcl_ListObjAppendElement(interp, add_symbol_queue, use_symbols));
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

	//fprintf(stderr, "compiling: %s\n", Tcl_GetString(cdef));
	//tcc_set_output_type(tcc, TCC_OUTPUT_MEMORY);
	tcc_set_output_type(tcc, TCC_OUTPUT_OBJ);
	//tcc_set_output_type(tcc, TCC_OUTPUT_DLL);

	if (add_symbol_queue) {
		int			qc;
		Tcl_Obj**	qv = NULL;

		TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, add_symbol_queue, &qc, &qv));

		if (qc % 2 != 0)
			THROW_ERROR_LABEL(finally, code, "add_symbol_queue must have an even number of elements");

		for (int i=0; i<qc; i+=2) {
			int			sc;
			Tcl_Obj**	sv = NULL;
			Tcl_Obj*	useobj		= qv[i];
			Tcl_Obj*	use_symbols	= qv[i+1];

			TEST_OK_LABEL(usedone, code, Tcl_ListObjGetElements(interp, use_symbols, &sc, &sv));
			for (int s=0; s<sc; s++) {
				void*	val = NULL;
				TEST_OK_LABEL(finally, code, Jitc_GetSymbolFromObj(interp, useobj, sv[s], &val));
				//tcc_add_symbol(tcc, Tcl_GetString(sv[s]), val);
			}
		}

		replace_tclobj(&add_symbol_queue, NULL);
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

	// Hack around wchar confusion on musl / aarch64
	tcc_define_symbol(tcc, "__DEFINED_wchar_t", "");

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
			case PART_LIBRARY_PATH:
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

					if (mode == MODE_TCL && use_stubs && -1 == tcc_compile_string(tcc, "#include <stdio.h>\n#include <tcl.h>\nconst char* _initstubs(Tcl_Interp* interp, const char* ver) {fprintf(stderr, \"_initstubs\\n\"); return Tcl_InitStubs(interp, ver, 0);}"))
						THROW_ERROR_LABEL(codeerror, code, "Error compiling _initstubs");

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

			case PART_LIBRARY:
				if (-1 == tcc_add_library(tcc, Tcl_GetString(v)))
					THROW_ERROR_LABEL(finally, code, "Error adding library \"", Tcl_GetString(v), "\"");
				break;

			case PART_DEFINE:
				{
					Tcl_Obj**	sv;
					int			sc;

					TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, v, &sc, &sv));
					if (sc < 1 || sc > 2)
						THROW_ERROR_LABEL(finally, code, "Definition must be a list: name value: \"", Tcl_GetString(v), "\"");
					if (sc == 1) {
						tcc_define_symbol(tcc, Tcl_GetString(sv[0]), "");
					} else {
						tcc_define_symbol(tcc, Tcl_GetString(sv[0]), Tcl_GetString(sv[1]));
					}
				}
				break;

			case PART_FILTER:
				{
					int			len;
					Tcl_GetStringFromObj(v, &len);

					replace_tclobj(&filter, len ? v : NULL);
				}
				break;

			default:
				THROW_ERROR_LABEL(finally, code, "Invalid part id");
		}
	}

	if (mode == MODE_TCL) {
	/*
		if (!add_library_queue) replace_tclobj(&add_library_queue, Tcl_NewListObj(1, NULL));
		TEST_OK_LABEL(freevals, code, Tcl_ListObjAppendElement(interp, add_library_queue, l->tclstublib));
	*/
		if (-1 == tcc_add_library(tcc, Tcl_GetString(l->tclstublib)))
			THROW_ERROR_LABEL(finally, code, "Error adding library \"", Tcl_GetString(l->tclstublib), "\"");
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
		.interp				= interp,
		.used				= used,
		.exported_symbols	= exported_symbols,
		.exported_headers	= exported_headers
	};
	used = exported_headers = exported_symbols = NULL;	// Transfer their refs (if any) to r->export_*

	Tcl_InitHashTable(&r->plt_syms, TCL_STRING_KEYS);
	Tcl_InitHashTable(&r->rsyms,    TCL_STRING_KEYS);
	replace_tclobj(&r->needed, add_library_queue);
	replace_tclobj(&add_library_queue, NULL);

	{
		char		template[] = P_tmpdir "/jitc_XXXXXX";
		char*		base = mkdtemp(template);
		Tcl_Obj*	tmp_fn = NULL;
		Tcl_DString	objfn;

		Tcl_DStringInit(&objfn);

		if (base == NULL) THROW_POSIX_LABEL(tmpfiledone, code, "Error creating temporary base directory");
		Tcl_DStringAppend(&objfn, base, sizeof(template)-1);
		Tcl_DStringAppend(&objfn, "/obj.o", -1);
		replace_tclobj(&tmp_fn, Tcl_NewStringObj(Tcl_DStringValue(&objfn), Tcl_DStringLength(&objfn)));

		const int output_rc = tcc_output_file(tcc, Tcl_DStringValue(&objfn));
		if (output_rc == -1) THROW_ERROR_LABEL(tmpfiledone, code, "Couldn't write object file");

		//TEST_OK_LABEL(tmpfiledone, code, Tcl_LoadFile(interp, tmp_fn, NULL, 0, NULL, &r->handle));
		TEST_OK_LABEL(tmpfiledone, code, load_elf_obj(interp, tmp_fn, r));

	tmpfiledone:
		if (base) {
			if (tmp_fn) {
				const int unlink_rc = unlink(Tcl_GetString(tmp_fn));
				if (unlink_rc == -1) THROW_POSIX_LABEL(finally, code, "Error unlinking jit object temporary file");
			}
			const int rmdir_rc = rmdir(base);
			if (rmdir_rc == -1) THROW_POSIX_LABEL(finally, code, "Error removing temporary base directory");
		}
		Tcl_DStringFree(&objfn);
		replace_tclobj(&tmp_fn, NULL);
		if (code != TCL_OK) goto finally;
	}

	if (compile_errors) goto finally;

	// Avoid a circular reference between cdef and our new jitc intrep obj
	replace_tclobj(&r->cdef, Tcl_DuplicateObj(cdef));

	if (use_stubs) {
		Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->rsyms, "_initstubs");
		if (he) {
			struct rsym*	initstubs_rsym = Tcl_GetHashValue(he);
			if (ELFW(ST_TYPE)(initstubs_rsym->sym->st_info) != STT_FUNC)
				THROW_ERROR_LABEL(finally, code, "initstubs symbol is not a function");
			cdef_initstubs*	initstubs = initstubs_rsym->addr;
			//fprintf(stderr, "cdef defines initstubs, calling: %p, symbols: (%s)\n", initstubs, Tcl_GetString(r->symbols));
			if (NULL == (initstubs)(interp, Tcl_GetString(l->tclver)))
				THROW_ERROR_LABEL(finally, code, "Could not init Tcl stubs");
		}
	}

	{
		Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->rsyms, "init");
		if (he) {
			struct rsym*	init_rsym = Tcl_GetHashValue(he);
			if (ELFW(ST_TYPE)(init_rsym->sym->st_info) != STT_FUNC)
				THROW_ERROR_LABEL(finally, code, "init symbol is not a function");
			cdef_init*	init = init_rsym->addr;
			//fprintf(stderr, "cdef defines init, calling: %p, symbols: (%s)\n", init, Tcl_GetString(r->symbols));
			TEST_OK_LABEL(finally, code, (init)(interp));
		}
	}

	replace_tclobj(&r->debugfiles, debugfiles);
	replace_tclobj(&debugfiles, NULL);

finally:
	if (tcc) {
		tcc_delete(tcc);
		tcc = NULL;
	}

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
		code = Tcl_RestoreInterpState(interp, state); state = NULL;
		Tcl_SetObjErrorCode(interp, errorcode);
		Tcl_SetObjResult(interp, errormsg);
		code = TCL_ERROR;
	done_compileerror:
		if (state) Tcl_DiscardInterpState(state);
		for (int i=0; i<5; i++) replace_tclobj(&cmd[i], NULL);
		replace_tclobj(&errorcode, NULL);
		replace_tclobj(&errormsg, NULL);
		replace_tclobj(&res, NULL);
	}

	if (code == TCL_OK) {
		*rPtr = r;
		r = NULL;
	}

	if (chan) {
		Tcl_Close(interp, chan);
		chan = NULL;
	}

	if (0 && debugfiles) {
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
	replace_tclobj(&used,				NULL);
	replace_tclobj(&exported_symbols,	NULL);
	replace_tclobj(&exported_headers,	NULL);
	replace_tclobj(&compileerror_code,	NULL);
	replace_tclobj(&add_library_queue,	NULL);
	replace_tclobj(&add_symbol_queue,	NULL);

	if (statbuf) {
		ckfree(statbuf);
		statbuf = NULL;
	}

	if (r) {
		free_jitc_intrep(r);
		r = NULL;
	}

	return code;
}

//}}}
int get_r_from_obj(Tcl_Interp* interp, Tcl_Obj* obj, struct jitc_intrep** rPtr) //{{{
{
	int					code = TCL_OK;
	Tcl_ObjInternalRep*	ir = Tcl_FetchInternalRep(obj, &jitc_objtype);
	struct jitc_intrep*	r = NULL;

	if (ir == NULL) {
		struct interp_cx*	l = Tcl_GetAssocData(interp, "jitc", NULL);
		Tcl_ObjInternalRep	newir = {0};

		TEST_OK_LABEL(finally, code, compile(interp, obj, (struct jitc_intrep **)&newir.twoPtrValue.ptr1));

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

	while (l->instance_head.next != &l->instance_tail) {
		struct jitc_instance*	instance = l->instance_head.next;

		if (!Tcl_HasStringRep(instance->obj)) Tcl_GetString(instance->obj);	// Regenerate the string rep
		Tcl_FreeInternalRep(instance->obj);									// Free the intrep (which references pointers we're about to invalidate by unloading our lib)
	}

	for (int i=0; i<LIT_SIZE; i++)
		replace_tclobj(&l->lit[i], NULL);

	replace_tclobj(&l->tclstublib, NULL);
	replace_tclobj(&l->tclver, NULL);

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

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, cdef, &r));

	Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->rsyms, Tcl_GetString(symbol));
	if (he) {
		struct rsym*	rsym = Tcl_GetHashValue(he);
		*val = rsym->addr;
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
	Tcl_HashSearch		search;

	TEST_OK_LABEL(finally, code, get_r_from_obj(interp, cdef, &r));

	replace_tclobj(&lsymbols, Tcl_NewListObj(r->symc, NULL));

	for (Tcl_HashEntry* he = Tcl_FirstHashEntry(&r->rsyms, &search); he; he = Tcl_NextHashEntry(&search)) {
		struct rsym*	rsym = Tcl_GetHashValue(he);
		TEST_OK_LABEL(finally, code, Tcl_ListObjAppendElement(interp, lsymbols, Tcl_NewStringObj(rsym->name, -1)));
	}

	replace_tclobj(symbols, lsymbols);

finally:
	replace_tclobj(&lsymbols, NULL);

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
static int nrapply_cmd(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
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
static int nrapply_cmd_setup(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	return Tcl_NRCallObjProc(interp, nrapply_cmd, cdata, objc, objv);
}

//}}}
static int _bind_invoke_curried(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj *const objv[]) //{{{
{
	int						code = TCL_OK;
	struct proc_binding*	binding = cdata;
#define STATIC_ARGS_SPACE	10
	Tcl_Obj*				o_static[STATIC_ARGS_SPACE];
	Tcl_Obj**				ov = NULL;
	int						oc = 0;
	Tcl_Obj**				cv = NULL;
	int						cc, arg = 0;

	TEST_OK_LABEL(finally, code, Tcl_ListObjGetElements(interp, binding->curryargs, &cc, &cv));
	oc = cc + objc;
	ov = oc <= STATIC_ARGS_SPACE ? o_static : ckalloc(sizeof(Tcl_Obj*) * oc);
	ov[arg++] = objv[0];
	for (int i=0; i<cc; i++)   ov[arg++] = cv[i];
	for (int i=1; i<objc; i++) ov[arg++] = objv[i];

	code = (binding->resolved)(NULL, interp, oc, ov);

finally:
	if (ov != o_static) {
		ckfree(ov);
		ov = NULL;
	}
	return code;
#undef STATIC_ARGS_SPACE
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
	int						code = TCL_OK;
	struct proc_binding*	binding = NULL;

	enum {A_cmd, A_NAME, A_CDEF, A_SYMBOL, A_args};
	CHECK_MIN_ARGS_LABEL(finally, code, "name cdef symbol ?curryarg ...?");

	binding = ckalloc(sizeof *binding);
	*binding = (struct proc_binding){0};
	replace_tclobj(&binding->cdef,   objv[A_CDEF]);
	replace_tclobj(&binding->symbol, objv[A_SYMBOL]);
	TEST_OK_LABEL(finally, code, Jitc_GetSymbolFromObj(interp, objv[A_CDEF], objv[A_SYMBOL], (void**)&binding->resolved));
	if (objc > A_args) {
		replace_tclobj(&binding->curryargs, Tcl_NewListObj(objc-A_args, objv+A_args));
		if (Tcl_NRCreateCommand(interp, Tcl_GetString(objv[A_NAME]), _bind_invoke_curried_setup, _bind_invoke_curried, binding, _unbind) == NULL)
			THROW_ERROR_LABEL(finally, code, "Failed to create command");
	} else {
		if (Tcl_NRCreateCommand(interp, Tcl_GetString(objv[A_NAME]), _bind_invoke_setup, binding->resolved, binding, _unbind) == NULL)
			THROW_ERROR_LABEL(finally, code, "Failed to create command");
	}

	binding = NULL;	// Hand over to cmd registration, will be freed by _unbind

finally:
	if (binding) {
		replace_tclobj(&binding->cdef, NULL);
		replace_tclobj(&binding->symbol, NULL);
		replace_tclobj(&binding->curryargs, NULL);
		ckfree(binding);
		binding = NULL;
	}
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
	int					code = TCL_OK;
	//Tcl_Namespace*		ns = NULL;
	struct cmd*			c = cmds;
	struct interp_cx*	l = NULL;

#if USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL)
		return TCL_ERROR;
#endif

	g_pagesize = sysconf(_SC_PAGESIZE);

	//ns = Tcl_CreateNamespace(interp, NS, NULL, NULL);
	//TEST_OK_LABEL(finally, code, Tcl_Export(interp, ns, "*", 0));

	// Set up interp_cx {{{
	l = (struct interp_cx*)ckalloc(sizeof *l);
	*l = (struct interp_cx){0};
	Tcl_SetAssocData(interp, "jitc", free_interp_cx, l);

	for (int i=0; i<LIT_SIZE; i++)
		replace_tclobj(&l->lit[i], Tcl_NewStringObj(lit_str[i], -1));


	TEST_OK_LABEL(finally, code, Tcl_EvalObjEx(interp, l->lit[LIT_TCLSTUBLIB_CMD], 0));
	replace_tclobj(&l->tclstublib, Tcl_GetObjResult(interp));
	TEST_OK_LABEL(finally, code, Tcl_EvalObjEx(interp, l->lit[LIT_TCLVER_CMD], 0));
	replace_tclobj(&l->tclver, Tcl_GetObjResult(interp));

	l->instance_head.next = &l->instance_tail;
	l->instance_tail.prev = &l->instance_head;
	// Set up interp_cx }}}

	while (c->name) {
		Tcl_Command r = NULL;

		if (c->nrproc) {
			r = Tcl_NRCreateCommand(interp, c->name, c->proc, c->nrproc, l, NULL);
		} else {
			r = Tcl_CreateObjCommand(interp, c->name, c->proc, l, NULL);
		}
		if (r == NULL) {
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

//}}}
DLLEXPORT int Jitc_Unload(Tcl_Interp* interp, int flags) //{{{
{
	int					code = TCL_OK;

	Tcl_DeleteAssocData(interp, "jitc");	// Have to do this here, otherwise Tcl will try to call it after we're unloaded
	if (flags == TCL_UNLOAD_DETACH_FROM_PROCESS) {
		//fprintf(stderr, "jitc unloading, finalizing mutexes\n");
		Tcl_MutexFinalize(&gdb_jit_mutex);
		gdb_jit_mutex = NULL;
		Tcl_MutexFinalize(&g_tcc_mutex);
		g_tcc_mutex = NULL;
	} else {
		//fprintf(stderr, "jitc detaching from interp\n");
		// TODO: remove commands
	}

	return code;
}

//}}}
#ifdef __cplusplus
}
#endif
