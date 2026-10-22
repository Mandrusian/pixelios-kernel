#!/usr/bin/env python3
"""Boot-image and read-only-safety checks for the bluejay probe shim.

check_ufs_safety() is the host-side enforcement of docs/SAFETY.md's
"no UFS write" rule. The primary control is compile-time: scsi() in
shim/ufs.c takes enum scsi_dir, which has no write value, and derives the
UPIU flags and UTRD DD bits from it alone; enum upiu_type gates the
transaction types to NOP OUT (v2 liveness probe) and COMMAND. The scan
below is the second layer: it rejects write-type SCSI opcodes and
query-write opcodes however they are spelled (hex/decimal, assignment,
initializer) in the CDB opcode slot, rejects any flags-byte literal
outside {0x00, 0x40}, rejects any transaction-type literal outside
{0x00 NOP OUT, 0x01 COMMAND}, and rejects the QUERY REQUEST UPIU type
byte anywhere. ufs_safety_selftest() feeds known-bad sources (the five
review-2 bypasses plus NOP-era pointer-form bypasses) and asserts each
is caught, so the scan cannot silently rot again.

check_usb_safety() enforces the stage-1 USB probe's (shim/usbprobe.c
AND usbprobe.h) "STRICTLY read-only" rule: no store to a register
address however spelled - write32/try_write32-style helper identifiers,
kernel writel spellings, *(volatile ...)= dereference stores, stores
through declared volatile pointers/arrays (x[i]=, *x=, x->f=), and
str-to-MMIO inline asm - plus a blanket ban on asm/__asm,
memcpy/memmove/memset and __builtin_mem*/__atomic*/__sync* tokens, which
can all hide an MMIO store from the spelling checks above. Read-only
volatile pointer declarations are not stores (review-1 H1 false
positive): declaration statements are blanked before the store patterns
run. usb_safety_selftest() feeds known-bad sources and asserts each is
caught, same style as the UFS checker.

check_usb_object() is the primary gate (review-1 H1): an OBJECT audit of
the built shim/out/usbprobe.o via aarch64-none-elf-objdump -dr/-h and
nm -u. Undefined symbols must stay within the probe's sanctioned
external surface, .data/.bss must be empty, smc/hvc/msr/dc/ic/at/tlbi
and the LSE atomic families (stadd/cas/swp/...) are banned, and every
store's base register must be provably stack-derived (sp; add/sub/mov
chains off sp or a spill slot holding such a register; or the x0 dst
parameter inside static put_* helpers). Anything the audit cannot
classify is an error - it fails closed. usb_object_selftest() feeds
synthetic bad objects (non-stack store, smc, LSE, stack pivots,
unclassifiable instructions, ...) and asserts each is rejected on
every run.
"""
import argparse
import os
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile

SCSI_WRITE_OPCODES = {
    0x0A: "WRITE(6)",
    0x2A: "WRITE(10)",
    0xAA: "WRITE(12)",
    0x8A: "WRITE(16)",
    0x42: "UNMAP",
    0x04: "FORMAT UNIT",
    0x41: "WRITE SAME(10)",
    0x93: "WRITE SAME(16)",
}

UFS_QUERY_WRITE_OPCODES = {
    0x02: "Set Flag",
    0x04: "Clear Flag",
    0x06: "Toggle Flag",
    0x07: "Write Attribute",
    0x08: "Write Descriptor",
}

# Bytes that must never appear in a CDB opcode slot (CDB[0], the first
# element of a CDB brace initializer, or the ucd.cmd[16+i] UPIU slot).
FORBIDDEN_OPCODE_BYTES = {
    **SCSI_WRITE_OPCODES,
    **UFS_QUERY_WRITE_OPCODES,
    0x16: "QUERY REQUEST UPIU",
}

# The only UPIU data-direction flag bytes a read-only probe may contain:
# 0x00 (no data) and 0x40 (R, data-in). 0x20 (W) is a write.
UPIU_READ_FLAGS = {0x00, 0x40}

# The only UPIU transaction types the probe may issue: NOP OUT (path
# liveness test, no SCSI layer) and COMMAND (carries the read-only CDBs).
# QUERY REQUEST and DATA OUT are the UFS write paths and are not allowed.
UPIU_ALLOWED_TYPES = {0x00: "NOP OUT", 0x01: "COMMAND"}

# Matches the command-UPIU header byte writes whether the code spells the
# command descriptor as ucd.cmd[...] or through a pointer, u->cmd[...].
_CMD_SLOT = r"(?:ucd\s*\.\s*cmd|\w+\s*->\s*cmd)"

_INT_RE = re.compile(r"(?<![\w.])(0[xX][0-9a-fA-F]+|\d+)[uUlL]*")


def _parse_int(token: str) -> int:
    try:
        return int(token, 0)
    except ValueError:
        return int(token, 8)  # C octal like 042


def _bare_literal(rhs: str):
    """Return the value of rhs if it is nothing but an integer literal
    (optionally parenthesized/cast), else None. Used so the opcode scans
    flag constant assignments like `w[0] = 0x2a` without tripping on
    register arithmetic like `(1u << 28) | ((x ? 2u : 0u) << 25)`."""
    rhs = re.sub(r"\b(?:u?int(?:8|16|32|64)?_t|char|signed|unsigned|const|volatile)\b", " ", rhs)
    rhs = re.sub(r"[()\s]+", "", rhs)
    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", rhs)
    return _parse_int(m.group(1)) if m else None


def _int_literals(text: str):
    for m in _INT_RE.finditer(text):
        yield _parse_int(m.group(1)), m.group(1)


def _strip_c_comments_and_strings(src: str) -> str:
    """Blank out // and /* */ comments and string/char literals so only
    executable tokens remain for the opcode scan."""
    out = []
    i, n = 0, len(src)
    while i < n:
        two = src[i : i + 2]
        if two == "/*":
            end = src.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif two == "//":
            end = src.find("\n", i)
            i = n if end < 0 else end
            out.append(" ")
        elif src[i] in ('"', "'"):
            quote = src[i]
            j = i + 1
            while j < n and src[j] != quote:
                j += 2 if src[j] == "\\" else 1
            i = j + 1 if j < n else n
            out.append(quote + quote)
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def check_ufs_safety_text(src: str, name: str = "ufs.c") -> None:
    text = _strip_c_comments_and_strings(src)

    # The compile-time control must be present: scsi() plumbing without the
    # read-only direction enum means someone rewrote the interface, and the
    # UPIU type enum gates the transaction types to NOP OUT and COMMAND.
    assert "enum scsi_dir" in text and "SCSI_DIR_READ" in text, (
        f"{name}: read-only direction enum missing from the scsi() plumbing"
    )
    assert "enum upiu_type" in text and "UPIU_NOP_OUT" in text and "UPIU_COMMAND" in text, (
        f"{name}: upiu type enum missing; NOP OUT and COMMAND are the only allowed types"
    )

    # (a) First element of any array brace initializer is a CDB opcode slot
    # when the array is a CDB; catch hex and decimal spellings there.
    for m in re.finditer(r"\w+\s*\[\s*[0-9]*\s*\]\s*=\s*\{", text):
        end = text.find("}", m.end())
        if end < 0:
            continue
        first_element = text[m.end() : end].split(",")[0]
        for value, tok in _int_literals(first_element):
            if value in FORBIDDEN_OPCODE_BYTES:
                raise AssertionError(
                    f"{name}: forbidden opcode {tok} "
                    f"({FORBIDDEN_OPCODE_BYTES[value]}) as first CDB element in initializer"
                )

    # (b) Any index-0 assignment: x[0] = <bare opcode constant>, however
    # the array was built (assignment instead of an initializer).
    for m in re.finditer(r"\w+\s*\[\s*0\s*\]\s*=(?!=)\s*([^;]*)", text):
        value = _bare_literal(m.group(1))
        if value in FORBIDDEN_OPCODE_BYTES:
            raise AssertionError(
                f"{name}: forbidden opcode {value:#04x} "
                f"({FORBIDDEN_OPCODE_BYTES[value]}) in a CDB[0] assignment"
            )

    # (c) The CDB area of the command UPIU itself: ucd.cmd[16(+i)] = <bare
    # opcode constant>, in object or pointer spelling.
    for m in re.finditer(_CMD_SLOT + r"\s*\[\s*16(?:\s*\+\s*\w+)?\s*\]\s*=(?!=)\s*([^;]*)", text):
        value = _bare_literal(m.group(1))
        if value in FORBIDDEN_OPCODE_BYTES:
            raise AssertionError(
                f"{name}: forbidden opcode {value:#04x} "
                f"({FORBIDDEN_OPCODE_BYTES[value]}) written into the CDB area of the command UPIU"
            )

    # (d) UPIU data-direction flags byte: every literal assigned to the
    # command UPIU flags byte (object or pointer spelling) must be a
    # read/none flag. Catches 0x20 in direct, ternary, and OR-assignment
    # forms, in hex or decimal.
    for m in re.finditer(_CMD_SLOT + r"\s*\[\s*1\s*\]\s*([^;]*)", text):
        for value, tok in _int_literals(m.group(1)):
            if value not in UPIU_READ_FLAGS:
                raise AssertionError(
                    f"{name}: command UPIU flags literal {tok} is neither 0x00 nor 0x40; "
                    "the UPIU write flag is 0x20 and no other direction may be requested"
                )

    # (f) UPIU transaction type byte: every literal in the type slot
    # (object or pointer spelling) must be NOP OUT or COMMAND. The NOP OUT
    # type was allow-listed for the v2 liveness probe; QUERY REQUEST and
    # DATA OUT stay out.
    for m in re.finditer(_CMD_SLOT + r"\s*\[\s*0\s*\]\s*([^;]*)", text):
        for value, tok in _int_literals(m.group(1)):
            if value not in UPIU_ALLOWED_TYPES:
                raise AssertionError(
                    f"{name}: command UPIU transaction-type literal {tok} is not NOP OUT "
                    "(0x00) or COMMAND (0x01)"
                )

    # (e) No QUERY REQUEST UPIU may be built at all (any hex spelling).
    assert not re.search(r"0[xX]16\b", text), (
        f"{name}: QUERY REQUEST UPIU (0x16) found; this probe must not issue UFS queries"
    )


def check_ufs_safety(ufs_c_path: pathlib.Path) -> None:
    assert ufs_c_path.is_file(), f"ufs.c not found: {ufs_c_path}"
    check_ufs_safety_text(ufs_c_path.read_text(encoding="utf-8"), str(ufs_c_path))

