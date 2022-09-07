package require jitclib::obstackpool
namespace eval ::jitclib {
	variable json [list use $::jitclib::obstackpool filter {jitc::re2c --case-ranges --conditions --tags --utf8} code { //@begin=c@<<<
		#include <stdint.h>

		/*
		#ifndef Tcl_ObjFetchInternalRep
		#define Tcl_ObjInternalRep						Tcl_ObjIntRep
		#define Tcl_ObjFetchInternalRep(obj, type)		Tcl_FetchIntRep(obj, type);
		#define Tcl_ObjStoreInternalRep(obj, type, ir)	Tcl_StoreIntRep(obj, type, ir);
		#endif
		*/

		struct mtag {
			struct mtag		*head;
			struct mtag		*next;
			ptrdiff_t		dist;
		};
		//@end=c@@begin=c@
		enum jsontype {
			JSON_OBJECT,
			JSON_ARRAY,
			JSON_STRING,
			JSON_NUMBER,
			JSON_TRUE,
			JSON_FALSE,
			JSON_NULL
		};
		//@end=c@@begin=c@
		struct escapes {
			struct mtag*	escape;
			struct mtag*	trail_f;
			struct mtag*	trail_t;
		};

		struct child {
			struct jsonval*	key;
			struct jsonval*	val;
			struct child*	next;
		};

		struct jsonval {
			enum jsontype	type;
			ptrdiff_t		from;
			ptrdiff_t		to;
			struct escapes*	escapes;
			Tcl_Obj*		native;
			struct child*	children;
			struct child*	lastchild;
			struct jsonval*	parent;
		};

		struct jitc_json_intrep {
			Tcl_Obj*		str;
			struct jsonval*	root;
			struct obstack*	ob;
		};

		void freeIntRep_jitc_json(Tcl_Obj* obj);
		void dupIntRep_jitc_json(Tcl_Obj* src, Tcl_Obj* dup);
		void updateString_jitc_json(Tcl_Obj* obj);

		Tcl_ObjType	jitc_json = {
			"jitc_json",
			freeIntRep_jitc_json,
			dupIntRep_jitc_json,
			updateString_jitc_json,
			NULL
		};

		void freeIntRep_jitc_json(Tcl_Obj* obj)
		{
			Tcl_ObjInternalRep*			ir = Tcl_FetchInternalRep(obj, &jitc_json);
			struct jitc_json_intrep*	r = ir->twoPtrValue.ptr1;
			struct obstack*				ob = r->ob;

			replace_tclobj(&r->str, NULL);
			// TODO: walk the tree releasing native Tcl_Objs
			r->root = NULL;
			r->ob = NULL;
			obstack_pool_release(ob);
			ob = NULL;
		}

		void dupIntRep_jitc_json(Tcl_Obj* src, Tcl_Obj* dup)
		{
			// TODO: implement
			Tcl_GetString(src);
		}

		void updateString_jitc_json(Tcl_Obj* obj)
		{
			Tcl_DString	ds;

			Tcl_DStringInit(&ds);
			// TODO: walk the tree serializing the values
			// Copy to string rep
			Tcl_DStringFree(&ds);
		}

		static void add_mtag(struct mtag** mt, struct obstack* ob, ptrdiff_t dist)
		{
			struct mtag*	newmt = obstack_alloc(ob, sizeof(struct mtag));
			if (*mt) {
				*newmt = (struct mtag){
					.head	= (*mt)->head,
					.dist	= dist
				};
				(*mt)->next = newmt;
				*mt = newmt;
			} else {
				*mt = newmt;
				*newmt = (struct mtag){
					.head	= newmt,
					.dist	= dist
				};
			}
		}

		static Tcl_Obj* g_json  = NULL;
		static Tcl_Obj* g_parse = NULL;
		static Tcl_Obj* g_true  = NULL;
		static Tcl_Obj* g_false = NULL;

		INIT {
			replace_tclobj(&g_json,  Tcl_NewStringObj("JITC_JSON", -1));
			replace_tclobj(&g_parse, Tcl_NewStringObj("PARSE", -1));
			replace_tclobj(&g_true,  Tcl_NewWideIntObj(1));
			replace_tclobj(&g_false, Tcl_NewWideIntObj(0));
			return TCL_OK;
		}

		RELEASE {
			replace_tclobj(&g_json,  NULL);
			replace_tclobj(&g_parse, NULL);
			replace_tclobj(&g_true,  NULL);
			replace_tclobj(&g_false, NULL);
		}


		static int parse_json(Tcl_Interp* interp, Tcl_Obj* jsonObj, struct jitc_json_intrep** valPtr)
		{
			int							code = TCL_OK;
			const unsigned char*		m = NULL;
			/*!types:re2c*/
			int							cond = yycvalue;
			struct obstack*				ob = obstack_pool_get(OBSTACK_POOL_SMALL);
			struct jsonval*				root = NULL;
			struct jsonval*				container = NULL;
			int							len;
			Tcl_Obj*					strObj = NULL;
			replace_tclobj(&strObj, Tcl_DuplicateObj(jsonObj));
			const unsigned char*const	str = (const unsigned char*)Tcl_GetStringFromObj(strObj, &len);
			const unsigned char*		s = str;
			/*!stags:re2c format = "const unsigned char*		@@;\n"; */
			/*!mtags:re2c format = "struct mtag*	@@ = NULL;\n"; */
			const unsigned char			*f=NULL, *t=NULL;
			struct mtag					*escape=NULL, *trail_f=NULL, *trail_t=NULL;
			struct jsonval*				jv = NULL;

			#define YYMTAGP(mt)	add_mtag(&(mt), ob, s-str);
			#define YYMTAGN(mt)	add_mtag(&(mt), ob, -1);

			#define MAKE_JSON_STRING do {											\
				jv = obstack_alloc(ob, sizeof(*jv));				\
				*jv = (struct jsonval){.type=JSON_STRING, .from=f-str, .to=t-str};	\
				if (escape) {														\
					jv->escapes = obstack_alloc(ob, sizeof(struct escapes));		\
					jv->escapes->escape = escape;									\
					jv->escapes->trail_f = trail_f;									\
					jv->escapes->trail_t = trail_t;									\
					escape = trail_f = trail_t = NULL;								\
				}																	\
				if (root == NULL) root = jv;										\
			} while(0)
			#define MAKE_JSON_NUMBER do {											\
				jv = obstack_alloc(ob, sizeof(*jv));				\
				*jv = (struct jsonval){.type=JSON_NUMBER, .from=f-str, .to=t-str};	\
				if (root == NULL) root = jv;										\
			} while(0)
			#define MAKE_JSON_TRUE do {												\
				jv = obstack_alloc(ob, sizeof(*jv));				\
				*jv = (struct jsonval){.type=JSON_TRUE, .from=f-str, .to=t-str};	\
				if (root == NULL) root = jv;										\
			} while(0)
			#define MAKE_JSON_FALSE do {											\
				jv = obstack_alloc(ob, sizeof(*jv));				\
				*jv = (struct jsonval){.type=JSON_FALSE, .from=f-str, .to=t-str};	\
				if (root == NULL) root = jv;										\
			} while(0)
			#define MAKE_JSON_NULL do {												\
				jv = obstack_alloc(ob, sizeof(*jv));				\
				*jv = (struct jsonval){.type=JSON_NULL, .from=f-str, .to=t-str};	\
				if (root == NULL) root = jv;										\
			} while(0)
			#define ADD_TO_CONTAINER do {											\
				if (container->type == JSON_OBJECT) {								\
					container->lastchild->val = jv;									\
					cond = yycobjectnext;											\
				} else {															\
					struct child*	child = obstack_alloc(ob, sizeof(*child));		\
					*child = (struct child){.val=jv};								\
					if (container->lastchild) {										\
						container->lastchild->next = child;							\
					} else {														\
						container->children = child;								\
					}																\
					container->lastchild = child;									\
					cond = yycarraynext;											\
				}																	\
			} while(0);

			#define GETCOND()	cond
			#define SETCOND(c)	(cond = c)

		loop:
			/*!re2c
				re2c:yyfill:enable			= 0;
				re2c:define:YYCTYPE			= "unsigned char";
				re2c:define:YYCURSOR		= "s";
				re2c:define:YYMARKER		= "m";
				re2c:define:YYGETCONDITION	= "GETCOND";
				re2c:define:YYSETCONDITION	= "SETCOND";

				end			= [\x00];
				character	= [\x20-\u10ffff] \ [\\"];
				digit		= [0-9];
				digit1		= [1-9];
				hexdigit	= [0-9a-fA-F];
				ws			= [ \x0a\x0d\x09]*;
				true		= ws @f "true"  @t ws;
				false		= ws @f "false" @t ws;
				null		= ws @f "null"  @t ws;
				integer		= digit1 digit* | "0";
				fraction	= "." digit+;
				sign		= "-" | "+";
				exponent	= 'e' sign? digit+;
				number		= ws @f "-"? integer fraction? exponent? @t ws;
				escape		= [\\] ( ["\\/bfnrt] | "u" hexdigit{4} );
				string		= ws ["] @f character* ( #escape escape #trail_f character* #trail_t)* @t ["] ws;

				<value>		string	{
										MAKE_JSON_STRING;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}
				<value>		number	{
										MAKE_JSON_NUMBER;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}
				<value>		true	{
										MAKE_JSON_TRUE;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}
				<value>		false	{
										MAKE_JSON_FALSE;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}
				<value>		null	{
										MAKE_JSON_NULL;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}
				<value>	ws "{" ws	{
										struct jsonval*	jv = obstack_alloc(ob, sizeof(*jv));
										*jv = (struct jsonval){ .type = JSON_OBJECT, .parent = container };
										container = jv;
										if (root == NULL) root = jv;
										cond = yyckey;
										goto loop;
									}
				<value> ws "[" ws	=> value {
										struct jsonval*	jv = obstack_alloc(ob, sizeof(*jv));
										*jv = (struct jsonval){ .type = JSON_ARRAY, .parent = container };
										container = jv;
										if (root == NULL) root = jv;
										goto loop;
									}

				<key>	string ":" ws	=> value {
										struct child*	child = obstack_alloc(ob, sizeof(*child));;
										MAKE_JSON_STRING;
										*child = (struct child){.key=jv};
										if (container->lastchild) {
											container->lastchild->next = child;
										} else {
											container->children = child;
										}
										container->lastchild = child;
										goto loop;
									}

				<objectnext> ","	:=> key
				<objectnext> "}"	{
										container = container->parent;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}

				<arraynext> ","		:=> value
				<arraynext> "]"		{
										container = container->parent;
										if (container) {ADD_TO_CONTAINER;} else {cond = yycterm;}
										goto loop;
									}

				<term> ws end		{ goto done; }

				<*> *				{
										if (interp) {
											const ptrdiff_t		ofs = s-str;
											Tcl_SetObjErrorCode(interp, Tcl_NewListObj(4, (Tcl_Obj*[]){
												g_json, g_parse, jsonObj, Tcl_NewWideIntObj(ofs)
											}));
											THROW_PRINTF_LABEL(done, code,
												"Invalid JSON character at offset %ld",	ofs);
										} else {
											code = TCL_ERROR;
											goto done;
										}
									}
			*/
		done:
			if (code == TCL_OK) {
				*valPtr = obstack_alloc(ob, sizeof **valPtr);
				**valPtr = (struct jitc_json_intrep){
					.root	= root,
					.ob		= ob
				};
				replace_tclobj(&((*valPtr)->str), strObj);
			} else {
				obstack_pool_release(ob);
			}

			replace_tclobj(&strObj, NULL);

			return code;
		}


		int Jitcjson_GetJSONFromObj(Tcl_Interp* interp, Tcl_Obj* json, struct jsonval** val)
		{
			int							code = TCL_OK;
			Tcl_ObjInternalRep*			ir = Tcl_FetchInternalRep(json, &jitc_json);
			struct jitc_json_intrep*	r = NULL;

			if (ir) {
				r = ir->twoPtrValue.ptr1;
			} else {
				TEST_OK_LABEL(done, code, parse_json(interp, json, &r));
				Tcl_StoreInternalRep(json, &jitc_json, &(Tcl_ObjInternalRep){
					.twoPtrValue.ptr1 = r
				});
			}

			*val = r->root;

		done:
			return code;
		}


		OBJCMD(valid) {
			struct jitc_json_intrep*	val = NULL;
			CHECK_ARGS(1, "json");

			const int isvalid = (TCL_OK == parse_json(NULL, objv[1], &val));
			if (val) {
				struct obstack*	ob = val->ob;
				val->root = NULL;
				replace_tclobj(&val->str, NULL);
				val = NULL;
				obstack_pool_release(ob);
				ob = NULL;
			}
			Tcl_SetObjResult(interp, isvalid ? g_true : g_false);
			return TCL_OK;
		}
	//@end=c@>>> }]
	variable json_check [list debug /tmp/jitc_json_check options {-Wall -gdwarf-5} use $::jitclib::obstackpool filter {jitc::re2c -i --case-ranges --conditions --tags --utf8} code { //@begin=c@<<<
		static Tcl_Obj* g_true  = NULL;
		static Tcl_Obj* g_false = NULL;

		INIT {
			replace_tclobj(&g_true,  Tcl_NewWideIntObj(1));
			replace_tclobj(&g_false, Tcl_NewWideIntObj(0));
			return TCL_OK;
		}

		//@end=c@@begin=c@
		RELEASE {
			replace_tclobj(&g_true,  NULL);
			replace_tclobj(&g_false, NULL);
		}

		static int check_json(Tcl_Obj* jsonObj)
		{
			int							res;
			const unsigned char*		m = NULL;
			/*!types:re2c*/
			int							cond = yycvalue;
			int							len;
			const unsigned char*const	str = (const unsigned char*)Tcl_GetStringFromObj(jsonObj, &len);
			const unsigned char*		s = str;
			Tcl_DString					containerstack;

			Tcl_DStringInit(&containerstack);

			#define GETCOND()	cond
			#define SETCOND(c)	(cond = c)

		loop:
			/*!re2c
				re2c:yyfill:enable			= 0;
				re2c:define:YYCTYPE			= "unsigned char";
				re2c:define:YYCURSOR		= "s";
				re2c:define:YYMARKER		= "m";
				re2c:define:YYGETCONDITION	= "GETCOND";
				re2c:define:YYSETCONDITION	= "SETCOND";

				end			= [\x00];
				character	= [\x20-\u10ffff] \ [\\"];
				digit		= [0-9];
				digit1		= [1-9];
				hexdigit	= [0-9a-fA-F];
				ws			= [ \x0a\x0d\x09]*;
				true		= ws "true"  ws;
				false		= ws "false" ws;
				null		= ws "null"  ws;
				integer		= digit1 digit* | "0";
				fraction	= "." digit+;
				sign		= "-" | "+";
				exponent	= 'e' sign? digit+;
				number		= ws "-"? integer fraction? exponent? ws;
				escape		= [\\] ( ["\\/bfnrt] | "u" hexdigit{4} );
				string		= ws ["] character* ( escape character* )* ["] ws;

				<value>		string	{
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value>		number	{
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value>		true	{
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value>		false	{
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value>		null	{
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value>	ws "{" ws "}" ws	{
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value>	ws "{" ws	{
										Tcl_DStringAppend(&containerstack, "o", 1);
										cond = yyckey;
										goto loop;
									}
				<value> ws "[" ws "]" ws	{
					// Marker:FOO
										const int stacklen = Tcl_DStringLength(&containerstack);
										if (stacklen) { cond = (Tcl_DStringValue(&containerstack)[stacklen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}
				<value> ws "[" ws	=> value {
										Tcl_DStringAppend(&containerstack, "a", 1);
										goto loop;
									}

				<key>	string ":" ws	:=> value

				<objectnext> ","	:=> key
				<objectnext> "}" ws	{
										const int newlen = Tcl_DStringLength(&containerstack)-1;
										Tcl_DStringSetLength(&containerstack, newlen);
										if (newlen) { cond = (Tcl_DStringValue(&containerstack)[newlen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}

				<arraynext> ","		:=> value
				<arraynext> "]" ws	{
										const int newlen = Tcl_DStringLength(&containerstack)-1;
										Tcl_DStringSetLength(&containerstack, newlen);
										if (newlen) { cond = (Tcl_DStringValue(&containerstack)[newlen-1] == 'o') ? yycobjectnext : yycarraynext; } else { cond = yycterm; }
										goto loop;
									}

				<term> ws end		{ res = 1; goto done; }

				<*> *				{ 
										res = 0;
										const ptrdiff_t		ofs = s-str-1;
										//fprintf(stderr, "Invalid JSON character at offset %ld",	ofs);
										Tcl_Panic("Invalid JSON character at offset %ld",	ofs);
										goto done;
									}
			*/
		done:
			Tcl_DStringFree(&containerstack);

			return res;
		}


		OBJCMD(check) {
			CHECK_ARGS(1, "json");
			Tcl_SetObjResult(interp, check_json(objv[1]) ? g_true : g_false);
			return TCL_OK;
		}
	//@end=c@>>> }]
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
