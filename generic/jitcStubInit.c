#include "jitc.h"

/* !BEGIN!: Do not edit below this line. */

const JitcStubs jitcStubs = {
    TCL_STUB_MAGIC,
    0,
    Jitc_GetSymbolFromObj, /* 0 */
    Jitc_GetSymbolsFromObj, /* 1 */
};

/* !END!: Do not edit above this line. */


const JitcStubs* JitcStubsPtr = NULL;
MODULE_SCOPE const JitcStubs* const jitcConstStubsPtr;
const JitcStubs* const jitcConstStubsPtr = &jitcStubs;