def ufs_safety_selftest() -> int:
    """Feed known-bad sources to the checker and assert each is caught;
    a clean control source must pass. Returns the count caught."""
    benign = (
        "enum scsi_dir { SCSI_DIR_NONE = 0, SCSI_DIR_READ = 1 };\n"
        "enum upiu_type { UPIU_NOP_OUT = 0x00, UPIU_COMMAND = 0x01 };\n"
        "uint8_t cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };\n"
        "cdb[0] = 0x25;\n"
        "ucd.cmd[0] = 0x01;\n"
        "ucd.cmd[1] = dir == SCSI_DIR_READ ? 0x40 : 0x00;\n"
        "ucd.cmd[16 + i] = cdb[i];\n"
        "u->cmd[0] = type;\n"
        "u->cmd[1] = dir == SCSI_DIR_READ ? 0x40 : 0x00;\n"
        "u->cmd[16 + i] = cdb[i];\n"
    )
    cases = [
        ("UPIU write flag 0x20 assigned to ucd.cmd[1]", "ucd.cmd[1] = 0x20;\n"),
        ("UPIU write flag 0x20 hidden in a ternary", "ucd.cmd[1] = len ? 0x20 : 0x00;\n"),
        ("UPIU write flag 0x20 via pointer spelling", "u->cmd[1] = 0x20;\n"),
        ("decimal WRITE(10) opcode 42 in a CDB initializer", "uint8_t w[6] = { 42, 0, 0, 0, 0, 0 };\n"),
        ("hex WRITE(10) opcode built by CDB[0] assignment", "uint8_t w[6]; w[0] = 0x2a;\n"),
        ("hex WRITE(10) opcode in a CDB initializer", "uint8_t w[6] = { 0x2A, 0, 0, 0, 0, 0 };\n"),
        ("UPIU transaction type 0x0F (not NOP OUT/COMMAND)", "u->cmd[0] = 0x0f;\n"),
        ("QUERY REQUEST UPIU 0x16 as transaction type", "ucd->cmd[0] = 0x16;\n"),
    ]
    check_ufs_safety_text(benign, "selftest control")  # must not raise
    caught = 0
    for label, bad in cases:
        try:
            check_ufs_safety_text(benign + bad, f"selftest: {label}")
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: {label} -> {exc}")
        else:
            raise AssertionError(f"ufs safety self-test FAILED to catch: {label}")
    print(f"PASS ufs safety self-test: {caught}/{len(cases)} known-bad sources rejected")
    return caught


# Matches a direct store through a cast dereference: the lvalue (and any
# index) sits between the cast's closing paren and the assignment.
_VOLATILE_DEREF_STORE = re.compile(
    r"\*\s*\(\s*volatile\b[^)]*\)\s*(?:\[[^\]]*\]\s*)?[^;=]*=(?!=)"
)

# Matches any write32-family or kernel MMIO-write identifier.
_MMIO_WRITE_ID = re.compile(
    r"\b\w*write32\w*\b|\b\w*writel\w*\b|\biowrite\w*\b|\boutsw?\b|\bout[lwd]\b"
)

# Matches an inline-asm statement containing an AArch64 store mnemonic
# (checked on comment-stripped text with string literals intact, since
# the mnemonic lives inside the asm string).
_ASM_STORE = re.compile(
    r"\b(?:__asm__|asm)\b[^;]*?\b(?:str[bh]?|stp|stlr\w*|sttr\w*|stur\w*|stxp?)\b"
)


