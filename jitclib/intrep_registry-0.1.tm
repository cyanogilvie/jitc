package require jitclib::tip445

namespace eval ::jitclib {
	variable intrep_registry [list use $tip445 {*}{
		options		{-Wall -Werror -g}
		export {
			symbols {
				intrep_registry_init
				intrep_registry_free
				intrep_register
				intrep_forget
			}
			header { //@begin=c@
				struct intrep_registry {
					Tcl_HashTable	intreps;
				};
				//@end=c@@begin=c@

				void intrep_registry_init(struct intrep_registry* r);
				void intrep_registry_free(struct intrep_registry* r);
				void intrep_register(struct intrep_registry* r, Tcl_Obj* obj);
				void intrep_forget(struct intrep_registry* r, Tcl_Obj* obj);
			//@end=c@}
		}
		code { //@begin=c@

			void intrep_registry_init(struct intrep_registry* r) //<<<
			{
				Tcl_InitHashTable(&r->intreps, TCL_ONE_WORD_KEYS);
			}

			//@end=c@@begin=c@>>>
			void intrep_registry_free(struct intrep_registry* r) //<<<
			{
				Tcl_HashSearch	search;
				Tcl_HashEntry*	he;
				while ((he = Tcl_FirstHashEntry(&r->intreps, &search))) {
					Tcl_Obj*	obj = Tcl_GetHashKey(&r->intreps, he);
					Tcl_GetString(obj);
					Tcl_FreeInternalRep(obj);
				}
				Tcl_DeleteHashTable(&r->intreps);
			}

			//@end=c@@begin=c@>>>
			void intrep_register(struct intrep_registry* r, Tcl_Obj* obj) //<<<
			{
				int				isnew;
				Tcl_HashEntry*	he = Tcl_CreateHashEntry(&r->intreps, obj, &isnew);
				if (!isnew) Tcl_Panic("intrep already registered for %p\n", obj);
				Tcl_SetHashValue(he, obj);
			}

			//@end=c@@begin=c@>>>
			void intrep_forget(struct intrep_registry* r, Tcl_Obj* obj) //<<<
			{
				Tcl_HashEntry*	he = Tcl_FindHashEntry(&r->intreps, obj);
				if (!he) Tcl_Panic("intrep not registered for %p\n", obj);
				Tcl_DeleteHashEntry(he);
			}

			//@end=c@@begin=c@>>>

		//@end=c@}
	}]
}
# vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
