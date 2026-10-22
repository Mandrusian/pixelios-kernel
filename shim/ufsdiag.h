#pragma once

/* Read-only survey of the gs101 UFS vendor state ABL left behind, plus a few
 * UIC DME_GET queries (the only writes are the UIC command argument/issue
 * registers). Every register read is fault-guarded; the PHY block is read
 * last. bluejay only. */
void ufs_vendor_diag(void);