def _strip_c_comments_keep_strings(src: str) -> str:
    """Blank out // and /* */ comments but keep string literals (the asm
    store-mnemonic scan needs the literal contents)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        two = src[i : i + 2]
        if two == "/*":
            end = src.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif two == "//":
            end = src.find("\n", i)
            i = n if end < 0 else end
            out.append(" ")
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def _volatile_pointer_vars(text: str):
    """Names declared as pointers to volatile ('volatile T *name;',
    'volatile T *const name;', including function PARAMETERS - ')' also
    terminates the declarator) or volatile arrays ('volatile T name[N];')
    - the lvalue spellings a store to a register window could hide
    behind."""
    names = set()
    for m in re.finditer(r"\bvolatile\b[^;()=]*?\*\s*(?:const\s+)?(\w+)\s*(?:=|;|,|\))", text):
        names.add(m.group(1))
    for m in re.finditer(r"\bvolatile\b[^;()=]*?\b(\w+)\s*\[[^\]]*\]\s*[;=]", text):
        names.add(m.group(1))
    return names

# Tokens that must not appear at all in the probe sources: inline asm
# (any spelling), the mem* family (a store behind a call), and compiler
# atomics (an RMW store). The object audit below catches their compiled
# forms; this ban catches them at the source level first (review-1 H1).
_USB_BANNED_SOURCE_TOKENS = re.compile(
    r"\b(?:asm|__asm|__asm__)\b"
    r"|\b(?:memcpy|memmove|memset)\b"
    r"|\b__builtin_mem\w*"
    r"|\b__atomic\w*"
    r"|\b__sync\w*"
)


def check_usb_safety_text(src: str, name: str = "usbprobe.c") -> None:
    """The stage-1 USB probe must contain no store to a register address.
    Reject write32/try_write32 identifiers, kernel writel spellings,
    *(volatile ...)= stores, stores through declared volatile pointers
    or arrays, and str-to-MMIO inline asm."""
    code = _strip_c_comments_and_strings(src)
    code_keep = _strip_c_comments_keep_strings(src)

    m = _MMIO_WRITE_ID.search(code)
    if m:
        raise AssertionError(
            f"{name}: MMIO store identifier '{m.group(0)}' in a read-only USB probe"
        )

    m = _USB_BANNED_SOURCE_TOKENS.search(code_keep)
    if m:
        raise AssertionError(
            f"{name}: banned token '{m.group(0)}' in a read-only USB probe "
            "(asm / mem* / atomics can hide an MMIO store)"
        )

    if _VOLATILE_DEREF_STORE.search(code):
        raise AssertionError(
            f"{name}: '*(volatile ...)= ' store in a read-only USB probe"
        )

    # A declaration cannot contain a store, but its declarator looks like
    # one ('*r = <init>', 'regs[N] = {...}') - matching it rejected
    # read-only declarations like 'volatile uint32_t *r = ...; x = *r;'
    # (review-1 H1 false positive). Blank declaration statements, then
    # scan only the remaining (executable) text for stores.
    body = code
    for var in sorted(_volatile_pointer_vars(code)):
        body = re.sub(
            rf"\bvolatile\b[^;{{}}=]*\*+\s*(?:const\s+)?{re.escape(var)}\b[^;{{}}]*;?",
            " ",
            body,
        )
        body = re.sub(
            rf"\bvolatile\b[^;{{}}=]*\b{re.escape(var)}\s*\[[^\];{{}}]*\]\s*(?:=[^;{{}}]*)?;",
            " ",
            body,
        )
    for var in sorted(_volatile_pointer_vars(code)):
        for pat in (
            rf"\b{re.escape(var)}\s*\[[^\]]*\]\s*=(?!=)",
            rf"\*\s*{re.escape(var)}\s*=(?!=)",
            rf"\b{re.escape(var)}\s*->\s*\w+\s*=(?!=)",
        ):
            m = re.search(pat, body)
            if m:
                raise AssertionError(
                    f"{name}: store through volatile pointer '{var}' "
                    f"('{m.group(0).strip()}') in a read-only USB probe"
                )

    m = _ASM_STORE.search(code_keep)
    if m:
        raise AssertionError(
            f"{name}: inline-asm store instruction in a read-only USB probe"
        )


def check_usb_safety(usb_c_path: pathlib.Path) -> None:
    assert usb_c_path.is_file(), f"usbprobe.c not found: {usb_c_path}"
    check_usb_safety_text(usb_c_path.read_text(encoding="utf-8"), str(usb_c_path))


def usb_safety_selftest() -> int:
    """Feed known-bad store spellings to the USB checker and assert each
    is caught; the real probe-shaped control must pass."""
    benign = (
        "#include \"usbprobe.h\"\n"
        "struct usb_reg { int ok; uint32_t v; };\n"
        "static struct usb_reg usb_read(uintptr_t a)\n"
        "{ struct usb_reg r; r.ok = try_read32(a, &r.v); if (!r.ok) r.v = 0; return r; }\n"
        "static unsigned put_reg(char *dst, unsigned cap, unsigned pos,\n"
        "                        const char *key, const struct usb_reg *r)\n"
        "{ pos = put_txt(dst, cap, pos, key); pos = put_txt(dst, cap, pos, \"=\");\n"
        "  if (!r->ok) return put_txt(dst, cap, pos, \"unreadable\");\n"
        "  return put_hex(dst, cap, pos, r->v, 8); }\n"
        "char line[96]; unsigned p = 0;\n"
        "line[p] = 0;\n"
    )
    cases = [
        ("try_write32 helper", "try_write32(USBPROBE_DWC3_BASE + USBPROBE_DCTL_OFF, 0);\n"),
        ("write32 embedded in a longer identifier", "usb_write32(USBPROBE_DWC3_BASE, 0);\n"),
        ("kernel writel spelling", "writel(0, base + USBPROBE_GCTL_OFF);\n"),
        ("direct *(volatile uint32_t *)= store", "*(volatile uint32_t *)(base + off) = 1;\n"),
        ("store through a declared volatile pointer", "volatile uint32_t *r = (void *)base; r[4] = 1;\n"),
        ("member store through a volatile struct pointer", "volatile struct dwc3 *d = (void *)base; d->dctl = 1;\n"),
        ("str-to-MMIO inline asm", "__asm__ volatile(\"str %w0, [%1]\" : : \"r\"(v), \"r\"(a));\n"),
        ("volatile register-file array store", "volatile uint32_t regs[8]; regs[0] = 1;\n"),
        ("volatile pointer parameter store", "static void f(volatile uint32_t *r) { r[1] = 0; }\n"),
        ("volatile T *const pointer store", "volatile uint32_t *const p = (void *)base; *p = 1;\n"),
        ("inline __asm__ smc", "__asm__ volatile(\"smc #0\");\n"),
        ("plain asm keyword", "asm(\"str %w0, [%1]\" : : \"r\"(v), \"r\"(a));\n"),
        ("memcpy to a register window", "memcpy((void *)(base + off), &v, 4);\n"),
        ("__builtin_memset", "__builtin_memset((void *)base, 0, 4);\n"),
        ("__atomic_store_n", "__atomic_store_n((uint32_t *)base, 0, 0);\n"),
        ("__sync compare-and-swap", "__sync_val_compare_and_swap((int *)base, 0, 1);\n"),
    ]
    check_usb_safety_text(benign, "selftest control")  # must not raise
    # review-1 H1 false positive: a READ-ONLY volatile declaration must
    # not be rejected (the old scan matched the declarator '*r =').
    check_usb_safety_text(
        benign + "volatile uint32_t *ro = (void *)0x11110000;\nunsigned x = *ro;\n",
        "selftest control: read-only volatile declaration")  # must not raise
    caught = 0
    for label, bad in cases:
        try:
            check_usb_safety_text(benign + bad, f"selftest: {label}")
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: {label} -> {exc}")
        else:
            raise AssertionError(f"usb safety self-test FAILED to catch: {label}")
    print(f"PASS usb safety self-test: {caught}/{len(cases)} known-bad store spellings rejected")
    return caught

# ---- stage-1 USB probe: OBJECT-level read-only audit (review-1 H1) ----
#
# The source scan above is a spelling check; review-1 showed 21 of 22
# tried bypass spellings got through it. This audit checks the BUILT
# object (shim/out/usbprobe.o), so anything that compiles to a store is
# seen as a store, whatever the C spelling was:
#
#   (a) nm -u: undefined symbols must be a subset of the sanctioned
#       external surface below (extending it is a reviewable decision);
#   (b) objdump -h: .data and .bss must be empty (no writable static
#       state the probe could corrupt or be corrupted through);
#   (c) no smc/hvc/msr/dc/ic/at/tlbi (system-register and cache writes,
#       dc zva writes cache lines) and no LSE atomics (stadd/stset/cas/
#   swp/... all modify memory);
#   (d) every store instruction's base register must be provably the
#       stack: sp itself; a register whose definition on EVERY path from
#       the function entry is add/sub xN, sp, #imm|reg, a plain
#       mov xN, xM off such a register, an add/sub xN, xM, #|reg whose
#       base xM is such a register (loop-index addressing into a stack
#       buffer), a load from a spill slot that last received such a
#       register, or - only inside a STATIC put_* helper - a register of
#       the x0 dst-parameter chain. The proof is a per-basic-block
#       dataflow with intersection at joins (see _usb_audit_function).
#
# Fail closed: a store whose base cannot be proven, an unparseable
# operand, or ANY instruction outside the understood mnemonic families
# is an error, never a pass.

_USB_ALLOWED_UNDEF = {
    "try_read32",          # fault-guarded MMIO read (shim.c)
    "con_str",             # console/ramlog line out (console.c)
    "fdt_collect_memmap",  # reserved-memory walk (fdt.c)
    "pstore_zone_addr",    # validated ramoops zone base (pstore.c, L2)
    "pstore_zone_size",    # validated ramoops zone size (pstore.c, L2)
    "_head", "_end",       # linked image bounds (shim.ld)
}

_USB_BANNED_INSN = re.compile(
    r"^(?:smc|hvc|msr|dc|ic|at|tlbi|"
    r"(?:st|ld)(?:add|clr|eor|set|smax|smin|umax|umin)[al]?|"
    r"swp[al]?|cas[a-z]*)$"
)

# Mnemonics that write their first operand (a plain value, never a
# stack-derived pointer): the destination register loses any class.
# Includes the bitfield-insert aliases (ubfiz/sbfiz/bfi/bfxil) of the
# ubfm/sbfm family - same first-operand-only semantics.
_USB_WRITES_FIRST = {
    "and", "ands", "bic", "bics", "eor", "eon", "orr", "orn", "mvn",
    "lsl", "lsr", "asr", "ror", "ubfx", "sbfx", "ubfm", "sbfm",
    "ubfiz", "sbfiz", "bfi", "bfxil", "bfm",
    "sxtw", "sxtb", "sxth", "uxtw", "uxtb", "uxth", "mul", "mneg",
    "madd", "msub", "umull", "umaddl", "umulh", "smull", "sdiv",
    "udiv", "neg", "rev", "rev16", "rev32", "rbit", "clz", "cls",
    "extr", "csel", "csinc", "csinv", "csneg", "cset", "csetm", "crc32b", "crc32x",
    "cinc", "cinv", "cneg",
}
_USB_FLAGS_ONLY = {"cmp", "cmn", "tst", "ccmp"}
_USB_NO_WRITE = {"nop", "dsb", "dmb", "isb", "yield", "br", "blr", "ret"}

_FUNC_LABEL = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
_INSN_LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4,16})\s+(.+?)\s*$")
_RELOC_LINE = re.compile(r"^\s*[0-9a-f]+:\s+R_\w+")
_MEM_BASE = re.compile(r"\[\s*(sp|x\d+|w\d+|xzr|wzr)")
_SP_SLOT_MEM = re.compile(
    r"^\s*(?P<rs>[^\[\]]+?)\s*,\s*\[\s*sp\s*,\s*#(?P<off>-?(?:0[xX][0-9a-fA-F]+|\d+))\s*\]\s*$"
)
_SP_SLOT_LD = re.compile(r"\[\s*sp\s*,\s*#(-?(?:0[xX][0-9a-fA-F]+|\d+))\s*\]")


def _usb_reg(tok: str):
    """Canonical register name for tok ('w5' -> 'x5', 'sp', 'xzr'), or
    None when tok is not a plain register."""
    tok = tok.strip()
    if tok == "sp":
        return "sp"
    if tok in ("xzr", "wzr"):
        return "xzr"
    m = re.fullmatch(r"([xw])([12]?\d|30)", tok)
    return "x" + m.group(2) if m else None


def check_usb_object_text(disasm: str, local_text: set[str], undefined: set[str],
                          sections: str, name: str = "usbprobe.o",
                          *, allowed_undef: set[str] | None = None,
                          trusted_args: frozenset[str] = frozenset(),
                          allow_adrp: bool = False,
                          require_clean_data: bool = True) -> int:
    """Audit one probe-shaped object from objdump/nm text. Raises
    AssertionError on any violation; returns the number of stores
    audited (reachable blocks). Keyword arguments generalize the audit
    for the other shim objects (see check_usbproto_object_text); the
    defaults are the original usbprobe.o policy."""
    allowed = _USB_ALLOWED_UNDEF if allowed_undef is None else allowed_undef
    extra = undefined - allowed
    if extra:
        raise AssertionError(
            f"{name}: undefined symbols beyond the sanctioned surface "
            f"({', '.join(sorted(allowed))}): {', '.join(sorted(extra))}"
        )
    if require_clean_data:
        for sec in (".data", ".bss"):
            m = re.search(rf"^\s*\d+\s+{re.escape(sec)}\s+([0-9a-f]+)\s", sections, re.M)
            if m and int(m.group(1), 16) != 0:
                raise AssertionError(
                    f"{name}: {sec} is {int(m.group(1), 16)} bytes; the probe "
                    "must carry no writable static state"
                )

    # Split the disassembly into functions of (addr, mnemonic, ops, text).
    funcs: list[tuple[str, list[tuple[int, str, str, str]]]] = []
    cur_name: str | None = None
    cur_insns: list[tuple[int, str, str, str]] = []
    for raw in disasm.splitlines():
        m = _FUNC_LABEL.match(raw.strip())
        if m:
            if cur_name is not None:
                funcs.append((cur_name, cur_insns))
            cur_name, cur_insns = m.group(2), []
            continue
        if not raw.strip() or ": R_" in raw or _RELOC_LINE.match(raw):
            continue
        m = _INSN_LINE.match(raw)
        if not m:
            continue  # 'file format' / 'Disassembly of section' headers
        addr_s, _hex, rest = m.groups()
        rest = rest.split("//")[0].strip()
        parts = rest.split(None, 1)
        cur_insns.append((int(addr_s, 16), parts[0],
                          parts[1] if len(parts) > 1 else "", rest))
    if cur_name is not None:
        funcs.append((cur_name, cur_insns))

    func_entries = {insns[0][0] for _fname, insns in funcs if insns}
    total = 0
    for fname, insns in funcs:
        total += _usb_audit_function(fname, insns, local_text, name,
                                     func_entries, trusted_args, allow_adrp)
    return total


# Registers a call may clobber (AAPCS caller-saved, plus the link
# register). Callee-saved x19-x28, x29 and sp survive 'bl'/'blr'.
_USB_CALLER_SAVED = {f"x{i}" for i in range(19)} | {"x30"}


def _usb_branch_target(ops: str, name: str, fname: str, addr: int) -> int:
    for tok in ops.replace(",", " ").split():
        if re.fullmatch(r"[0-9a-f]+", tok):
            return int(tok, 16)
    raise AssertionError(
        f"{name}: {fname}+{addr:x}: cannot parse branch target '{ops.strip()}'"
    )


def _usb_audit_function(fname: str, insns: list[tuple[int, str, str, str]],
                        local_text: set[str], name: str,
                        func_entries: set[int],
                        trusted: frozenset[str] = frozenset(),
                        allow_adrp: bool = False) -> int:
    """Prove every store in one function stack-based, or raise.

    Runs a per-basic-block dataflow with INTERSECTION at joins: a
    register counts as sp-derived at a store only when EVERY path from
    the function entry proves it so. GCC interleaves epilogues with
    hot blocks (this object has 'ret'-terminated epilogue blocks
    sitting between the stores of the PHY label loop), so a linear
    "most recent definition" scan would reject good code here - and on
    other shapes it could wrongly accept bad code."""
    if not insns:
        return 0
    is_put = fname.startswith("put_") and fname in local_text
    index = {a: i for i, (a, _mn, _ops, _rest) in enumerate(insns)}

    def fail(addr: int, msg: str):
        raise AssertionError(f"{name}: {fname}+{addr:x}: {msg}")

    # ---- basic blocks: entry, branch targets, and any instruction
    # after a branch/ret/br are leaders. An unconditional 'b' to another
    # function's ENTRY is a tail call: a terminator like 'ret' (this
    # object tail-calls put_dec from put_bit); anything else that leaves
    # the function is an error - fail closed.
    leaders = {0}
    for i, (a, mn, ops, rest) in enumerate(insns):
        if mn == "b" or mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz"):
            tgt = _usb_branch_target(ops, name, fname, a)
            if tgt not in index:
                if mn == "b" and tgt in func_entries:
                    leaders.add(i + 1)
                    continue
                fail(a, f"branch '{rest}' leaves the function (fail closed)")
            leaders.add(index[tgt])
            leaders.add(i + 1)
        elif mn in ("ret", "br"):
            leaders.add(i + 1)
    blocks = []
    start = 0
    for i in range(1, len(insns)):
        if i in leaders:
            blocks.append((start, i))
            start = i
    blocks.append((start, len(insns)))
    block_of = {}
    for bi, (s, e) in enumerate(blocks):
        for i in range(s, e):
            block_of[i] = bi

    def block_succs(bi: int) -> list[int]:
        _s, e = blocks[bi]
        a, mn, ops, _rest = insns[e - 1]
        if mn in ("ret", "br"):
            return []
        if mn == "b" or mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz"):
            tgt = _usb_branch_target(ops, name, fname, a)
            if tgt not in index:
                if mn == "b" and tgt in func_entries:
                    return []  # tail call: control does not come back
                fail(a, f"branch leaves the function (fail closed)")
            out = [block_of[index[tgt]]]
            if mn != "b" and e < len(insns):
                out.append(block_of[e])
            return out
        return [block_of[e]] if e < len(insns) else []

    nblocks = len(blocks)
    succs = [block_succs(b) for b in range(nblocks)]
    preds: list[list[int]] = [[] for _ in range(nblocks)]
    for b, ss in enumerate(succs):
        for t in ss:
            preds[t].append(b)

    init = (frozenset(), frozenset({"x0"}) if is_put else frozenset(trusted),
            frozenset(), {})

    def meet(states):
        it = iter(states)
        first = next(it)
        sp, x0, img, slots = (set(first[0]), set(first[1]), set(first[2]),
                              dict(first[3]))
        for st in it:
            sp &= st[0]
            x0 &= st[1]
            img &= st[2]
            slots = {k: v for k, v in slots.items() if st[3].get(k) == v}
        return (frozenset(sp), frozenset(x0), frozenset(img), slots)

    def transfer(bi: int, state, check: bool):
        """Apply one block's instruction semantics; with check=True,
        every store in the block must be provably stack-based."""
        sp, x0, img, slots = (set(state[0]), set(state[1]), set(state[2]),
                              dict(state[3]))
        n_stores = 0

        def kill(r):
            sp.discard(r)
            x0.discard(r)
            img.discard(r)

        def cls(r):
            if r is None:
                return None
            if r == "sp" or r in sp:
                return "sp"
            if (allow_adrp and r in trusted) or r in x0:
                return "x0"
            if r in img:
                return "img"
            return None

        s, e = blocks[bi]
        for i in range(s, e):
            a, mn, ops, rest = insns[i]
            if _USB_BANNED_INSN.match(mn):
                fail(a, f"banned instruction '{rest}' "
                        "(system write / cache maintenance / atomic RMW)")
            if mn.startswith("st"):  # every store family (str/stp/stur/st1/...)
                n_stores += 1
                bm = _MEM_BASE.search(ops)
                base = _usb_reg(bm.group(1)) if bm else None
                if base is None:
                    fail(a, f"cannot parse store '{rest}'")
                if check and trusted and not allow_adrp:
                    m_off = re.search(r"\[\s*(x\d+)\s*,\s*(x\d+)", ops)
                    if m_off:
                        r1 = _usb_reg(m_off.group(1))
                        r2 = _usb_reg(m_off.group(2))
                        if not (r1 == "sp" or cls(r1) == "sp"):
                            if not ((r1 == "sp" or cls(r1)) and (r2 == "sp" or cls(r2))):
                                fail(a, f"store '{rest}' register-offset [{r1}, {r2}] not provably safe")
                if check and not (base == "sp" or cls(base)):
                    fail(a, f"store '{rest}' base {base} is not provably "
                            "stack-based")
                if base == "sp":  # plain spill stores feed the slot map
                    sm = _SP_SLOT_MEM.match(ops)
                    if sm:
                        srcs = [t.strip() for t in sm.group("rs").split(",")]
                        off = int(sm.group("off"), 0)
                        width = 4 if re.fullmatch(r"w(?:[12]?\d|30)|wzr",
                                                  srcs[0]) else 8
                        for j, srctok in enumerate(srcs):
                            slots[off + j * width] = cls(_usb_reg(srctok))
            elif (mn == "ldr" or mn.startswith(("ldr", "ldur", "ldtr"))
                    or mn in ("ldar", "ldaxr", "ldxr", "ldxp", "ldaxp", "ldp")):
                n_dest = 2 if mn in ("ldp", "ldxp", "ldaxp") else 1
                toks = [t.strip() for t in ops.split(",")[:n_dest]]
                dests = [_usb_reg(t) for t in toks]
                if any(d is None for d in dests):
                    fail(a, f"cannot parse load '{rest}'")
                sm = _SP_SLOT_LD.search(ops)
                for j, (tok, d) in enumerate(zip(toks, dests)):
                    c = None
                    if not tok.startswith("w") and sm:
                        c = slots.get(int(sm.group(1), 0) + j * 8)
                    if c == "sp":
                        sp.add(d)
                    elif c == "x0":
                        x0.add(d)
                    elif c == "img":
                        img.add(d)
                    else:
                        kill(d)
            elif mn in ("add", "sub", "adds", "subs"):
                toks = [t.strip() for t in ops.split(",")]
                dest = _usb_reg(toks[0])
                if dest is None:
                    fail(a, f"cannot parse '{rest}'")
                if toks[0].startswith("w"):
                    kill(dest)  # 32-bit result: zero-extended, not a pointer
                elif dest != "sp":  # sp stays sp (frame adjustment)
                    src1 = _usb_reg(toks[1]) if len(toks) > 1 else None
                    c = cls(src1)
                    if c == "sp":
                        sp.add(dest)
                    elif c == "x0":
                        is_imm = len(toks) > 2 and toks[2].startswith("#")
                        if is_imm or allow_adrp or is_put:
                            x0.add(dest)
                        else:
                            kill(dest)
                    elif c == "img":
                        img.add(dest)
                    else:
                        kill(dest)
            elif mn == "mov":
                toks = [t.strip() for t in ops.split(",")]
                dest = _usb_reg(toks[0])
                if dest is None:
                    fail(a, f"cannot parse '{rest}'")
                src = _usb_reg(toks[1]) if len(toks) > 1 else None
                if dest == "sp":
                    if cls(src) != "sp":
                        fail(a, f"stack pivot '{rest}'")
                else:
                    c = cls(src) if not toks[0].startswith("w") else None
                    if c == "sp":
                        sp.add(dest)
                    elif c == "x0":
                        x0.add(dest)
                    elif c == "img":
                        img.add(dest)
                    else:
                        kill(dest)
            elif mn in ("movz", "movn", "movk", "adr", "adrp"):
                d = _usb_reg(ops.split(",")[0])
                if d is None:
                    fail(a, f"cannot parse '{rest}'")
                if allow_adrp and mn in ("adrp", "adr"):
                    img.add(d)  # PC-relative, resolved inside the linked image
                else:
                    kill(d)
            elif mn in ("bl", "blr"):
                for r in _USB_CALLER_SAVED:
                    kill(r)
            elif (mn == "b" or mn.startswith("b.")
                    or mn in ("cbz", "cbnz", "tbz", "tbnz")
                    or mn in _USB_NO_WRITE or mn in _USB_FLAGS_ONLY):
                pass
            elif mn in _USB_WRITES_FIRST:
                d = _usb_reg(ops.split(",")[0])
                if d is None:
                    fail(a, f"cannot parse '{rest}'")
                kill(d)
            else:
                fail(a, f"cannot classify instruction '{rest}' (fail closed)")
        return (frozenset(sp), frozenset(x0), frozenset(img), slots), n_stores

    # ---- fixpoint: out-states only shrink, so this terminates ----
    outs: list[tuple[frozenset[str], frozenset[str], frozenset[str], dict[int, str | None]] | None] = [None] * nblocks
    changed = True
    while changed:
        changed = False
        for b in range(nblocks):
            states = ([init] if b == 0 else []) + \
                     [outs[p] for p in preds[b] if outs[p] is not None]
            if not states:
                continue  # not (yet) reachable
            new_out, _n = transfer(b, meet(states), check=False)
            if new_out != outs[b]:
                outs[b] = new_out
                changed = True

    # ---- verification pass over the final in-states ----
    total = 0
    for b in range(nblocks):
        if outs[b] is None:
            for i in range(*blocks[b]):
                if insns[i][1].startswith("st"):
                    fail(insns[i][0], f"store '{insns[i][3]}' in an "
                            "unreachable block (fail closed)")
            continue
        states = ([init] if b == 0 else []) + \
                 [outs[p] for p in preds[b] if outs[p] is not None]
        _out, n = transfer(b, meet(states), check=True)
        total += n
    return total

def _cross_tool(kind: str) -> str:
    """Resolve objdump/nm the way shim/build.sh does: CROSS_* env, PATH,
    then the arm-gnu toolchain in $HOME/.local/opt."""
    env = os.environ.get(f"CROSS_{kind.upper()}")
    if env:
        return env
    home = pathlib.Path.home() / ".local/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin"
    for prefix in ("aarch64-none-elf-", "aarch64-linux-gnu-"):
        name = prefix + kind
        if shutil.which(name):
            return name
        if (home / name).is_file():
            return str(home / name)
    raise AssertionError(
        f"cannot find {kind} for the usbprobe object audit (set CROSS_{kind.upper()})"
    )


def check_usb_object(obj_path: pathlib.Path) -> int:
    """Run the object audit on a built usbprobe.o; returns the number of
    stores audited."""
    assert obj_path.is_file(), f"{obj_path} not found (run shim/build.sh first)"
    objdump, nm = _cross_tool("objdump"), _cross_tool("nm")

    def run(*cmd):
        p = subprocess.run(cmd, capture_output=True, text=True)
        assert p.returncode == 0, f"{cmd[0]} failed: {p.stderr.strip()}"
        return p.stdout

    undefined = set()
    for line in run(nm, "-u", str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 2 and toks[0] == "U":
            undefined.add(toks[1])
    local_text = set()
    for line in run(nm, str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 3 and toks[1] == "t":
            local_text.add(toks[2])
    return check_usb_object_text(
        run(objdump, "-dr", str(obj_path)),
        local_text,
        undefined,
        run(objdump, "-h", str(obj_path)),
        str(obj_path),
    )


# Synthetic object fixtures for the self-test: encodings are ignored by
# the audit, only the mnemonic/operand text matters.
_SYN_H = """out.o:     file format elf64-littleaarch64

