#!/usr/bin/env python3
"""Board definitions for the bare-metal shim, payload and ADT builders.

The data lives next to the hardware it describes, AOSP style:

  soc/<vendor>/<soc>/soc.json            what every device with that SoC shares
  device/<vendor>/<codename>/board.json  one device: names its SoC, adds what differs

A device's board is its SoC's values overlaid with its own. Keys other than the build
constants (name, codename, soc, status, marketing, notes) are metadata. Integers may be
written as "0x..." strings. Run `tools/pix devices` to list them.

  boards.py BOARD OUT.h          write the generated C header
  boards.py BOARD --get KEY      print one value (shell use), e.g. soc_dir
"""
import glob
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
META_KEYS = {"name", "codename", "soc", "status", "marketing", "notes"}


def _num(v):
    if isinstance(v, str):
        try:
            return int(v, 0)
        except ValueError:
            return v		# e.g. a CPU compatible string
    if isinstance(v, list):
        items = [_num(x) for x in v]
        return tuple(items)
    return v


def _load(path):
    with open(path) as f:
        return json.load(f)


def load_devices(root=ROOT):
    """Return {codename: (device_dir, soc_dir, meta, values)}."""
    out = {}
    for path in sorted(glob.glob(os.path.join(root, "device", "*", "*", "board.json"))):
        dev = _load(path)
        ddir = os.path.dirname(path)
        sdir = os.path.join(root, "soc", *dev["soc"].split("/"))
        soc = _load(os.path.join(sdir, "soc.json"))
        merged = dict(soc)
        merged.update(dev)
        values = {k: _num(v) for k, v in merged.items() if k not in META_KEYS}
        out[dev["codename"]] = (ddir, sdir, merged, values)
    return out


_DEVICES = load_devices()
BOARDS = {name: v[3] for name, v in _DEVICES.items()}


def device_dir(board):
    return os.path.relpath(_DEVICES[board][0], ROOT)


def soc_dir(board):
    return os.path.relpath(_DEVICES[board][1], ROOT)


def _touch_macros(v):
    """TOUCH_* macros for shim/touchprobe.c: the same soc.json/board.json keys tools/mkadt.py puts in /sweet-touch
    (xnu patches 0088/0089/0092 read them from the ADT), so the shim probe and the XNU driver cannot disagree."""
    out = []

    def d(name, value):
        out.append(f"#define TOUCH_{name} {value:#x}UL")

    usi = v["touch_usi"]
    d("SPI_BASE", v[f"usi{usi}_spi_base"])
    d("USI_OFF", v["usi_regs_offset"])
    d("USI_MODE_REG", v[f"usi{usi}_mode_reg"])
    d("USI_MODE_SPI", v["usi_mode_spi"])
    d("USI_MODE_MASK", v["usi_mode_mask"])
    d("SPI_FUNC", v["touch_spi_pin_func"])
    d("CHIP_ID0", v["touch_chip_id"][0])
    d("CHIP_ID1", v["touch_chip_id"][1])
    for pin in ("clk", "mosi", "miso", "cs", "irq", "reset", "mux"):
        bank, idx = v[f"touch_pin_{pin}"]
        d(f"PIN_{pin.upper()}_BANK", v[f"{bank}_bank"])
        d(f"PIN_{pin.upper()}_NR", idx)
    c = v["cmu_peric1"]
    d("CMU_BASE", int(c["base"], 0))
    for k, off in c["offsets"].items():
        d(f"CMU_{k.upper()}", int(off, 0))
    for k, bit in c["bits"].items():
        d(f"CMU_BIT_{k.upper()}", bit)
    t = c["top"]
    d("CMUTOP_BASE", int(t["base"], 0))
    for k in ("mux", "div", "gate"):
        d(f"CMUTOP_{k.upper()}", int(t[k], 0))
    d("CMUTOP_GATE_BIT", t["gate_bit"])
    return out


