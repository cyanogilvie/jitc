namespace eval ::jitclib {
	variable asciilower {
		options		{-Wall -Werror}
		filter		{jitc::re2c -W --case-ranges --no-debug-info}
		export {
			symbols {
				GetAsciiLowerFromObj
			}
			header {
				Tcl_Obj* GetAsciiLowerFromObj(Tcl_Obj* obj);
			}
		}
		code {//@begin=c@<<<
			Tcl_HashTable	g_intreps;

			static void freeIntRep_asciilower(Tcl_Obj* obj);

			Tcl_ObjType objtype_asciilower = {
				"asciilower",
				freeIntRep_asciilower,
				NULL,
				NULL,
				NULL
			};

			static int	cleanup = 0;

			//@end=c@@begin=c@
			static void freeIntRep_asciilower(Tcl_Obj* obj) //<<<
			{
				Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(obj, &objtype_asciilower);
				Tcl_Obj*				lower = ir->ptrAndLongRep.ptr;
				replace_tclobj(&lower, NULL);
				if (!cleanup) {
					Tcl_HashEntry*	he = Tcl_FindHashEntry(&g_intreps, obj);
					if (he) Tcl_DeleteHashEntry(he);
				}
			}

			//@end=c@@begin=c@>>>
			static void free_intreps() //<<<
			{
				Tcl_HashSearch	search;

				cleanup = 1;
				for (Tcl_HashEntry* he=Tcl_FirstHashEntry(&g_intreps, &search); he; he=Tcl_NextHashEntry(&search)) {
					Tcl_Obj*	obj = Tcl_GetHashValue(he);
					Tcl_GetString(obj);
					Tcl_FreeInternalRep(obj);
					Tcl_DeleteHashEntry(he);
				}
			}

			//@end=c@@begin=c@>>>
			Tcl_Obj* GetAsciiLowerFromObj(Tcl_Obj* obj) //<<<
			{
				Tcl_ObjInternalRep*		ir = Tcl_FetchInternalRep(obj, &objtype_asciilower);

				if (ir) {
					return (Tcl_Obj*)ir->ptrAndLongRep.ptr;
				} else {
					const char*		str = Tcl_GetString(obj);
					const char*		cur = str;
					const char		*mar, *tok;
					Tcl_DString		ds;
					Tcl_DStringInit(&ds);
					for (;;) {
						tok = cur;
						/*!local:re2c:ascii_lowercase_intrep
						re2c:api:style			= free-form;
						re2c:yyfill:enable		= 0;
						re2c:define:YYCTYPE		= char;
						re2c:define:YYCURSOR	= cur;
						re2c:define:YYMARKER	= mar;

						end			= [\x00];
						upper		= [A-Z];
						pass		= [^] \ end \ upper;

						upper	{ char l=tok[0]|(1<<5); Tcl_DStringAppend(&ds, &l, 1);        continue; }
						pass+	{                       Tcl_DStringAppend(&ds, tok, cur-tok); continue; }

						end {
							Tcl_Obj*	lower = NULL;
							int			isnew;

							replace_tclobj(&lower, Tcl_DStringToObj(&ds));

							Tcl_HashEntry*	he = Tcl_CreateHashEntry(&g_intreps, obj, &isnew);
							Tcl_SetHashValue(he, obj);
							Tcl_StoreInternalRep(obj, &objtype_asciilower, &(Tcl_ObjInternalRep){
								.ptrAndLongRep.ptr = lower	// Donate our ref
							});

							return lower;
						}

						*/
					}
				}
			}

			//>>>
			INIT { //<<<
				Tcl_InitHashTable(&g_intreps, TCL_ONE_WORD_KEYS);
				return TCL_OK;
			}

			//>>>
			RELEASE { //<<<
				free_intreps();
				Tcl_DeleteHashTable(&g_intreps);
			}

			//>>>
			OBJCMD(asciilower) //<<<
			{
				CHECK_ARGS(1, "str");
				Tcl_SetObjResult(interp, GetAsciiLowerFromObj(objv[1]));
				return TCL_OK;
			}

			//>>>
		//@end=c@>>>}
	}
}

# vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
