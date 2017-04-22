#include "mangle.h"
#define MPL_DCT64 MANGLECALL(dct64_3dnowext)
#define SYNTH_NAME ASM_NAME(synth_1to1_3dnowext_asm)
#include "synth_sse3d.h"

NONEXEC_STACK
