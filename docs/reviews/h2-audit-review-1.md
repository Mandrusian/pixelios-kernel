# Review: commit 886ddc3 (audit H2 fix) in tools/test-bluejay-boot.py

Method: loaded the file before (886ddc3^) and after as modules in /tmp/rv and
fed synthetic objdump text through `check_usb_object_text` with the exact udump
policy (`trusted_args=x0-x7`, `allow_adrp=False`, `_UDUMP_ALLOWED_UNDEF`). Also
ran old and new on the built `shim/out/{usbprobe,usbproto,udump}.o`.
"PASS" below means the audit accepted the store, which is a hole when the
address is arbitrary. Harness: /tmp/rv/h.py, h2.py, h3.py, h4.py.

## Verdict on (1): improved, but NOT sound. Arbitrary-address stores still pass for udump.o.

### Closed by the change (old PASS -> new REJECT)
| sequence | new verdict |
|---|---|
| `ldr x4,[x0,#8]; str w1,[x4]` | reject (load from arg struct is not trusted) |
| `add x1,x1,x0; str w2,[x1]` | reject (reg operand kills class) |
| `mov x0,#0x11110000; str w3,[x1,x0,lsl #2]` | reject (register-offset) |
| `movz x1,..; stp x2,x3,[x1,#16]!` | reject |
| `csel x1,..; str [x1]` / `ldr x1,literal; str [x1]` | reject |

### STILL PASSING with an arbitrary address (new == PASS)

**H-1 (High): `[sp, xN]` register offset is never checked.**
```
mov x0, #0x11110000
str w3, [sp, x0]
```
Verdict: PASS(1). The register-offset regex only matches a first operand of
the form `x\d+`, so a first operand of `sp` is skipped and `base == "sp"`
satisfies the base check. Address = sp + arbitrary.

**H-2 (High): 32-bit / extended index registers are never checked.**
```
mov  w0, #0x1111
movk w0, #0x1111, lsl #16
str  w3, [x1, w0, uxtw #2]
```
Verdict: PASS(1). Regex for the offset is `x\d+`, so `w0` (uxtw/sxtw forms,
which GCC emits) is invisible. Address = arg + arbitrary 32-bit value (reaches
MMIO). Same for `sxtw`. Note `[x1, x0, sxtx]` IS checked (reject).

**H-3 (High): `add xD, sp, xM` keeps the sp class for any xM.**
```
mov x0, #0x11110000
add x9, sp, x0
str w3, [x9]
```
Verdict: PASS(1). The `is_imm` rule is applied only to the "x0" class; the
"sp" class propagates through any register operand (deliberately, for
loop-index addressing into stack buffers, see header comment) with no bound.

**M-1 (Medium): writeback of a register post-index is not modelled.**
```
mov x2, #0x11110000
st1 {v0.4s}, [x1], x2
st1 {v0.4s}, [x1]
```
Verdict: PASS(2). x1 keeps its trusted class after being advanced by an
arbitrary register. The offset regex does not match `[x1], x2`.

**M-2 (Medium): immediates are unbounded.**
`add x1,x1,#0xfff,lsl #12; str w4,[x1]` PASS; post/pre-index immediates
likewise; repeated in a loop gives any offset. Inherent to class tracking
without ranges; the audit trusts "arg buffer + small constant".

**M-3 (Medium): spill-slot map is unsound.** Slots (`slots` dict) are only
updated by `str ... [sp,#imm]` with the exact same offset key. All PASS(3/2):
```
str x1,[sp,#8]; add x9,sp,#8; mov x3,#0x11110000; str x3,[x9]; ldr x2,[sp,#8]; str w4,[x2]   (alias overwrite)
str x1,[sp,#8]; mov x3,#0x11; strb w3,[sp,#9]; ldr x2,[sp,#8]; str w4,[x2]                  (partial overwrite)
str x1,[sp,#8]; sub sp,sp,#0x10; ldr x2,[sp,#8]; str w4,[x2]                                (sp moved; offset now means a different slot)
```
Slot class survives. Stores through sp-derived registers, sub-slot writes,
and sp adjustment do not invalidate it.

