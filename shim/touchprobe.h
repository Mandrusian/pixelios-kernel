#pragma once
#include <stdint.h>

/*
 * touchprobe.h - Pixel 6a touchscreen SPI11 / USI11 bring-up experiment WITHOUT booting iOS
 * (build.sh --touch-probe[=run]).
 * --touch-probe=railsr / rails add the ACPM client of acpm.c (read / set the touch rails LDO25M, LDO26M). A register-level port of the XNU driver
 * xnu/patches/0088-sweet-gs101-touch.patch (+0089 CMU_PERIC1 clock chain, +0090 variants A-E, +0092 CMU_TOP gate)
 * as it runs with boot-args `sweettouchclk=3 sweettouchvar=1`.
 *
 * touch_probe_dump(): phase 1, READ-ONLY (CMU_PERIC1 + CMU_TOP, USI11 mode, pad banks, USI/SPI registers).
 * touch_probe_run(opts): phase 2, the writes of that boot-arg set plus the docs/research/21 experiments chosen by TP_O_*;
 *                     only called by --touch-probe=run[,...].
 * Every constant comes from the generated TOUCH_* board macros (tools/boards.py <- soc.json / board.json, the same
 * keys tools/mkadt.py puts into the /sweet-touch node). Output lines are the XNU driver's "SWEET TOUCH: ..." lines
 * (label "SWEET", text "TOUCH: ..."); long XNU lines are split at spaces to fit the 96-byte RAM-log line.
 *
 * Pure decoders live here so shim/touch_host_test.c can test them.
 */

/* SPI engine layout, spi-s3c64xx.c (0088 patch :235-:257) */
#define TP_SPI_CH_CFG		0x00u
#define TP_SPI_CLK_CFG		0x04u
#define TP_SPI_MODE_CFG		0x08u
#define TP_SPI_CS_REG		0x0cu
#define TP_SPI_INT_EN		0x10u
#define TP_SPI_STATUS		0x14u
#define TP_SPI_TX_DATA		0x18u
#define TP_SPI_RX_DATA		0x1cu
#define TP_SPI_PACKET_CNT	0x20u
#define TP_SPI_PENDING_CLR	0x24u
#define TP_SPI_SWAP_CFG		0x28u
#define TP_SPI_FB_CLK		0x2cu
#define TP_CH_SW_RST		(1u << 5)
#define TP_CH_RXCH_ON		(1u << 1)
#define TP_CH_TXCH_ON		(1u << 0)
#define TP_CH_HS_EN		(1u << 6)
#define TP_PKT_CNT_EN		(1u << 16)
#define TP_MODE_TRAILCNT	(0x3ffu << 19)
#define TP_PND_ERR_CLR		0x1eu
#define TP_CLK_ENCLK		(1u << 8)	/* 0090 variant A */
#define TP_CLK_PSR_MASK		0xffu
#define TP_CS_AUTO		(1u << 1)	/* 0090 variant E */
#define TP_CS_NSC_CNT_2		(2u << 4)
#define TP_USI_CON		0x04u
#define TP_USI_CON_RESET	(1u << 0)
#define TP_USI_OPTION		0x08u
#define TP_USI_OPT_CLKREQ	(1u << 1)
#define TP_USI_OPT_CLKSTOP	(1u << 2)
#define TP_SPI_FIFO_BYTES	64u
#define TP_NVARIANTS		5u		/* A..E, tried in this order */
#define TP_BURST_TIMEOUT_US	20000u		/* 0088 tg_spi_burst */
#define TP_MUX_BUSY_TIMEOUT_US	1000u		/* 0089 tg_cmu_enable */
#define TP_MODE_LOOPBACK	(1u << 3)	/* S3C64XX_SPI_MODE_SELF_LOOPBACK, docs/research/21 X3 */
#define TP_FTM5_CMD_FIFO_READ	0x87u		/* 0088 sweet_ftm5.h */
#define TP_FTM5_EVENT_SIZE	8u
#define TP_FTM5_EVT_CONTROLLER_READY 0x03u