Sections:
Idx Name          Size      VMA               LMA               File off  Algn
  0 .text         000000f0  0000000000000000  0000000000000000  00000040  2**5
  1 .data         00000000  0000000000000000  0000000000000000  00000130  2**0
  2 .bss          00000000  0000000000000000  0000000000000000  00000130  2**0
"""
_SYN_NM = (
    "0000000000000000 t usbprobe_report\n"
    "0000000000000040 t put_hex\n"
)
_SYN_NM_GLOBAL_PUT = (
    "0000000000000000 t usbprobe_report\n"
    "0000000000000040 T put_hex\n"   # not static: x0 dst rule must NOT apply
)
_SYN_NM_U = (
    "                 U try_read32\n"
    "                 U con_str\n"
    "                 U fdt_collect_memmap\n"
    "                 U _head\n"
    "                 U _end\n"
)
_SYN_CONTROL = """0000000000000000 <usbprobe_report>:
   0:	d10043ff 	sub	sp, sp, #0x10
   4:	910003fd 	mov	x29, sp
   8:	910083f3 	add	x19, sp, #0x20
   c:	b9000660 	str	w0, [x19, 4]
  10:	f90003f3 	str	x19, [sp, #8]
  14:	f94003e2 	ldr	x2, [sp, #8]
  18:	b9000042 	str	w2, [x2]
  1c:	aa0603f3 	mov	x3, x19
  20:	b9000063 	str	w3, [x3, 8]
  24:	7100041f 	cmp	w0, #0x1
  28:	54000061 	b.ne	2c <usbprobe_report+0x2c>
  2c:	d503201f 	nop
0000000000000040 <put_hex>:
  40:	aa0003e5 	mov	x5, x0
  44:	39000185 	strb	w1, [x5]
  48:	39000001 	strb	w2, [x0]
  4c:	d65f03c0 	ret
"""
_SYN_TAIL = "  2c:\td503201f \tnop\n"  # splice point for injected bad code

# CFG fixtures: a join where x19 is sp-derived on one path and reloaded
# from a foreign pointer on the other, and a store in a dead block.
_SYN_JOIN_BAD = """0000000000000000 <usbprobe_report>:
   0:	d10043ff 	sub	sp, sp, #0x10
   4:	910083f3 	add	x19, sp, #0x20
   8:	34000010 	cbz	w0, 14 <usbprobe_report+0x14>
   c:	f9400027 	ldr	x19, [x1]
  10:	14000002 	b	18 <usbprobe_report+0x18>
  14:	d503201f 	nop
  18:	b9000660 	str	w0, [x19]
"""
_SYN_JOIN_OK = _SYN_JOIN_BAD.replace("ldr\tx19, [x1]", "add\tx19, sp, #0x8")
_SYN_UNREACHABLE = """0000000000000000 <usbprobe_report>:
   0:	d65f03c0 	ret
   4:	b9000020 	str	w0, [x1]
"""


def usb_object_selftest() -> int:
    """Feed the object audit synthetic bad objects and assert each is
    rejected; the good control must pass (and its stores must actually
    be counted, so a parser that sees nothing cannot pass silently)."""

    def parse_nm(nm_text):
        local = {t[2] for t in (l.split() for l in nm_text.splitlines())
                 if len(t) == 3 and t[1] == "t"}
        undef = {t[1] for t in (l.split() for l in _SYN_NM_U.splitlines())
                 if len(t) == 2 and t[0] == "U"}
        return local, undef

    def run_case(disasm, nm=_SYN_NM, nm_u=_SYN_NM_U, h=_SYN_H):
        local, _ = parse_nm(nm)
        undef = {t[1] for t in (l.split() for l in nm_u.splitlines())
                 if len(t) == 2 and t[0] == "U"}
        return check_usb_object_text(disasm, local, undef, h, "selftest object")

    n = run_case(_SYN_CONTROL)
    assert n == 6, f"selftest control: expected 6 audited stores, saw {n}"
    n = run_case(_SYN_JOIN_OK)
    assert n == 1, f"selftest join control: expected 1 audited store, saw {n}"

    def bad(tail):
        assert _SYN_TAIL in _SYN_CONTROL
        return _SYN_CONTROL.replace(_SYN_TAIL, tail)

    cases = [
        ("store to a non-stack base",
         bad("  2c:\tb9000020 \tstr\tw0, [x1]\n")),
        ("smc", bad("  2c:\td4000003 \tsmc\t#0\n")),
        ("hvc", bad("  2c:\td4000002 \thvc\t#0\n")),
        ("msr system register", bad("  2c:\td5184100 \tmsr\tsctlr_el1, x0\n")),
        ("dc cache maintenance", bad("  2c:\td50b7e20 \tdc\tzva, x19\n")),
        ("LSE atomic stadd", bad("  2c:\tb8200260 \tstadd\tw0, [x19]\n")),
        ("LSE atomic swp", bad("  2c:\tb8201260 \tswp\tw0, w1, [x19]\n")),
        ("LSE atomic cas", bad("  2c:\t88a00260 \tcas\tw0, w1, [x19]\n")),
        ("x0 store outside a put_ helper",
         bad("  2c:\tb9000000 \tstr\tw9, [x0]\n")),
        ("store through a rodata pointer",
         bad("  2c:\t90000009 \tadrp\tx9, 0 <usbprobe_report>\n"
             "  30:\t91002129 \tadd\tx9, x9, #0x8\n"
             "  34:\tb9000129 \tstr\tw0, [x9]\n")),
        ("store through a spilled unknown pointer",
         bad("  2c:\tf90007e1 \tstr\tx1, [sp, #16]\n"
             "  30:\tf94007e9 \tldr\tx9, [sp, #16]\n"
             "  34:\tb9000129 \tstr\tw0, [x9]\n")),
        ("movk breaks an sp-derived chain",
         bad("  2c:\t91002129 \tadd\tx9, sp, #0x8\n"
             "  30:\td2acce29 \tmovk\tx9, #0x667, lsl #16\n"
             "  34:\tb9000129 \tstr\tw0, [x9]\n")),
        ("stack pivot via mov sp",
         bad("  2c:\taa0103f0 \tmov\tsp, x1\n")),
        ("store through a register killed on one incoming path", _SYN_JOIN_BAD),
        ("store in an unreachable block", _SYN_UNREACHABLE),
        ("unclassifiable instruction (.inst)",
         bad("  2c:\te7ffdefe \t.inst\t0xe7ffdefe ; undefined\n")),
    ]
    caught = 0
    for label, disasm in cases:
        try:
            run_case(disasm)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: object {label} -> {exc}")
        else:
            raise AssertionError(f"usb object self-test FAILED to catch: {label}")
    for label, kwargs in (
        ("undefined symbol beyond the surface",
         {"nm_u": _SYN_NM_U + "                 U memcpy\n"}),
        ("non-empty .data", {"h": _SYN_H.replace("  1 .data         00000000",
                                                  "  1 .data         00000010")}),
        ("non-empty .bss", {"h": _SYN_H.replace("  2 .bss          00000000",
                                                 "  2 .bss          00000020")}),
        ("put_ helper that is not static", {"nm": _SYN_NM_GLOBAL_PUT}),
    ):
        try:
            run_case(_SYN_CONTROL, **kwargs)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: object {label} -> {exc}")
        else:
            raise AssertionError(f"usb object self-test FAILED to catch: {label}")
    print(f"PASS usb object self-test: {caught}/{len(cases) + 4} bad objects rejected")
    return caught


# ---- usbproto.o: pure-protocol object audit (stage-2 gate honesty) ----
#
# usbproto.c must stay 100% free of MMIO: it has NO undefined symbols at
# all (it cannot call try_read32/try_write32 or anything else), and every
# store must be provably to the stack, to a caller-provided pointer, or
# through a PC-relative adrp/adr-derived pointer into the object's own
# image (in_byte, the one-byte static backing one-byte IN stages; adrp
# resolves inside the linked image and can never reach a device window).
# "Caller-provided pointer" means the AAPCS argument registers x0-x7,
# seeded at every function entry and invalidated by calls - the task's
# 'x0-x3' family widened because the 5-parameter get_descriptor helper
# receives its out-length pointer in x4.

_USBPROTO_TRUSTED_ARGS = frozenset(f"x{i}" for i in range(8))


def check_usbproto_object_text(disasm: str, local_text: set[str],
                               undefined: set[str], sections: str,
                               name: str = "usbproto.o") -> int:
    return check_usb_object_text(
        disasm, local_text, undefined, sections, name,
        allowed_undef=set(), trusted_args=_USBPROTO_TRUSTED_ARGS,
        allow_adrp=True, require_clean_data=False,
    )


def check_usbproto_object(obj_path: pathlib.Path) -> int:
    """Run the no-MMIO audit on a built usbproto.o."""
    assert obj_path.is_file(), f"{obj_path} not found (run shim/build.sh first)"
    objdump, nm = _cross_tool("objdump"), _cross_tool("nm")

    def run(*cmd):
        p = subprocess.run(cmd, capture_output=True, text=True)
        assert p.returncode == 0, f"{cmd[0]} failed: {p.stderr.strip()}"
        return p.stdout

    undefined = set()
    for line in run(nm, "-u", str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 2 and toks[0] == "U":
            undefined.add(toks[1])
    local_text = set()
    for line in run(nm, str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 3 and toks[1] == "t":
            local_text.add(toks[2])
    return check_usbproto_object_text(
        run(objdump, "-dr", str(obj_path)),
        local_text,
        undefined,
        run(objdump, "-h", str(obj_path)),
        str(obj_path),
    )


def usbproto_object_selftest() -> int:
    """Synthetic objects for the usbproto no-MMIO audit; the control
    carries one store of every sanctioned class (stack, arg register,
    adrp-derived image pointer)."""

    def run_case(disasm, nm_u=""):
        local = {t[2] for t in (l.split() for l in _UP_NM.splitlines())
                 if len(t) == 3 and t[1] == "t"}
        undef = {t[1] for t in (l.split() for l in nm_u.splitlines())
                 if len(t) == 2 and t[0] == "U"}
        return check_usbproto_object_text(disasm, local, undef, _SYN_H,
                                          "usbproto selftest")

    n = run_case(_UP_CONTROL)
    assert n == 4, f"usbproto selftest control: expected 4 audited stores, saw {n}"

    cases = [
        ("undefined symbol (any)", run_case,
         _UP_CONTROL, "                 U memcpy\n"),
        ("store through a movz/movk absolute pointer", run_case,
         _UP_CONTROL.replace(
             "   c:\t90000009 \tadrp\tx9, 0 <in_byte>\n"
             "  14:\t91000d29 \tadd\tx9, x9, #0x3\n",
             "   c:\td2a22229 \tmovz\tx9, #0x1111, lsl #16\n"
             "  14:\t72c20009 \tmovk\tx9, #0x0\n"), ""),
        ("smc", run_case,
         _UP_CONTROL.replace("  24:\td65f03c0 \tret\n",
                             "  24:\td4000003 \tsmc\t#0\n"), ""),
        ("store through an unclassified register", run_case,
         _UP_CONTROL.replace("strb\tw9, [x9]", "strb\tw9, [x20]"), ""),
    ]
    caught = 0
    for label, fn, *args in cases:
        try:
            fn(*args)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: usbproto {label} -> {exc}")
        else:
            raise AssertionError(f"usbproto object self-test FAILED to catch: {label}")
    print(f"PASS usbproto object self-test: {caught}/{len(cases)} bad objects rejected")
    return caught


_UP_NM = (
    "0000000000000000 t usbproto_setup\n"
    "0000000000000040 t usbproto_init\n"
)
_UP_CONTROL = """0000000000000000 <usbproto_setup>:
   0:	d10043ff 	sub	sp, sp, #0x10
   4:	39000001 	strb	w1, [x0]
   8:	b9000062 	str	w2, [x3]
   c:	90000009 	adrp	x9, 0 <in_byte>
  14:	91000d29 	add	x9, x9, #0x3
  18:	39000129 	strb	w9, [x9]
  1c:	52800029 	mov	w9, #0x1
  20:	b9000be9 	str	w9, [sp, #8]
  24:	d65f03c0 	ret
"""


# ---- usbgadget.o: WRITER audit (stage-2 gate honesty) ----
#
# usbgadget.c writes registers by design, so the read-only proof does
# not apply. Instead: (a) undefined symbols stay inside the sanctioned
# set; (b) EVERY reference to try_write32 (any relocation kind) must be
# a R_AARCH64_CALL26 bl from inside the single symbol w32 - the source
# keeps one noinline write helper, so the address of try_write32 never
# escapes it; (c) no smc/hvc/msr/dc/ic/at/tlbi and no LSE atomics (a
# writer must not sneak a system-register write or an RMW past the
# w32 whitelist).

_USBG_ALLOWED_UNDEF = {
    "try_read32",       # fault-guarded MMIO read (shim.c)
    "try_write32",      # ONLY from w32, enforced above
    "con_str",          # console/ramlog line out (console.c)
    "usbproto_init",    # pure protocol state reset (usbproto.c)
    "usbproto_setup",   # pure ep0 decision (usbproto.c)
}


_TOUCH_ALLOWED_UNDEF = {
    "try_read32",       # fault-guarded MMIO read (shim.c)
    "try_write32",      # ONLY from w32, enforced by the same writer audit
    "con_str",          # console/ramlog line out (console.c)
    "counter_freq",     # timer frequency (console.c)
    "wait_ticks",       # bounded delay (console.c)
    "touch_ticks",      # CNTPCT (shim.c)
    # --touch-probe=railsr|rails prints the ACPM client's results (acpm.o has its own writer audit below; these never write here)
    "acpm_cfg_init", "acpm_describe", "acpm_chan_id", "acpm_qword", "acpm_pmic_read", "acpm_pmic_update", "acpm_unsure", "acpm_why",
    # --touch-probe=dvfsr|dvfs: the ACPM DVFS client (CPU cluster rates, docs/research/25); same rule, acpm.o's writes are audited there
    "acpm_dvfs_ndom", "acpm_dvfs_dom", "acpm_dvfs_get", "acpm_dvfs_set",
}


_ACPM_ALLOWED_UNDEF = {
    "try_read32",       # fault-guarded MMIO / SRAM read (shim.c)
    "try_write32",      # ONLY from w32, enforced by the same writer audit
    "touch_ticks",      # CNTPCT (shim.c)
    "counter_freq",     # timer frequency (console.c)
}


def check_usbgadget_object_text(disasm: str, undefined: set[str],
                                name: str = "usbgadget.o",
                                allowed_undef: set[str] = _USBG_ALLOWED_UNDEF) -> None:
    extra = undefined - allowed_undef
    if extra:
        raise AssertionError(
            f"{name}: undefined symbols beyond the sanctioned surface "
            f"({', '.join(sorted(allowed_undef))}): "
            f"{', '.join(sorted(extra))}"
        )
    cur = None
    for raw in disasm.splitlines():
        line = raw.strip()
        m = _FUNC_LABEL.match(line)
        if m:
            cur = m.group(2)
            continue
        if ": R_" in line:
            m = re.match(r"^([0-9a-f]+):\s+(R_\w+)\s+(\S+)", line)
            if m and m.group(3) == "try_write32":
                if m.group(2) != "R_AARCH64_CALL26" or cur != "w32":
                    raise AssertionError(
                        f"{name}: reference to try_write32 at {m.group(1)} "
                        f"({m.group(2)}) inside '{cur}'; the only sanctioned "
                        "use is a bl from the w32 helper"
                    )
            continue
        m = _INSN_LINE.match(raw)
        if not m:
            continue
        rest = m.group(3).split("//")[0].strip()
        mn = rest.split(None, 1)[0]
        if _USB_BANNED_INSN.match(mn):
            raise AssertionError(
                f"{name}: banned instruction '{rest}' inside '{cur}' "
                "(system write / cache maintenance / atomic RMW)"
            )


def check_usbgadget_object(obj_path: pathlib.Path,
                           allowed_undef: set[str] = _USBG_ALLOWED_UNDEF) -> None:
    """Run the WRITER audit on a built usbgadget.o (or touchprobe.o with its own undef set)."""
    assert obj_path.is_file(), f"{obj_path} not found (run shim/build.sh first)"
    objdump, nm = _cross_tool("objdump"), _cross_tool("nm")

    def run(*cmd):
        p = subprocess.run(cmd, capture_output=True, text=True)
        assert p.returncode == 0, f"{cmd[0]} failed: {p.stderr.strip()}"
        return p.stdout

    undefined = set()
    for line in run(nm, "-u", str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 2 and toks[0] == "U":
            undefined.add(toks[1])
    check_usbgadget_object_text(run(objdump, "-dr", str(obj_path)),
                                undefined, str(obj_path), allowed_undef)


def usbgadget_object_selftest() -> int:
    """Synthetic objects for the usbgadget WRITER audit; the control is
    a w32 with one bl try_write32 plus another function that never
    references it."""

    def run_case(disasm, nm_u=_UG_NM_U):
        undef = {t[1] for t in (l.split() for l in nm_u.splitlines())
                 if len(t) == 2 and t[0] == "U"}
        check_usbgadget_object_text(disasm, undef, "usbgadget selftest")

    run_case(_UG_CONTROL)  # must pass

    cases = [
        ("bl try_write32 outside w32",
         _UG_CONTROL.replace(
             "  14:\t94000000 \tbl\t0 <con_str>\n"
             "\t\t\t14: R_AARCH64_CALL26\tcon_str\n",
             "  14:\t94000000 \tbl\t0 <try_write32>\n"
             "\t\t\t14: R_AARCH64_CALL26\ttry_write32\n")),
        ("address of try_write32 taken (adrp)",
         _UG_CONTROL.replace(
             "  14:\t94000000 \tbl\t0 <con_str>\n"
             "\t\t\t14: R_AARCH64_CALL26\tcon_str\n",
             "  14:\t90000009 \tadrp\tx9, 0 <try_write32>\n"
             "\t\t\t14: R_AARCH64_ADR_PREL_PG_HI21\ttry_write32\n")),
        ("msr system register",
         _UG_CONTROL + "  18:\td5184100 \tmsr\tsctlr_el1, x0\n"),
        ("LSE atomic stadd",
         _UG_CONTROL + "  18:\tb8200260 \tstadd\tw0, [x19]\n"),
    ]
    caught = 0
    for label, disasm in cases:
        try:
            run_case(disasm)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: usbgadget writer {label} -> {exc}")
        else:
            raise AssertionError(f"usbgadget writer self-test FAILED to catch: {label}")
    for label, nm_u in (
        ("undefined symbol wait_ticks (time comes via the hooks struct)",
         _UG_NM_U + "                 U wait_ticks\n"),
        ("undefined symbol memcpy",
         _UG_NM_U + "                 U memcpy\n"),
    ):
        try:
            run_case(_UG_CONTROL, nm_u)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: usbgadget writer {label} -> {exc}")
        else:
            raise AssertionError(f"usbgadget writer self-test FAILED to catch: {label}")
    print(f"PASS usbgadget writer self-test: {caught}/{len(cases) + 2} bad objects rejected")
    return caught


_UG_NM_U = "                 U try_read32\n                 U try_write32\n                 U con_str\n"
_UG_CONTROL = """0000000000000000 <w32>:
   0:	d10043ff 	sub	sp, sp, #0x10
   4:	94000000 	bl	0 <try_write32>
\t\t\t4: R_AARCH64_CALL26\ttry_write32
   8:	d65f03c0 	ret
0000000000000010 <usbgadget_poll>:
  14:	94000000 	bl	0 <con_str>
\t\t\t14: R_AARCH64_CALL26\tcon_str
  18:	d65f03c0 	ret
"""



# ---- usbgadget.c source audit (review H3): cannot be bypassed by asm / raw pointers ----
# The object audit proves where try_write32 is *named*; it cannot see stores
# done by inline asm, a hard-coded register pointer, or a second write path.
# These source rules close those holes: no asm; try_write32 / the DWC3 base
# only inside the two single-purpose helpers w32 / r32; no integer-literal
# pointer casts anywhere (raw MMIO); w32 must call its allowlist function.

def _strip_c_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def _c_function_span(code: str, name: str) -> tuple[int, int]:
    m = re.search(r"\b(?:static\s+)?(?:int|void|uint32_t)\s+" + name + r"\s*\([^)]*\)\s*\{", code)
    assert m, f"usbgadget source audit: function {name} not found"
    depth, i = 1, m.end()
    while depth and i < len(code):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    assert depth == 0, f"usbgadget source audit: unbalanced braces in {name}"
    return m.start(), i


def check_usbgadget_source_text(src: str, name: str = "usbgadget.c",
                                base_macro: str | None = "USBG_DWC3_BASE",
                                strict: bool = False,
                                guard_funcs: tuple[str, ...] = ("w32_allowed",)) -> None:
    """strict (touchprobe.c): no volatile outside r32 and no explicit pointer casts at all, so no second writer can
    be built from a pointer; guard_funcs must really depend on the address and not be a constant."""
    code = _strip_c_comments(src)
    if re.search(r"\b(__asm__|__asm|asm)\b", code):
        raise AssertionError(f"{name}: inline asm is banned in the hardware writer")
    spans = [_c_function_span(code, f) for f in ("w32", "r32")]

    def inside(pos: int) -> bool:
        return any(a <= pos < b for a, b in spans)

    w32_a, w32_b = spans[0]
    for m in re.finditer(r"\btry_write32\b", code):
        if not (w32_a <= m.start() < w32_b):
            raise AssertionError(f"{name}: try_write32 used outside w32")
    for m in (re.finditer(r"\b" + base_macro + r"\b", code) if base_macro else ()):
        line_start = code.rfind("\n", 0, m.start()) + 1
        if code[line_start:m.start()].lstrip().startswith("#define"):
            continue
        if not inside(m.start()):
            raise AssertionError(f"{name}: DWC3 base used outside w32/r32")
    if re.search(r"\(\s*(?:volatile\s+)?[A-Za-z_][\w\s]*\*\s*\)\s*(?:0x[0-9a-fA-F]+|\d+)\b", code):
        raise AssertionError(f"{name}: integer-literal pointer cast (raw MMIO) is banned")
    body = code[w32_a:w32_b]
    guard = re.search(r"if\s*\(\s*!\s*w32_allowed\s*\([^;{}]*\)\s*\)\s*return\b", body)
    if not guard:
        raise AssertionError(f"{name}: w32 lacks the 'if (!w32_allowed(...)) return' guard")
    if body.find("try_write32") < guard.start():
        raise AssertionError(f"{name}: w32 writes before its allowlist guard")
    for g in guard_funcs:
        ga, gb = _c_function_span(code, g)
        gbody = code[ga:gb]
        params = re.search(r"\(([^)]*)\)", gbody).group(1)
        first = re.findall(r"[A-Za-z_]\w*", params.split(",")[0])[-1]
        inner = gbody[gbody.index("{") + 1:-1]
        if not re.search(r"\b" + first + r"\b", inner) or re.fullmatch(r"\s*return\s+[01]\s*;\s*", inner):
            raise AssertionError(f"{name}: {g} does not depend on its address parameter '{first}' (constant allowlist)")
    if strict:
        r32_a, r32_b = spans[1]
        for m in re.finditer(r"\bvolatile\b", code):
            if not (r32_a <= m.start() < r32_b):
                raise AssertionError(f"{name}: 'volatile' outside r32 (a second MMIO writer?)")
        if re.search(r"\(\s*(?:const\s+)?(?:volatile\s+)?[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*\s*\*+\s*\)", code):
            raise AssertionError(f"{name}: explicit pointer cast is banned in the hardware writer")


def usbgadget_source_selftest() -> int:
    good = open(pathlib.Path(__file__).resolve().parents[1] / "shim" / "usbgadget.c").read()
    check_usbgadget_source_text(good)  # control must pass
    cases = [
        ("inline asm store", good.replace("static int r32", 'static void evil(void) { __asm__ volatile("str wzr, [x0]"); }\nstatic int r32', 1)),
        ("second write path", good.replace("static int r32", "static void evil(void) { try_write32(0x11110000UL + 0xC110u, 0); }\nstatic int r32", 1)),
        ("base used in a helper", good.replace("static int r32", "static uint32_t evil(void) { return USBG_DWC3_BASE + 8; }\nstatic int r32", 1)),
        ("literal pointer cast", good.replace("static int r32", "static void evil(void) { *(volatile uint32_t *)0x11111234 = 0; }\nstatic int r32", 1)),
        ("w32 skips allowlist", good.replace("if (!w32_allowed(off))", "if (0)", 1)),
    ]
    caught = 0
    for label, bad in cases:
        assert bad != good, f"usbgadget source selftest case did not mutate: {label}"
        try:
            check_usbgadget_source_text(bad, "selftest")
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: usbgadget source {label} -> {exc}")
        else:
            raise AssertionError(f"usbgadget source self-test FAILED to catch: {label}")
    print(f"PASS usbgadget source self-test: {caught}/{len(cases)} bad sources rejected")
    return caught


_TOUCH_KW = dict(base_macro=None, strict=True, guard_funcs=("w32_allowed", "wmask"))


def touchprobe_source_selftest() -> int:
    """Strict source audit of shim/touchprobe.c: no asm, no volatile / pointer casts, one write path (w32) that checks
    w32_allowed before writing, and an allowlist that really depends on the address."""
    good = (pathlib.Path(__file__).resolve().parents[1] / "shim" / "touchprobe.c").read_text()
    check_usbgadget_source_text(good, "touchprobe.c", **_TOUCH_KW)  # control must pass
    ins = "static int r32"
    cases = [
        ("inline asm store", good.replace(ins, 'static void evil(void) { __asm__ volatile("str wzr, [x0]"); }\n' + ins, 1)),
        ("second write path", good.replace(ins, "static void evil(void) { try_write32(0x10d40000UL, 0); }\n" + ins, 1)),
        ("second writer via a non-literal pointer",
         good.replace(ins, "static void evil(volatile uint32_t *p) { *p = 0; }\n" + ins, 1)),
        ("second writer via a pointer cast of a variable",
         good.replace(ins, "static void evil(uintptr_t pa) { *(uint32_t *)pa = 0; }\n" + ins, 1)),
        ("literal pointer cast", good.replace(ins, "static void evil(void) { *(volatile uint32_t *)0x10d40000 = 0; }\n" + ins, 1)),
        ("w32 skips allowlist", good.replace("if (!w32_allowed(pa, old, v, opts))", "if (0)", 1)),
        ("w32 calls the allowlist and ignores it",
         good.replace("if (!w32_allowed(pa, old, v, opts))\n\t\treturn -2;", "(void)w32_allowed(pa, old, v, opts);", 1)),
        ("w32 writes before the guard",
         good.replace("\tif (!w32_allowed(pa, old, v, opts))\n\t\treturn -2;\n\treturn try_write32(pa, v) ? 0 : -1;",
                      "\t(void)try_write32(pa, v);\n\tif (!w32_allowed(pa, old, v, opts))\n\t\treturn -2;\n\treturn 0;", 1)),
        ("w32_allowed always returns 1",
         re.sub(r"(static int w32_allowed\(uintptr_t pa, uint32_t old, uint32_t v, unsigned opts\)\n\{).*?\n\}\n",
                r"\1\n\treturn 1;\n}\n", good, count=1, flags=re.S)),
        ("wmask ignores the address",
         re.sub(r"(static uint32_t wmask\(uintptr_t pa, unsigned opts\)\n\{).*?\n\}\n",
                r"\1\n\treturn 0xffffffffu;\n}\n", good, count=1, flags=re.S)),
    ]
    caught = 0
    for label, bad in cases:
        assert bad != good, f"touchprobe source selftest case did not mutate: {label}"
        try:
            check_usbgadget_source_text(bad, "selftest", **_TOUCH_KW)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: touchprobe source {label} -> {exc}")
        else:
            raise AssertionError(f"touchprobe source self-test FAILED to catch: {label}")
    print(f"PASS touchprobe source self-test: {caught}/{len(cases)} bad sources rejected")
    return caught


_ACPM_KW = dict(base_macro=None, strict=True, guard_funcs=("w32_allowed",))


def acpm_source_selftest() -> int:
    """Strict source audit of shim/acpm.c (same rules as touchprobe.c): one write path (w32) that checks w32_allowed first,
    an allowlist that depends on the address, no asm / volatile / pointer casts."""
    good = (pathlib.Path(__file__).resolve().parents[1] / "shim" / "acpm.c").read_text()
    check_usbgadget_source_text(good, "acpm.c", **_ACPM_KW)  # control must pass
    ins = "static int r32"
    cases = [
        ("inline asm store", good.replace(ins, 'static void evil(void) { __asm__ volatile("str wzr, [x0]"); }\n' + ins, 1)),
        ("second write path", good.replace(ins, "static void evil(void) { try_write32(0x17610040UL, 4); }\n" + ins, 1)),
        ("second writer via a pointer cast of a variable",
         good.replace(ins, "static void evil(uintptr_t pa) { *(uint32_t *)pa = 0; }\n" + ins, 1)),
        ("volatile outside r32", good.replace(ins, "static void evil(volatile uint32_t *p) { *p = 0; }\n" + ins, 1)),
        ("w32 skips allowlist", good.replace("if (!w32_allowed(pa, v, g))", "if (0)", 1)),
        ("w32 writes before the guard",
         good.replace("\tif (!w32_allowed(pa, v, g))\n\t\treturn -2;\n\treturn try_write32(pa, v) ? 0 : -1;",
                      "\t(void)try_write32(pa, v);\n\tif (!w32_allowed(pa, v, g))\n\t\treturn -2;\n\treturn 0;", 1)),
        ("w32_allowed always returns 1",
         re.sub(r"(static int w32_allowed\(uintptr_t pa, uint32_t v, const struct wguard \*g\)\n\{).*?\n\}\n",
                r"\1\n\treturn 1;\n}\n", good, count=1, flags=re.S)),
    ]
    caught = 0
    for label, bad in cases:
        assert bad != good, f"acpm source selftest case did not mutate: {label}"
        try:
            check_usbgadget_source_text(bad, "selftest", **_ACPM_KW)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: acpm source {label} -> {exc}")
        else:
            raise AssertionError(f"acpm source self-test FAILED to catch: {label}")
    print(f"PASS acpm source self-test: {caught}/{len(cases)} bad sources rejected")
    return caught


def check_touchprobe_object_text(disasm: str, undefined: set[str], local_text: set[str], sections: str,
                                 name: str = "touchprobe.o", allowed_undef: set[str] = _TOUCH_ALLOWED_UNDEF) -> int:
    """Writer audit (try_write32 only by a bl inside w32, undef subset, no banned insns) plus the store audit on
    everything except w32: every store is stack based or goes through an argument buffer (x0-x7 chain)."""
    check_usbgadget_object_text(disasm, undefined, name, allowed_undef)
    blocks = re.split(r"(?m)^(?=[0-9a-f]+ <[^>]+>:$)", disasm)
    rest = "".join(b for b in blocks if not re.match(r"[0-9a-f]+ <w32>:", b))
    return check_usb_object_text(rest, local_text | {"w32"}, undefined, sections, name,
                                 allowed_undef=allowed_undef,
                                 trusted_args=frozenset(f"x{i}" for i in range(8)),
                                 allow_adrp=False, require_clean_data=True)


def _touch_obj_info(obj_path: pathlib.Path):
    objdump, nm = _cross_tool("objdump"), _cross_tool("nm")

    def run(*cmd):
        p = subprocess.run(cmd, capture_output=True, text=True)
        assert p.returncode == 0, f"{cmd[0]} failed: {p.stderr.strip()}"
        return p.stdout

    undefined, local_text = set(), set()
    for line in run(nm, str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 2 and toks[0] == "U":
            undefined.add(toks[1])
        elif len(toks) == 3 and toks[1] in ("t", "T"):
            local_text.add(toks[2])
    return run(objdump, "-dr", str(obj_path)), undefined, local_text, run(objdump, "-h", str(obj_path))


def check_touchprobe_object(obj_path: pathlib.Path, allowed_undef: set[str] = _TOUCH_ALLOWED_UNDEF) -> int:
    assert obj_path.is_file(), f"{obj_path} not found (run shim/build.sh first)"
    disasm, undefined, local_text, sections = _touch_obj_info(obj_path)
    return check_touchprobe_object_text(disasm, undefined, local_text, sections, str(obj_path), allowed_undef)


def touchprobe_object_selftest(obj_path: pathlib.Path, allowed_undef: set[str] = _TOUCH_ALLOWED_UNDEF,
                               tag: str = "touchprobe") -> int:
    """Mutations of the real object's disassembly: each must be rejected."""
    disasm, undefined, local_text, sections = _touch_obj_info(obj_path)
    check_touchprobe_object_text(disasm, undefined, local_text, sections, f"{tag} selftest", allowed_undef)  # control
    evil = ("0000000000ffff00 <evil>:\n"
            "  ffff00:\td2a22229 \tmovz\tx9, #0x1111, lsl #16\n"
            "  ffff04:\tb9000129 \tstr\tw0, [x9]\n"
            "  ffff08:\td65f03c0 \tret\n")
    evil_sp = ("0000000000ffff00 <evil>:\n"
               "  ffff00:\tb90013e0 \tstr\tw0, [sp, #16]\n"
               "  ffff04:\td65f03c0 \tret\n")
    evil_w = ("0000000000ffff00 <evil>:\n"
              "  ffff00:\t94000000 \tbl\t0 <try_write32>\n"
              "\t\t\tffff00: R_AARCH64_CALL26\ttry_write32\n"
              "  ffff04:\td65f03c0 \tret\n")
    check_touchprobe_object_text(disasm + evil_sp, undefined, local_text | {"evil"}, sections, f"{tag} selftest", allowed_undef)
    caught = 0
    for label, extra, und in (
        ("store through a movz/movk absolute pointer outside w32", evil, undefined),
        ("second bl try_write32 outside w32", evil_w, undefined),
        ("undefined symbol memcpy", evil_sp, undefined | {"memcpy"}),
    ):
        try:
            check_touchprobe_object_text(disasm + extra, und, local_text | {"evil"}, sections, f"{tag} selftest", allowed_undef)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: {tag} object {label} -> {exc}")
        else:
            raise AssertionError(f"{tag} object self-test FAILED to catch: {label}")
    print(f"PASS {tag} object self-test: {caught}/3 bad objects rejected")
    return caught


# ---- udump.o: read-only dump framer object audit (Spec 01 gate honesty) ----
#
# shim/udump.c frames the userdata dump and must stay free of MMIO: it
# reaches hardware only through the audited surfaces - usbgadget_write/
# usbgadget_ready for the wire, con_str/con_hex2 for text - plus the
# caller's hooks (a bounded UFS reader, the log pump, fakeable time).
# It carries NO static state, so .data/.bss must be empty, and every
# store must be provably to the stack or an argument buffer (x0-x7
# chain); adrp and movz/movk pointers can never be a store base, so no
# register window can be written through. Fail closed, like the audits
# above. "No WRITE opcodes" holds because the object's whole undefined
# surface is this set: it cannot call or build any SCSI/UFS command.

_UDUMP_ALLOWED_UNDEF = {
    "usbgadget_write",   # bounded bulk IN pieces (usbgadget.c)
    "usbgadget_ready",   # configured+DTR+no-fault query (usbgadget.c)
    "con_str",           # console/ramlog line out (console.c)
    "con_hex2",          # console/ramlog line out (console.c)
}


def check_udump_object(obj_path: pathlib.Path) -> int:
    """Run the no-MMIO store audit on a built udump.o."""
    assert obj_path.is_file(), f"{obj_path} not found (run shim/build.sh first)"
    objdump, nm = _cross_tool("objdump"), _cross_tool("nm")

    def run(*cmd):
        p = subprocess.run(cmd, capture_output=True, text=True)
        assert p.returncode == 0, f"{cmd[0]} failed: {p.stderr.strip()}"
        return p.stdout

    undefined = set()
    for line in run(nm, "-u", str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 2 and toks[0] == "U":
            undefined.add(toks[1])
    local_text = set()
    for line in run(nm, str(obj_path)).splitlines():
        toks = line.split()
        if len(toks) == 3 and toks[1] == "t":
            local_text.add(toks[2])
    return check_usb_object_text(
        run(objdump, "-dr", str(obj_path)),
        local_text,
        undefined,
        run(objdump, "-h", str(obj_path)),
        str(obj_path),
        allowed_undef=_UDUMP_ALLOWED_UNDEF,
        trusted_args=frozenset(f"x{i}" for i in range(8)),
        allow_adrp=False,
        require_clean_data=True,
    )


_UD_NM_U = (
    "                 U usbgadget_write\n"
    "                 U usbgadget_ready\n"
    "                 U con_str\n"
    "                 U con_hex2\n"
)
_UD_CONTROL = """0000000000000000 <udump_run>:
   0:	d10043ff 	sub	sp, sp, #0x10
   4:	39000001 	strb	w1, [x0]
   8:	39000642 	strb	w2, [x2, 8]
   c:	b90017e3 	str	w3, [sp, 12]
  10:	d65f03c0 	ret
"""


def udump_object_selftest() -> int:
    """Synthetic udump-shaped bad objects; the control carries a store
    of every sanctioned class (stack, argument register) under the
    exact udump allowed-undef set."""

    def run_case(disasm, nm_u=_UD_NM_U):
        undef = {t[1] for t in (l.split() for l in nm_u.splitlines())
                 if len(t) == 2 and t[0] == "U"}
        return check_usb_object_text(
            disasm, {"udump_run"}, undef, _SYN_H, "udump selftest",
            allowed_undef=_UDUMP_ALLOWED_UNDEF,
            trusted_args=frozenset(f"x{i}" for i in range(8)),
            allow_adrp=False, require_clean_data=True)

    n = run_case(_UD_CONTROL)
    assert n == 3, f"udump selftest control: expected 3 audited stores, saw {n}"

    cases = [
        ("try_write32 in the undef set (a write path sneaking in)",
         run_case, _UD_CONTROL, _UD_NM_U + "                 U try_write32\n"),
        ("store through a movz/movk absolute pointer (an MMIO window)",
         run_case, _UD_CONTROL.replace(
             "  10:\td65f03c0 \tret\n",
             "  10:\td2a22229 \tmovz\tx9, #0x1111, lsl #16\n"
             "  14:\tb9000129 \tstr\tw0, [x9]\n"), ""),
        ("store through an adrp-derived pointer",
         run_case, _UD_CONTROL.replace(
             "   8:\t39000642 \tstrb\tw2, [x2, 8]\n",
             "   8:\t90000009 \tadrp\tx9, 0 <udump_run>\n"
             "   c:\t39000129 \tstrb\tw2, [x9]\n"
             "  10:\tb90017e3 \tstr\tw3, [sp, 12]\n"
             "  14:\td65f03c0 \tret\n"), ""),
        ("store through an unclassified callee-saved register",
         run_case, _UD_CONTROL.replace("strb\tw2, [x2, 8]", "strb\tw2, [x20]"), ""),
        ("store through a redefined argument register in register-offset form (finding H2 bypass)",
         run_case, _UD_CONTROL.replace(
             "  10:\td65f03c0 \tret\n",
             "  10:\td2a22220 \tmov\tx0, #0x11110000\n"
             "  14:\tb8206823 \tstr\tw3, [x1, x0]\n"
             "  18:\td65f03c0 \tret\n"), ""),
    ]
    caught = 0
    for label, fn, *args in cases:
        try:
            fn(*args)
        except AssertionError as exc:
            caught += 1
            print(f"self-test caught: udump {label} -> {exc}")
        else:
            raise AssertionError(f"udump object self-test FAILED to catch: {label}")
    print(f"PASS udump object self-test: {caught}/{len(cases)} bad objects rejected")
    return caught


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("boot_image", type=pathlib.Path)
    args = parser.parse_args()
    image = args.boot_image.read_bytes()
    assert image[:8] == b"ANDROID!", "missing Android boot image magic"
    kernel_size, ramdisk_size, _, header_size = struct.unpack_from("<4I", image, 8)
    header_version, = struct.unpack_from("<I", image, 40)
    assert header_version == 4, f"expected header v4, got {header_version}"
    assert header_size == 1584, f"unexpected v4 header size: {header_size}"
    assert kernel_size > 0 and ramdisk_size > 0, "kernel and ramdisk must both be present"
    page_size = 4096
    kernel_data = image[page_size:page_size + kernel_size]
    is_arm64 = len(kernel_data) > 0x3c and kernel_data[0x38:0x3c] == b"ARMd"
    is_lz4 = kernel_data[:4] == bytes([0x04, 0x22, 0x4d, 0x18])
    assert is_arm64 or is_lz4, "kernel is neither an ARM64 Image nor an LZ4-compressed Image"

    repo_root = pathlib.Path(__file__).resolve().parent.parent
    check_ufs_safety(repo_root / "shim" / "ufs.c")
    ufs_safety_selftest()
    check_usb_safety(repo_root / "shim" / "usbprobe.c")
    check_usb_safety(repo_root / "shim" / "usbprobe.h")
    check_usb_safety(repo_root / "shim" / "wlanprobe.c")
    check_usb_safety(repo_root / "shim" / "wlanprobe.h")
    check_usb_safety(repo_root / "shim" / "gpuprobe.c")
    check_usb_safety(repo_root / "shim" / "gpuprobe.h")
    usb_safety_selftest()
    usb_object_selftest()
    n_stores = check_usb_object(repo_root / "shim" / "out" / "usbprobe.o")
    print(f"PASS usb object audit: {n_stores} stores, all stack-based; "
          "undef/.data/.bss/instruction bans clean")
    n_stores = check_usb_object(repo_root / "shim" / "out" / "wlanprobe.o")
    print(f"PASS wlanprobe object audit: {n_stores} stores, all stack-based "
          "(same policy as usbprobe.o)")
    n_stores = check_usb_object(repo_root / "shim" / "out" / "gpuprobe.o")
    print(f"PASS gpuprobe object audit: {n_stores} stores, all stack-based "
          "(same policy as usbprobe.o)")
    usbproto_object_selftest()
    n_stores = check_usbproto_object(repo_root / "shim" / "out" / "usbproto.o")
    print(f"PASS usbproto object audit: {n_stores} stores, all stack/arg/"
          "adrp-image based; undef empty; bans clean")
    n_stores = check_usbproto_object(repo_root / "shim" / "out" / "usbpool.o")
    print(f"PASS usbpool object audit: {n_stores} stores, all stack/arg/"
          "adrp-image based; undef empty; bans clean (same no-MMIO audit as usbproto.o)")
    usbgadget_source_selftest()
    check_usbgadget_source_text((repo_root / "shim" / "usbgadget.c").read_text())
    print("PASS usbgadget source audit: no asm; try_write32 and the DWC3 base only in w32/r32; "
          "no literal pointer casts; w32 consults its exact allowlist")
    usbgadget_object_selftest()
    check_usbgadget_object(repo_root / "shim" / "out" / "usbgadget.o")
    print("PASS usbgadget writer audit: try_write32 referenced only by a "
          "bl inside w32; undef subset; no banned instructions")
    touchprobe_source_selftest()
    check_usbgadget_source_text((repo_root / "shim" / "touchprobe.c").read_text(), "touchprobe.c", **_TOUCH_KW)
    print("PASS touchprobe source audit: no asm / volatile / pointer casts; try_write32 only in w32; w32 checks "
          "w32_allowed before writing; allowlist depends on the address")
    touchprobe_object_selftest(repo_root / "shim" / "out" / "touchprobe.o")
    n_stores = check_touchprobe_object(repo_root / "shim" / "out" / "touchprobe.o")
    print(f"PASS touchprobe object audit: try_write32 referenced only by a bl inside w32; {n_stores} stores outside w32, "
          "all stack/argument based; undef subset; no banned instructions")
    acpm_source_selftest()
    check_usbgadget_source_text((repo_root / "shim" / "acpm.c").read_text(), "acpm.c", **_ACPM_KW)
    print("PASS acpm source audit: no asm / volatile / pointer casts; try_write32 only in w32; w32 checks "
          "w32_allowed before writing; allowlist depends on the address (this proves the write PATH; the ADDRESSES come from SRAM "
          "words checked by acpm_describe and by shim/acpm_host_test.c test_guard, not by this audit)")
    touchprobe_object_selftest(repo_root / "shim" / "out" / "acpm.o", _ACPM_ALLOWED_UNDEF, "acpm")
    n_stores = check_touchprobe_object(repo_root / "shim" / "out" / "acpm.o", _ACPM_ALLOWED_UNDEF)
    print(f"PASS acpm object audit: try_write32 referenced only by a bl inside w32; {n_stores} stores outside w32, "
          "all stack/argument based; undef subset; no banned instructions")
    udump_object_selftest()
    n_stores = check_udump_object(repo_root / "shim" / "out" / "udump.o")
    print(f"PASS udump object audit: {n_stores} stores, all stack/argument-"
          "based; .data/.bss empty; undef subset (no SCSI surface, no MMIO)")
    with tempfile.TemporaryDirectory() as temp:
        host_test = pathlib.Path(temp) / "loader_host_test"
        fdt_test = pathlib.Path(temp) / "fdt_host_test"
        subprocess.run([
            "gcc", "-Wall", "-Wextra", "-Ishim", "shim/payload.c",
            "shim/loader_host_test.c", "-o", str(host_test),
        ], check=True, cwd=repo_root)
        subprocess.run([str(host_test)], check=True, cwd=repo_root)
        subprocess.run([
            "gcc", "-Wall", "-Wextra", "-Ishim", "shim/fdt.c",
            "shim/fdt_host_test.c", "-o", str(fdt_test),
        ], check=True, cwd=repo_root)
        subprocess.run([str(fdt_test)], check=True, cwd=repo_root)
    fmt = "lz4" if is_lz4 else "raw"
    print(f"PASS header v{header_version} ({fmt}): kernel={kernel_size} ramdisk={ramdisk_size}; host tests & safety checks passed")


if __name__ == "__main__":
    main()