**L-1 (Low): `[x1, x0]` with two argument registers is accepted by design**
(`str w3,[x1,x0]` PASS, also `mov x0,x5; str w3,[x1,x0]`). The sum of two
pointers/values is not a buffer address; the caller controls both.

**L-2 (Low, pre-existing, out of scope of this commit):** `blr x9` / `br`
are in `_USB_NO_WRITE`, so `mov x9,#addr; blr x9` passes (0 stores). The
audit proves stores in the function only; indirect jumps escape it.

For real GCC output of udump.o the audit passes (75 stores) and these
holes are not exercised, but the stated goal ("every store base provably
stack or argument buffer") is not met against a hostile or unlucky
sequence; H-1 and H-2 are cheap to close (treat `sp` base the same as `xN`,
match `[wx]\d+` including extend forms, and reject unless the index is an
immediate or provably a bounded value; or simply reject any register-offset
store in udump.o, since a correct udump apparently needs none: control case
`[x1,x0,lsl #2]` is accepted only because both are "trusted").

## (2) Did it loosen any check for usbprobe/usbproto/usbgadget? No.

Built objects, old vs new (`check_usb_object_text`, each with its real
policy): usbprobe.o 119 stores PASS/PASS, usbproto.o 38 PASS/PASS,
udump.o 75 PASS/PASS. (usbgadget.o has its own checker, not touched.)

Semantics:
- usbprobe (no trusted args, `allow_adrp=False`): the extra register-offset
  check is skipped (`trusted` empty). Only change: in `put_*` helpers the
  literal `x0` was formerly trusted forever (`r == "x0"`); now only until
  redefined (tightening). A non-put function can no longer treat x0 as
  trusted via the old `is_put and ...` clause: same as before (is_put false).
- usbproto (`allow_adrp=True`): `cls` uses `allow_adrp and r in trusted`,
  i.e. identical to the old `r in trusted`: static, never invalidated. The
  `is_imm` kill is bypassed by `allow_adrp`, the register-offset check is
  skipped. Hence usbproto got NO H2 protection and is bit-for-bit as before
  (no loosening). Evidence, old and new both PASS(1): `mov x1,#0x11110000;
  str w3,[x1]` and `ldr x1,[x0]; str w3,[x1]` under usbproto policy. The
  commit message ("invalidate trusted arg registers on overwrite") is only
  true for udump, not usbproto: same hole remains there (Medium).
- Side effect for a non-put function when `trusted` is non-empty and
  `allow_adrp=False`: init x0-set is `trusted`, but `is_put` functions get
  only `{"x0"}` (trusted ignored); stricter than before for `put_*`.

## (3) The `is_imm` rule

In the `add/sub` branch, when the source register has class "x0"
(arg-derived), the destination keeps that class only if the third operand
is an immediate (`toks[2].startswith("#")`), OR `allow_adrp` (usbproto) OR
the function is a static `put_*` helper; otherwise the destination is
killed. Intent: `p = buf + CONST` stays a buffer pointer, `p = buf + var`
(unbounded index) is not provable, so it loses trust.

Is it wrong? Not wrong in direction, but weak and inconsistent:
- Immediate-only is not a bound (M-2); `add x1,x1,#0xfff,lsl #12` passes.
- `toks[2]` for `add x1, x0, #0x10` is correct, but anything where the
  immediate is not the third token (`sub x1, x0, #1` ok) is fine; extended
  forms such as `add x1,x1,w2,uxtw` are killed (no `#`), good.
- Does not apply to the "sp" class (H-3) and is skipped for `put_*` and
  usbproto, so the same `add x9,x9,x2` (arbitrary x2) is accepted there.
- The `elif c == "x0"` branch covers `sub xD, xN, xM` too; fine.
- `adds/subs` are treated like add/sub; harmless.
- Interplay with the new register-offset check is inconsistent: `add
  x1,x1,x0` is killed but `str w,[x1,x0]` is accepted when both are
  "trusted" (L-1).

VERDICT: UNSOUND: arbitrary-address stores still pass for udump.o via `str w3,[sp,x0]`, `str w3,[x1,w0,uxtw #2]` (index regex only sees xN with a non-sp base), `add x9,sp,xN` (sp class unbounded), `st1 ..,[x1],xN` writeback, and spill-slot aliasing; change does not loosen the other objects (usbproto simply gets no H2 protection)
