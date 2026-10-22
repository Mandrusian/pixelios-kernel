/* sm7150 (sweet): ABL leaves UFS usable, no cold re-init needed; keeps the shim's interface satisfied. */
#include "ufsinit.h"

int ufs_cold_init(void) { return -1; }
