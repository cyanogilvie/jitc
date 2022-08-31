
/* !BEGIN!: Do not edit below this line. */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exported function declarations:
 */

/* 0 */
EXTERN int		Jitc_GetSymbolFromObj(Tcl_Interp*interp, Tcl_Obj*obj,
				Tcl_Obj*symbol, void**val);
/* 1 */
EXTERN int		Jitc_GetSymbolsFromObj(Tcl_Interp*interp,
				Tcl_Obj*obj, Tcl_Obj**symbols);

typedef struct JitcStubs {
    int magic;
    void *hooks;

    int (*jitc_GetSymbolFromObj) (Tcl_Interp*interp, Tcl_Obj*obj, Tcl_Obj*symbol, void**val); /* 0 */
    int (*jitc_GetSymbolsFromObj) (Tcl_Interp*interp, Tcl_Obj*obj, Tcl_Obj**symbols); /* 1 */
} JitcStubs;

extern const JitcStubs *jitcStubsPtr;

#ifdef __cplusplus
}
#endif

#if defined(USE_JITC_STUBS)

/*
 * Inline function declarations:
 */

#define Jitc_GetSymbolFromObj \
	(jitcStubsPtr->jitc_GetSymbolFromObj) /* 0 */
#define Jitc_GetSymbolsFromObj \
	(jitcStubsPtr->jitc_GetSymbolsFromObj) /* 1 */

#endif /* defined(USE_JITC_STUBS) */

/* !END!: Do not edit above this line. */
