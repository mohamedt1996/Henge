/* host/gunyah_fixup.S once more, with the generic timer's traps for the Mac's clock at 24 MHz (0.3.121).
   A file of its own so that the fix-up every other start uses stays byte for byte what it was. */
#define HV_FIX_TB 1
#include "gunyah_fixup.S"