# Constants of the ACPM client that come from the Google kernel (android-gs-bluejay-5.10-android13-qpr3), pinned here so that an edit
# of soc.json alone cannot move a write address or the PMIC target: changing one means changing this table (and its citation) too.
_ACPM_PINS = {
    "mbox_base": 0x17610000, "sram_base": 0x02039000, "sram_size": 0x40000, "initdata_off": 0xA000,  # gs101.dtsi:496-499
    "ch_id": 2,                                                              # gs101-pmic.dtsi:38 (acpm-ipc-channel)
    "speedy_ch": 0, "pmic_addr": 1,                                          # s2mpg10-core.c:30,58 (S2MPG10: channel 0, type 1)
    "intgr1": 0x40, "intcr0": 0x24, "intsr0": 0x2c,                          # acpm_ipc.h:106-116 (APM_INTGR = INTGR1, AP_INTCR, AP_INTSR)
    "desc_stride": 0x48,                                                     # framework.h:97-133 (guess until layout_verified)
}
_ACPM_RAIL_REGS = (0x5C, 0x5D)  # S2MPG10_PM_L25M_CTRL / L26M_CTRL, enable = bit 7 (gk_s2mpg10-register.h:108-109, regulator.c:275-278)


def _acpm_check(a):
    """Fail the build if the acpm block moved a pinned constant, or if a rail's voltage-code window is unknown or wrong while
    layout_verified / elem_unit_verified is 1. The window must equal what the regulator group (min + step * code) and the device-tree
    limits give: code = (uV - group_min_uv) / step_uv, 0..63 (vsel_mask 63)."""
    for k, want in _ACPM_PINS.items():
        if _num(a[k]) != want:
            raise ValueError(f"acpm.{k} = {a[k]!r}: pinned to {want:#x} (tools/boards.py _ACPM_PINS, cited there); change the pin and its citation together")
    if _num(a["elem_unit"]) not in (1, 4) or _num(a["layout_verified"]) not in (0, 1) or _num(a["elem_unit_verified"]) not in (0, 1):
        raise ValueError("acpm.elem_unit must be 1 (bytes) or 4 (words); the verified flags 0 or 1")
    if len(a["rails"]) != len(_ACPM_RAIL_REGS):
        raise ValueError("acpm.rails: expected the two touch rails LDO25M and LDO26M")
    armed = _num(a["layout_verified"]) or _num(a["elem_unit_verified"])
    for r, reg in zip(a["rails"], _ACPM_RAIL_REGS):
        if _num(r["reg"]) != reg:
            raise ValueError(f"acpm.rails {r['name']}: reg {r['reg']!r}, pinned to {reg:#x}")
        lo, hi = r["vsel_min"], r["vsel_max"]
        if lo is None or hi is None:
            if lo is not None or hi is not None or armed:
                raise ValueError(f"acpm.rails {r['name']}: vsel_min/vsel_max are null (or half set) while layout_verified/elem_unit_verified is 1: "
                                 "the rail voltage gate would be open")
            continue
        gmin, step = _num(r["group_min_uv"]), _num(r["step_uv"])
        codes = []
        for key in ("dt_min_uv", "dt_max_uv"):
            code, rem = divmod(_num(r[key]) - gmin, step)
            if rem or not 0 <= code <= 63:
                raise ValueError(f"acpm.rails {r['name']}: {key} {r[key]} is not group_min_uv + step_uv * (0..63)")
            codes.append(code)
        if (_num(lo), _num(hi)) != tuple(codes):
            raise ValueError(f"acpm.rails {r['name']}: vsel_min/vsel_max {lo}/{hi} != {codes[0]}/{codes[1]} derived from group_min_uv, step_uv and the DT limits")


# DVFS (docs/research/25): channel and domain indexes are pinned like the rest of the ACPM block.
_ACPM_DVFS_CH = 1                                     # gs101.dtsi:720 (exynos_dm acpm-ipc-channel = <1>)
_ACPM_DVFS_DOMS = {"CPUCL0": 2, "CPUCL1": 3, "CPUCL2": 4}   # dt-bindings/clock/gs101.h:305-307 (GET_IDX = id & 0xFFFF)
_ACPM_DVFS_MAXLV = 24


