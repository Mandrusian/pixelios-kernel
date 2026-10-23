#pragma once
#include <stdint.h>

void test_ufs(void);
void test_ufs_bluejay(uintptr_t dtb);

#ifdef UDUMP
/* Read-only userdata dump over the USB gadget (--udump): the GPT
 * 'userdata' match found by the winning UFS attempt, a reader bounded
 * to that match, and the 64 KiB record staging buffer (4 KiB aligned,
 * one dump chunk). */
#define UFS_UDUMP_STAGE_LEN	(16u * 4096u)
int ufs_userdata_found(uint64_t *first, uint64_t *last);
uint32_t ufs_read_userdata(uint64_t lba, uint32_t nblocks, void *buf);
extern uint8_t ufs_udump_stage[UFS_UDUMP_STAGE_LEN];
#endif