/* Build options (build.sh --touch-probe=run,mux,cs,osc|top,loop,rst,misopu,railsr|rails), docs/research/21 section 3 */
#define TP_O_MUX	1u	/* drive the TS_SPI_SEL pad low if the AP does not (XNU sweettouchmux=1) */
#define TP_O_CS		2u	/* X1: ABL-exact manual chip select, variant F */
#define TP_O_OSC	4u	/* X2: USI11 clock from oscclk, divider 5 */
#define TP_O_TOP	8u	/* X6: USI11 clock from the TOP source, divider 9 */
#define TP_O_LOOP	16u	/* X3: self-loopback */
#define TP_O_RST	32u	/* X4: pull-ups off, reset pulse, controller-ready poll */
#define TP_O_MISOPU	64u	/* X5: MISO pull-up during the identify */
#define TP_O_RAILSR	128u	/* ACPM: READ the two touch rail enable registers (LDO25M/LDO26M, docs/research/23); no SPI writes */
#define TP_O_RAILS	256u	/* ACPM: UPDATE bit 7 of both (set, never clear), read back; implies railsr */
#define TP_O_EXP	(TP_O_CS | TP_O_OSC | TP_O_TOP | TP_O_LOOP | TP_O_RST | TP_O_MISOPU)

#define TP_MAX_POLLS		100000u		/* belt and braces: a poll loop ends even if the timer is dead */

#define TP_ST_RX_LVL(v)		(((v) >> 15) & 0x1ffu)
#define TP_ST_TX_LVL(v)		(((v) >> 6) & 0x1ffu)

/* FTM5 chip-id read, sweet_ftm5.h (0088 :32-:43) */
#define TP_FTM5_CMD_HW_REG_R	0xFBu
#define TP_FTM5_ADDR_DCHIP_ID	0x20000000u
#define TP_FTM5_DUMMY_BYTES	1u
#define TP_FTM5_SPI_DELAY_CS_US	10u

#ifdef TOUCH_CMU_BIT_GATE_MANUAL
/* 0089 tg_cmu_gate: manual gate open = MANUAL 1, CG_VAL 1, automatic gating 0. */
static inline uint32_t tp_gate_open(uint32_t v)
{
	return (v | (1u << TOUCH_CMU_BIT_GATE_MANUAL) | (1u << TOUCH_CMU_BIT_GATE_VAL)) &
	       ~(1u << TOUCH_CMU_BIT_GATE_AUTO);
}
#define TP_GATE_CHECK_MASK ((1u << TOUCH_CMU_BIT_GATE_MANUAL) | (1u << TOUCH_CMU_BIT_GATE_VAL) | \
			    (1u << TOUCH_CMU_BIT_GATE_AUTO))
#endif

/* ftm5_chip_id_match: 1 = same order, 2 = swapped, 0 = no match */
static inline int tp_chip_id_match(const uint8_t *got, const uint8_t *want)
{
	if (got[0] == want[0] && got[1] == want[1])
		return 1;
	if (got[0] == want[1] && got[1] == want[0])
		return 2;
	return 0;
}

/* Fault-guarded MMIO (shim.c; host tests substitute a fake window). */
int try_read32(uintptr_t address, uint32_t *value);
int try_write32(uintptr_t address, uint32_t value);
uint64_t touch_ticks(void);	/* CNTPCT (shim.c) */

void touch_probe_dump(void);	/* phase 1, read-only */
int touch_probe_dvfs(unsigned step);	/* ACPM DVFS (TOUCH_DVFS): 0 describe channel 1 (reads), 1 FREQ_GET all domains, 2 FREQ_REQ DVFS_KHZ0..2 + read back; 0 = ok */
int touch_probe_rails(unsigned step);	/* ACPM rails (TOUCH_RAILS): 0 describe (reads), 1 read both regs, 2 update + read back; 0 = ok */
void touch_probe_run(unsigned opts);	/* phase 2: the sweettouchclk=3 sweettouchvar=1 writes + the TP_O_* experiments */