def _acpm_dvfs_check(a):
    """The rate table of a domain must be strictly decreasing kHz (the fvmap order, observed run fvmap1), the ceiling one of its rows."""
    dv = a["dvfs"]
    if _num(dv["ch_id"]) != _ACPM_DVFS_CH:
        raise ValueError(f"acpm.dvfs.ch_id = {dv['ch_id']!r}: pinned to {_ACPM_DVFS_CH} (tools/boards.py _ACPM_DVFS_CH, cited there)")
    names = [d["name"] for d in dv["domains"]]
    if names != list(_ACPM_DVFS_DOMS):
        raise ValueError(f"acpm.dvfs.domains: expected exactly {list(_ACPM_DVFS_DOMS)} in that order, got {names}")
    for d in dv["domains"]:
        if _num(d["idx"]) != _ACPM_DVFS_DOMS[d["name"]]:
            raise ValueError(f"acpm.dvfs {d['name']}: idx {d['idx']!r}, pinned to {_ACPM_DVFS_DOMS[d['name']]}")
        r = d["rates_khz"]
        if not 2 <= len(r) <= _ACPM_DVFS_MAXLV or any(not isinstance(x, int) or x <= 0 for x in r) or any(x <= y for x, y in zip(r, r[1:])):
            raise ValueError(f"acpm.dvfs {d['name']}: rates_khz must be 2..{_ACPM_DVFS_MAXLV} strictly decreasing positive integers")
        if d["ceiling_khz"] not in r:
            raise ValueError(f"acpm.dvfs {d['name']}: ceiling_khz {d['ceiling_khz']} is not one of the table rows")


def dvfs_requests(board, csv):
    """'K0,K1,K2' (kHz, 0 = leave that cluster) -> three ints, each 0 or a row of the domain's table at or below its ceiling.
    The shim re-checks the same in acpm.c; refusing here keeps a bad request out of the image altogether."""
    doms = BOARDS[board]["acpm"]["dvfs"]["domains"]
    vals = [int(x) for x in csv.split(",")]
    if len(vals) != len(doms):
        raise ValueError(f"--dvfs needs {len(doms)} comma separated kHz values (CL0,CL1,CL2; 0 = leave), got {len(vals)}")
    for d, v in zip(doms, vals):
        if v != 0 and (v not in d["rates_khz"] or v > d["ceiling_khz"]):
            raise ValueError(f"--dvfs {d['name']}: {v} kHz is not a firmware table row at or below the ceiling {d['ceiling_khz']} "
                             f"(rows: {d['rates_khz']})")
    return vals


def _acpm_macros(v):
    """ACPM_* macros for shim/acpm.c (soc.json "acpm", docs/research/23). null vsel_min/vsel_max = unknown (VSEL_KNOWN 0)."""
    a = v["acpm"]
    _acpm_check(a)
    out = []

    def n(name, value):
        out.append(f"#define ACPM_{name} {_num(value)}")

    def h(name, value):
        out.append(f"#define ACPM_{name} {_num(value):#x}UL")

    h("MBOX_BASE", a["mbox_base"])
    h("SRAM_BASE", a["sram_base"])
    h("SRAM_SIZE", a["sram_size"])
    h("INITDATA_OFF", a["initdata_off"])
    n("CH_ID", a["ch_id"])
    n("SPEEDY", a["speedy_ch"])
    n("PMIC_ADDR", a["pmic_addr"])
    for k in ("intgr1", "intcr0", "intsr0", "desc_stride"):
        h(k.upper(), a[k])
    n("LAYOUT_VERIFIED", a["layout_verified"])
    n("ELEM_UNIT", a["elem_unit"])
    n("ELEM_UNIT_VERIFIED", a["elem_unit_verified"])
    for i, r in enumerate(a["rails"]):
        n(f"RAIL{i}_REG", r["reg"])
        known = r["vsel_min"] is not None and r["vsel_max"] is not None
        n(f"RAIL{i}_VSEL_KNOWN", 1 if known else 0)
        n(f"RAIL{i}_VSEL_MIN", r["vsel_min"] if known else 0)
        n(f"RAIL{i}_VSEL_MAX", r["vsel_max"] if known else 0)
    if "dvfs" in a:
        _acpm_dvfs_check(a)
        doms = a["dvfs"]["domains"]
        n("DVFS_CH", a["dvfs"]["ch_id"])
        n("DVFS_N", len(doms))
        n("DVFS_MAXLV", _ACPM_DVFS_MAXLV)
        out.append("#define ACPM_DVFS_IDXS " + ", ".join(str(d["idx"]) for d in doms))
        out.append("#define ACPM_DVFS_CEILS " + ", ".join(str(d["ceiling_khz"]) for d in doms))
        out.append("#define ACPM_DVFS_NRS " + ", ".join(str(len(d["rates_khz"])) for d in doms))
        out.append("#define ACPM_DVFS_ROWS " + ", ".join("{ " + ", ".join(str(x) for x in d["rates_khz"]) + " }" for d in doms))
    return out


