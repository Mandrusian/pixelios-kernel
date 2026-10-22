#pragma once

/* Cold re-init of the gs101 UFS host + device link after ABL's shutdown, up to
 * "link is up, host list not yet programmed" (docs/research/07 steps 3-17).
 * Writes only the documented controller/PHY/UniPro registers and pulses the
 * device RST_n; no SCSI/UFS command is sent. Returns 0 on success, otherwise
 * a stage number (see ufsinit.c). bluejay only; other boards return -1. */
int ufs_cold_init(void);
