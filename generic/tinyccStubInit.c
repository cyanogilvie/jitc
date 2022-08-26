#include "tinycc.h"

/* !BEGIN!: Do not edit below this line. */

const TinyccStubs tinyccStubs = {
    TCL_STUB_MAGIC,
    0,
    Tinycc_GetSymbolFromObj, /* 0 */
    Tinycc_GetSymbolsFromObj, /* 1 */
};

/* !END!: Do not edit above this line. */


const TinyccStubs* TinyccStubsPtr = NULL;
MODULE_SCOPE const TinyccStubs* const tinyccConstStubsPtr;
const TinyccStubs* const tinyccConstStubsPtr = &tinyccStubs;