def c_header(board: str) -> str:
    values = BOARDS[board]
    names = {
        "GICD_BASE": "gicd_base", "GICD_SIZE": "gicd_size",
        "GICR_BASE": "gicr_base", "GICR_SIZE": "gicr_size",
        "GICR_PE_SIZE": "gicr_stride", "VTIMER_INTID": "vtimer_intid",
        "TIMER_FREQ": "timer_freq",
        "FB_BASE": "fb_base", "FB_SIZE": "fb_size", "FB_W": "fb_width",
        "FB_H": "fb_height", "FB_STRIDE": "fb_stride", "UFS_BASE": "ufs_base",
        "UFS_PRDT_BYTE_GRAN": "ufs_prdt_byte_gran", "UART_BASE": "uart_base",
        "DECON_BASE": "decon_base", "NR_CPUS": "cpu_count",
        "PSCI_SYSTEM_RESET": "psci_system_reset",
    }
    lines = ["/* Generated from tools/boards.py; do not edit. */", "#pragma once"]
    for macro, key in names.items():
        value = 8 if key == "cpu_count" else values[key]
        lines.append(f"#define {macro} {value}" if key == "cpu_count" else f"#define {macro} {value:#x}UL")
    for i, mpidr in enumerate(values["cpu_mpidrs"]):
        lines.append(f"#define BOARD_MPIDR_{i} {mpidr:#x}UL")
    watchdogs = values["watchdog_bases"] + (0, 0)
    lines.append(f"#define WDT_CL0_BASE {watchdogs[0]:#x}UL")
    lines.append(f"#define WDT_CL1_BASE {watchdogs[1]:#x}UL")
    # SoC family macros: code paths are selected by SoC, so siblings (Pixel 6 / 6 Pro share gs101
    # with the 6a) need no source change. BOARD_BLUEJAY is kept as an alias for out-of-tree users.
    this_soc = _DEVICES[board][2]["soc"].split("/")[1].upper()
    for name in sorted({os.path.basename(os.path.dirname(p)).upper()
                        for p in glob.glob(os.path.join(ROOT, "soc", "*", "*", "soc.json"))}):
        lines.append(f"#define SOC_{name} {1 if name == this_soc else 0}")
    for key, addr in sorted(values.get("wlan", {}).items()):
        lines.append(f"#define WLAN_{key.upper()} {int(addr, 0):#x}UL")
    for key, addr in sorted(values.get("gpu", {}).items()):
        lines.append(f"#define GPU_{key.upper()} {int(addr, 0):#x}UL")
    if "touch_chip_id" in values:
        lines += _touch_macros(values)
    if "acpm" in values:
        lines += _acpm_macros(values)
    lines.append(f"#define BOARD_BLUEJAY {1 if this_soc == 'GS101' else 0}")
    lines.append(f"#define BOOT_HEADER_VERSION {values['boot_header_version']}")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    import argparse
    from pathlib import Path

    parser = argparse.ArgumentParser()
    parser.add_argument("board", choices=BOARDS)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--get", metavar="KEY", help="print device_dir | soc_dir | any board value")
    parser.add_argument("--dvfs-check", metavar="K0,K1,K2", help="validate a --dvfs request, print the three values")
    args = parser.parse_args()
    if args.dvfs_check is not None:
        try:
            print(" ".join(str(x) for x in dvfs_requests(args.board, args.dvfs_check)))
        except (ValueError, KeyError) as exc:
            raise SystemExit(f"error: {exc}")
    elif args.get:
        if args.get == "device_dir":
            print(device_dir(args.board))
        elif args.get == "soc_dir":
            print(soc_dir(args.board))
        elif args.get == "soc":
            print(_DEVICES[args.board][2]["soc"])
        else:
            print(BOARDS[args.board][args.get])
    else:
        args.output.write_text(c_header(args.board))
