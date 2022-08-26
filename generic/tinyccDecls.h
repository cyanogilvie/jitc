
/* !BEGIN!: Do not edit below this line. */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exported function declarations:
 */

/* 0 */
EXTERN int		Tinycc_GetSymbolFromObj(Tcl_Interp*interp,
				Tcl_Obj*obj, Tcl_Obj*symbol, void**val);
/* 1 */
EXTERN int		Tinycc_GetSymbolsFromObj(Tcl_Interp*interp,
				Tcl_Obj*obj, Tcl_Obj**symbols);

typedef struct TinyccStubs {
    int magic;
    void *hooks;

    int (*tinycc_GetSymbolFromObj) (Tcl_Interp*interp, Tcl_Obj*obj, Tcl_Obj*symbol, void**val); /* 0 */
    int (*tinycc_GetSymbolsFromObj) (Tcl_Interp*interp, Tcl_Obj*obj, Tcl_Obj**symbols); /* 1 */
} TinyccStubs;

extern const TinyccStubs *tinyccStubsPtr;

#ifdef __cplusplus
}
#endif

#if defined(USE_TINYCC_STUBS)

/*
 * Inline function declarations:
 */

#define Tinycc_GetSymbolFromObj \
	(tinyccStubsPtr->tinycc_GetSymbolFromObj) /* 0 */
#define Tinycc_GetSymbolsFromObj \
	(tinyccStubsPtr->tinycc_GetSymbolsFromObj) /* 1 */

#endif /* defined(USE_TINYCC_STUBS) */

/* !END!: Do not edit above this line. */
