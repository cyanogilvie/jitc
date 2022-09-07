
/* !BEGIN!: Do not edit below this line. */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exported function declarations:
 */

/* 0 */
EXTERN int		Jitc_GetSymbolFromObj(Tcl_Interp*interp,
				Tcl_Obj*cdef, Tcl_Obj*symbol, void**val);
/* 1 */
EXTERN int		Jitc_GetSymbolsFromObj(Tcl_Interp*interp,
				Tcl_Obj*cdef, Tcl_Obj**symbols);
/* 2 */
EXTERN int		Jitc_GetExportHeadersFromObj(Tcl_Interp*interp,
				Tcl_Obj*cdef, Tcl_Obj**headers);
/* 3 */
EXTERN int		Jitc_GetExportSymbolsFromObj(Tcl_Interp*interp,
				Tcl_Obj*cdef, Tcl_Obj**symbols);

typedef struct JitcStubs {
    int magic;
    void *hooks;

    int (*jitc_GetSymbolFromObj) (Tcl_Interp*interp, Tcl_Obj*cdef, Tcl_Obj*symbol, void**val); /* 0 */
    int (*jitc_GetSymbolsFromObj) (Tcl_Interp*interp, Tcl_Obj*cdef, Tcl_Obj**symbols); /* 1 */
    int (*jitc_GetExportHeadersFromObj) (Tcl_Interp*interp, Tcl_Obj*cdef, Tcl_Obj**headers); /* 2 */
    int (*jitc_GetExportSymbolsFromObj) (Tcl_Interp*interp, Tcl_Obj*cdef, Tcl_Obj**symbols); /* 3 */
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
#define Jitc_GetExportHeadersFromObj \
	(jitcStubsPtr->jitc_GetExportHeadersFromObj) /* 2 */
#define Jitc_GetExportSymbolsFromObj \
	(jitcStubsPtr->jitc_GetExportSymbolsFromObj) /* 3 */

#endif /* defined(USE_JITC_STUBS) */

/* !END!: Do not edit above this line. */
