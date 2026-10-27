"""Static recompiler: Z80 (Game Gear ROM) -> C.

  python3 -I tools/recomp.py ROM disasm/code_map.json OUTDIR [coverage files...]

Every entry point (bank, addr) becomes `void f_BB_AAAA(Z80 *z)`.  A function's
body is everything reachable from its entry through jumps/branches, emitted as
labelled C statements with gotos; calls become C calls; jumps to another entry
become tail calls; `ret` pops PC and returns.  After every instruction the
generated code adds the exact T-states the interpreter would and runs the same
scheduler check (scanlines, interrupts), so recompiled and interpreted runs are
bit-identical.  Anything not known statically goes through gg_jump()/gg_call(),
which fall back to the interpreter.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from z80dis import decode  # noqa: E402

ROM = open(sys.argv[1], "rb").read()
CMAP = json.load(open(sys.argv[2]))
OUT = sys.argv[3]
COVERAGE = sys.argv[4:]
NBANKS = len(ROM) // 0x4000
HERE = os.path.dirname(os.path.abspath(__file__))
HINTS = json.load(open(os.path.join(HERE, "hints.json")))

# cycle table shared with the interpreter (parsed so the two can never drift)
_interp = open(os.path.join(HERE, "..", "runtime", "src", "interp.c")).read()
_tbl = _interp[_interp.index("cyc_main[256] = {"):]
_tbl = _tbl[_tbl.index("{") + 1:_tbl.index("};")]
CYC = [int(x) for x in re.findall(r"\d+", _tbl)]
assert len(CYC) == 256

NOT_CODE = {(h["bank"], h["addr"]) for h in HINTS.get("not_code", [])}


def bank_of(addr, home):
    """Physical bank for a CPU address as seen by code living in bank `home`."""
    if addr < 0x4000:
        return 0
    if addr < 0x8000:
        return 1
    if addr < 0xC000:
        return home if home >= 2 else None  # None: depends on run-time mapping
    return None  # RAM


def rd(bank, addr):
    b = bank_of(addr, bank)
    if b is None:
        raise ValueError(f"read outside ROM at {addr:04X} from bank {bank}")
    return ROM[b * 0x4000 + (addr & 0x3FFF)]


def fname(bank, addr):
    return f"f_{bank:02X}_{addr:04X}"


def home_bank(bank, addr):
    """Bank a function is keyed under in the dispatch table."""
    return bank_of(addr, bank)


# ---------------------------------------------------------------- operands
R8 = ["z->b", "z->c", "z->d", "z->e", "z->h", "z->l", None, "z->a"]
R8IX = {1: ["z->b", "z->c", "z->d", "z->e", "z->ixh", "z->ixl", None, "z->a"],
        2: ["z->b", "z->c", "z->d", "z->e", "z->iyh", "z->iyl", None, "z->a"]}
RP = ["z->bc", "z->de", "z->hl", "z->sp"]
IXN = {0: "z->hl", 1: "z->ix", 2: "z->iy"}
CC = ["!(z->f & FZ)", "(z->f & FZ)", "!(z->f & FC)", "(z->f & FC)",
      "!(z->f & FP)", "(z->f & FP)", "!(z->f & FS)", "(z->f & FS)"]


def r8(i, idx):
    return R8IX[idx][i] if idx else R8[i]


def rp(i, idx):
    return IXN[idx] if i == 2 else RP[i]


def s8(v):
    return v - 256 if v >= 128 else v


class Insn:
    """C translation of one instruction."""

    def __init__(self, addr):
        self.addr = addr
        self.body = []        # statements executed before cycles are charged
        self.cycles = 0       # cycles when falling through / not taken
        self.rinc = 1
        self.kind = "seq"     # seq | jump | cjump | call | ccall | ret | cret | jpind | halt | stop
        self.target = None
        self.cond = None
        self.taken_cycles = 0
        self.length = 0
        self.ret_extra = None  # e.g. iff restore for RETN


def translate(bank, addr):
    """Decode the instruction at addr (code living in `bank`) into an Insn."""
    I = Insn(addr)
    p = addr
    rb = lambda a: rd(bank, a & 0xFFFF)  # noqa: E731
    op = rb(p); p += 1
    idx = 0
    nprefix = 0
    while op in (0xDD, 0xFD):
        idx = 1 if op == 0xDD else 2
        nprefix += 1
        op = rb(p); p += 1
    I.rinc = 1 + nprefix
    B = I.body

    if op == 0xCB:
        if idx:
            d = s8(rb(p)); p += 1
            op2 = rb(p); p += 1
            x, y, r = op2 >> 6, (op2 >> 3) & 7, op2 & 7
            ea = f"(uint16_t)({IXN[idx]} + {d})"
            B.append(f"{{ uint16_t ea = {ea}; uint8_t v = rd(z, ea);")
            if x == 1:
                B.append(f"  bit(z, {y}, v, (uint8_t)(ea >> 8)); }}")
                I.cycles = 20
            else:
                B.append(f"  v = cb_op(z, {x}, {y}, v); wr(z, ea, v);" + (f" {R8[r]} = v;" if r != 6 else "") + " }")
                I.cycles = 23
            I.rinc = 1 + nprefix  # DD + CB (displacement/op are not M1 fetches)
        else:
            op2 = rb(p); p += 1
            I.rinc = 2
            x, y, r = op2 >> 6, (op2 >> 3) & 7, op2 & 7
            if r == 6:
                if x == 1:
                    B.append(f"bit(z, {y}, rd(z, z->hl), z->h);"); I.cycles = 12
                else:
                    B.append(f"wr(z, z->hl, cb_op(z, {x}, {y}, rd(z, z->hl)));"); I.cycles = 15
            else:
                if x == 1:
                    B.append(f"bit(z, {y}, {R8[r]}, {R8[r]});")
                else:
                    B.append(f"{R8[r]} = cb_op(z, {x}, {y}, {R8[r]});")
                I.cycles = 8
        I.length = p - addr
        return I

    if op == 0xED:
        op2 = rb(p); p += 1
        I.rinc = 2 + nprefix
        x, y, zz = op2 >> 6, (op2 >> 3) & 7, op2 & 7
        pp, q = y >> 1, y & 1
        I.cycles = 8
        if x == 1:
            if zz == 0:
                B.append("{ uint8_t v = op_in_c(z);" + (f" {R8[y]} = v;" if y != 6 else " (void)v;") + " }"); I.cycles = 12
            elif zz == 1:
                B.append(f"Z80_OUT(z->c, {'0' if y == 6 else R8[y]});"); I.cycles = 12
            elif zz == 2:
                B.append(f"z->hl = {'adc16' if q else 'sbc16'}(z, z->hl, {RP[pp]});"); I.cycles = 15
            elif zz == 3:
                nn = rb(p) | rb(p + 1) << 8; p += 2
                B.append(f"{RP[pp]} = rd16(z, 0x{nn:04X});" if q else f"wr16(z, 0x{nn:04X}, {RP[pp]});")
                I.cycles = 20
            elif zz == 4:
                B.append("op_neg(z);")
            elif zz == 5:
                I.kind = "ret"; I.cycles = 14; I.ret_extra = "z->iff1 = z->iff2;"
            elif zz == 6:
                im = 0 if (y & 3) in (0, 1) else (y & 3) - 1
                B.append(f"z->im = {im};")
            else:
                if y == 0: B.append("z->i = z->a;"); I.cycles = 9
                elif y == 1: B.append("z->r = z->a;"); I.cycles = 9
                elif y == 2: B.append("op_ld_a_ir(z, z->i);"); I.cycles = 9
                elif y == 3: B.append("op_ld_a_ir(z, z->r);"); I.cycles = 9
                elif y == 4: B.append("op_rrd(z);"); I.cycles = 18
                elif y == 5: B.append("op_rld(z);"); I.cycles = 18
        elif x == 2 and zz <= 3 and y >= 4:
            d = -1 if y & 1 else 1
            rep = y >= 6
            fn = ["blk_ld", "blk_cp", "blk_in", "blk_out"][zz]
            I.cycles = 16
            if not rep:
                B.append(f"{fn}(z, {d});" if zz != 1 else f"(void)blk_cp(z, {d});")
            else:
                cnd = {0: "z->bc", 1: "z->bc && r", 2: "z->b", 3: "z->b"}[zz]
                call = f"uint8_t r = blk_cp(z, {d});" if zz == 1 else f"{fn}(z, {d});"
                I.kind = "block"
                I.block = (call, cnd)
        I.length = p - addr
        return I

    x, y, zz = op >> 6, (op >> 3) & 7, op & 7
    pp, q = y >> 1, y & 1
    cyc = CYC[op] + 4 * nprefix

    def memaddr():
        nonlocal p, cyc
        if idx:
            d = s8(rb(p)); p += 1
            cyc += 8
            return f"(uint16_t)({IXN[idx]} + {d})"
        return "z->hl"

    if x == 0:
        if zz == 0:
            if y == 0:
                pass
            elif y == 1:
                B.append("op_ex_af(z);")
            else:
                d = s8(rb(p)); p += 1
                tgt = (p + d) & 0xFFFF
                I.target = tgt
                if y == 2:
                    I.kind = "cjump"; I.cond = "--z->b"; I.taken_cycles = cyc + 5
                elif y == 3:
                    I.kind = "jump"
                else:
                    I.kind = "cjump"; I.cond = CC[y - 4]; I.taken_cycles = cyc + 5
        elif zz == 1:
            if q == 0:
                nn = rb(p) | rb(p + 1) << 8; p += 2
                B.append(f"{rp(pp, idx)} = 0x{nn:04X};")
            else:
                B.append(f"{IXN[idx]} = add16(z, {IXN[idx]}, {rp(pp, idx)});")
        elif zz == 2:
            if op == 0x02: B.append("wr(z, z->bc, z->a);")
            elif op == 0x12: B.append("wr(z, z->de, z->a);")
            elif op == 0x0A: B.append("z->a = rd(z, z->bc);")
            elif op == 0x1A: B.append("z->a = rd(z, z->de);")
            else:
                nn = rb(p) | rb(p + 1) << 8; p += 2
                if op == 0x22: B.append(f"wr16(z, 0x{nn:04X}, {IXN[idx]});")
                elif op == 0x32: B.append(f"wr(z, 0x{nn:04X}, z->a);")
                elif op == 0x2A: B.append(f"{IXN[idx]} = rd16(z, 0x{nn:04X});")
                else: B.append(f"z->a = rd(z, 0x{nn:04X});")
        elif zz == 3:
            B.append(f"{rp(pp, idx)}{'--' if q else '++'};")
        elif zz in (4, 5):
            fn = "inc8" if zz == 4 else "dec8"
            if y == 6:
                ea = memaddr()
                B.append(f"{{ uint16_t ea = {ea}; wr(z, ea, {fn}(z, rd(z, ea))); }}")
            else:
                r = r8(y, idx)
                B.append(f"{r} = {fn}(z, {r});")
        elif zz == 6:
            if y == 6:
                if idx:
                    d = s8(rb(p)); p += 1
                    cyc += 5
                    ea = f"(uint16_t)({IXN[idx]} + {d})"
                else:
                    ea = "z->hl"
                n = rb(p); p += 1
                B.append(f"wr(z, {ea}, 0x{n:02X});")
            else:
                n = rb(p); p += 1
                B.append(f"{r8(y, idx)} = 0x{n:02X};")
        else:
            B.append(["op_rlca(z);", "op_rrca(z);", "op_rla(z);", "op_rra(z);",
                      "daa(z);", "op_cpl(z);", "op_scf(z);", "op_ccf(z);"][y])
    elif x == 1:
        if op == 0x76:
            I.kind = "halt"
        elif y == 6:
            ea = memaddr()
            B.append(f"wr(z, {ea}, {R8[zz]});")
        elif zz == 6:
            ea = memaddr()
            B.append(f"{R8[y]} = rd(z, {ea});")
        else:
            B.append(f"{r8(y, idx)} = {r8(zz, idx)};")
    elif x == 2:
        v = f"rd(z, {memaddr()})" if zz == 6 else r8(zz, idx)
        B.append(f"alu(z, {y}, {v});")
    else:
        if zz == 0:
            I.kind = "cret"; I.cond = CC[y]; I.taken_cycles = cyc + 6
        elif zz == 1:
            if q == 0:
                B.append(f"{'z->af' if pp == 3 else rp(pp, idx)} = pop(z);")
            elif pp == 0:
                I.kind = "ret"
            elif pp == 1:
                B.append("op_exx(z);")
            elif pp == 2:
                I.kind = "jpind"; I.target_expr = IXN[idx]
            else:
                B.append(f"z->sp = {IXN[idx]};")
        elif zz == 2:
            nn = rb(p) | rb(p + 1) << 8; p += 2
            I.kind = "cjump"; I.cond = CC[y]; I.target = nn; I.taken_cycles = cyc
        elif zz == 3:
            if y == 0:
                nn = rb(p) | rb(p + 1) << 8; p += 2
                I.kind = "jump"; I.target = nn
            elif y == 2:
                n = rb(p); p += 1
                B.append(f"Z80_OUT(0x{n:02X}, z->a);")
            elif y == 3:
                n = rb(p); p += 1
                B.append(f"z->a = Z80_IN(0x{n:02X});")
            elif y == 4:
                B.append(f"op_ex_sp(z, &{IXN[idx]});")
            elif y == 5:
                B.append("{ uint16_t t = z->de; z->de = z->hl; z->hl = t; }")
            elif y == 6:
                B.append("z->iff1 = z->iff2 = 0; z->ei_delay = 0;")
            else:
                B.append("z->iff1 = z->iff2 = 1; z->ei_delay = 1;")
        elif zz == 4:
            nn = rb(p) | rb(p + 1) << 8; p += 2
            I.kind = "ccall"; I.cond = CC[y]; I.target = nn; I.taken_cycles = cyc + 7
        elif zz == 5:
            if q == 0:
                B.append(f"push(z, {'z->af' if pp == 3 else rp(pp, idx)});")
            elif pp == 0:
                nn = rb(p) | rb(p + 1) << 8; p += 2
                I.kind = "call"; I.target = nn
            # DD/ED/FD prefixes handled above
        elif zz == 6:
            n = rb(p); p += 1
            B.append(f"alu(z, {y}, 0x{n:02X});")
        else:
            I.kind = "call"; I.target = y * 8
    I.cycles = cyc
    I.length = p - addr
    return I


# ---------------------------------------------------------------- entries
def norm_entry(bank, addr):
    b = bank_of(addr, bank)
    if b is None:
        return None
    return (b, addr)


entries = set()   # every address that gets its own C function (dispatchable)
strong = set()    # entries that also split other functions' bodies (real function starts)


def add_entry(bank, addr, split=True):
    e = norm_entry(bank, addr)
    if e and e not in NOT_CODE and e[0] < NBANKS:
        entries.add(e)
        if split:
            strong.add(e)


for f in CMAP["functions"]:
    add_entry(f["bank"], f["addr"])
for l in CMAP["labels"]:
    if l["kind"] in ("func", "vector", "state"):
        add_entry(l["bank"], l["addr"])
for t in CMAP["jump_tables"]:
    for e in t["entries"]:
        add_entry(t["bank"] if e >= 0x8000 else 0, e)
for s in CMAP["states"]:
    add_entry(s["bank"], s["entry"])
add_entry(0, 0x0000)
add_entry(0, 0x0038)
api = HINTS.get("api_vector")
if api:
    for i in range(api["count"]):
        add_entry(0, api["start"] + i * 3)
for h in HINTS.get("extra_entries", []):
    add_entry(h["bank"], h["addr"])
dyn = 0
known_code = {(l["bank"], l["addr"]) for l in CMAP["labels"]}   # (bank, addr) seen as code
for path in COVERAGE:
    for line in open(path):
        k, b, a = line.split()
        if k == "R":
            continue
        known_code.add((int(b, 16), int(a, 16)))
        if k in ("E", "W"):
            # W = reached by a 'ret' to an unexpected address: only needs to be
            # dispatchable, it is not a function start
            add_entry(int(b, 16), int(a, 16), split=(k == "E"))
            dyn += 1


# ---------------------------------------------------------------- bodies
def is_local(home, tgt):
    """Can code living in `home` reach tgt with a static goto/call?"""
    if tgt >= 0xC000:
        return False
    if tgt >= 0x8000:
        return home >= 2
    return True


def build(entry):
    """Collect instructions reachable from entry by jumps (not calls)."""
    bank, addr = entry
    home = bank  # for fixed banks, slot-2 targets are dynamic
    insns = {}
    goto_targets = set()
    tail = set()
    work = [addr]
    while work:
        a = work.pop()
        if a in insns:
            continue
        if a != addr and (bank_of(a, home), a) in strong:
            tail.add(a)
            continue
        try:
            I = translate(home, a)
        except ValueError:
            tail.add(a)
            continue
        # don't run off the end of the bank
        if (a & 0x3FFF) + I.length > 0x4000 and a >= 0x4000:
            raise RuntimeError(f"instruction crosses bank boundary at {bank:02X}:{a:04X}")
        insns[a] = I
        nxt = (a + I.length) & 0xFFFF
        I.next = nxt

        def follow(t):
            if is_local(home, t):
                if (bank_of(t, home), t) in strong and t != addr:
                    tail.add(t)
                else:
                    goto_targets.add(t)
                    work.append(t)

        if I.kind in ("seq", "call", "ccall", "cret", "halt", "block"):
            follow(nxt)
        if I.kind in ("jump", "cjump"):
            if I.target is not None:
                follow(I.target)
            if I.kind == "cjump":
                follow(nxt)
        if I.kind == "block":
            goto_targets.add(a)
    return insns, goto_targets, tail


def call_expr(home, tgt):
    """C statement that performs a call (return address already pushed)."""
    b = bank_of(tgt, home)
    if b is not None and tgt < 0xC000 and (b, tgt) in entries:
        return f"{fname(b, tgt)}(z);"
    return f"gg_call(z, 0x{tgt:04X});"


def jump_expr(home, tgt):
    b = bank_of(tgt, home)
    if b is not None and tgt < 0xC000 and (b, tgt) in entries:
        return f"{fname(b, tgt)}(z); return;"
    return f"z->pc = 0x{tgt:04X}; gg_jump(z); return;"


def emit(entry):
    bank, addr = entry
    home = bank
    insns, gotos, tail = build(entry)
    out = [f"void {fname(bank, addr)}(Z80 *z)", "{"]
    order = [addr] + sorted(x for x in insns if x != addr)

    def go(t):
        if t in insns and is_local(home, t):
            return f"goto L_{t:04X};"
        return jump_expr(home, t)

    for i, a in enumerate(order):
        I = insns[a]
        out.append(f"L_{a:04X}:")
        if I.kind != "block":
            out.append(f"    INC_R(z, {I.rinc});")  # M1 fetches happen before execution
        for st in I.body:
            out.append("    " + st)
        R = ""
        nx = I.next
        follows = i + 1 < len(order) and order[i + 1] == nx
        fall = "" if follows else go(nx)
        k = I.kind
        if k == "seq":
            out.append(f"    {R} STEP({I.cycles}, 0x{nx:04X}); {fall}")
        elif k == "jump":
            out.append(f"    {R} STEP({I.cycles}, 0x{I.target:04X}); {go(I.target)}")
        elif k == "cjump":
            out.append(f"    {R} if ({I.cond}) {{ STEP({I.taken_cycles}, 0x{I.target:04X}); {go(I.target)} }}")
            out.append(f"    STEP({I.cycles}, 0x{nx:04X}); {fall}")
        elif k in ("call", "ccall"):
            cyc = I.taken_cycles if k == "ccall" else I.cycles
            body = (f"{{ uint16_t S = z->sp; push(z, 0x{nx:04X}); STEP({cyc}, 0x{I.target:04X}); "
                    f"{call_expr(home, I.target)} if ((z->pc != 0x{nx:04X} || z->sp != S) && gg_resume(z, 0x{nx:04X}, S)) return; }}")
            if k == "ccall":
                out.append(f"    {R} if ({I.cond}) {{ {body} }} else {{ STEP({I.cycles}, 0x{nx:04X}); }}")
            else:
                out.append(f"    {R} {body}")
            if fall:
                out.append(f"    {fall}")
        elif k == "ret":
            extra = (I.ret_extra + " ") if I.ret_extra else ""
            out.append(f"    {R} z->pc = pop(z); {extra}STEP({I.cycles}, z->pc); return;")
        elif k == "cret":
            out.append(f"    {R} if ({I.cond}) {{ z->pc = pop(z); STEP({I.taken_cycles}, z->pc); return; }}")
            out.append(f"    STEP({I.cycles}, 0x{nx:04X}); {fall}")
        elif k == "jpind":
            out.append(f"    {R} z->pc = {I.target_expr}; STEP({I.cycles}, z->pc); gg_jump(z); return;")
        elif k == "halt":
            out.append(f"    {R} z->halted = 1; STEP(4, 0x{nx:04X});")
            out.append(f"    while (z->halted) {{ STEP(4, 0x{nx:04X}); }} {fall}")
        elif k == "block":
            call, cnd = I.block
            out.append(f"    {{ INC_R(z, {I.rinc}); {call} if ({cnd}) {{ STEP(21, 0x{a:04X}); goto L_{a:04X}; }} }}")
            out.append(f"    STEP(16, 0x{nx:04X}); {fall}")
    out.append("}")
    return "\n".join(out), tail, insns


# ---------------------------------------------------------------- driver
def main():
    os.makedirs(OUT, exist_ok=True)
    # 1. close the entry set over static call / tail-call targets.  Fixed-bank
    # code that jumps into slot 2 can't be resolved statically; every bank in
    # which that address is known code gets an entry so dispatch finds it.
    pending = sorted(entries)
    seen = set()
    slot2_targets = set()
    while pending:
        e = pending.pop()
        if e in seen:
            continue
        seen.add(e)
        insns, gotos, tail = build(e)
        refs = set(tail)
        for I in insns.values():
            if I.kind in ("call", "ccall") and I.target is not None:
                refs.add(I.target)
        for I in insns.values():
            if I.kind in ("jump", "cjump", "call", "ccall") and I.target is not None \
                    and 0x8000 <= I.target < 0xC000 and e[0] < 2 and I.target not in slot2_targets:
                slot2_targets.add(I.target)
                for (kb, ka) in known_code:
                    if ka == I.target and kb >= 2 and (kb, ka) not in entries:
                        add_entry(kb, ka)
                        pending.append((kb, ka))
        for t in refs:
            b = bank_of(t, e[0])
            if b is not None and t < 0xC000 and (b, t) not in strong and (b, t) not in NOT_CODE:
                add_entry(b, t)
                pending.append((b, t))
    # 2. emit (bodies depend on the final entry set)
    done = {}
    for e in sorted(entries):
        done[e] = emit(e)[0]
    funcs = sorted(done)
    with open(os.path.join(OUT, "recomp.h"), "w") as f:
        f.write("/* GENERATED by tools/recomp.py - do not edit */\n#ifndef RECOMP_H\n#define RECOMP_H\n"
                "#include \"z80ops.h\"\n\n"
                "#define STEP(n, next) do { z->cycles += (n); GG_TRACE_HOOK(z, (uint16_t)(next)); if (GG_NEED_CHECK(z)) gg_check(z, (uint16_t)(next)); } while (0)\n\n")
        for e in funcs:
            f.write(f"void {fname(*e)}(Z80 *z);\n")
        f.write("\n#endif\n")
    by_bank = {}
    for e in funcs:
        by_bank.setdefault(e[0], []).append(e)
    total_lines = 0
    for b, es in by_bank.items():
        with open(os.path.join(OUT, f"bank{b:02X}.c"), "w") as f:
            f.write("/* GENERATED by tools/recomp.py - do not edit */\n#include \"recomp.h\"\n")
            f.write("#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n")
            for e in es:
                f.write(done[e] + "\n\n")
                total_lines += done[e].count("\n")
    with open(os.path.join(OUT, "recomp_table.c"), "w") as f:
        f.write("/* GENERATED by tools/recomp.py - do not edit */\n#include \"recomp.h\"\n\n"
                "typedef struct { uint16_t bank, addr; gg_fn fn; } gg_entry;\n"
                f"const gg_entry gg_recomp_table[{len(funcs)}] = {{\n")
        for e in funcs:
            f.write(f"    {{ 0x{e[0]:02X}, 0x{e[1]:04X}, {fname(*e)} }},\n")
        f.write(f"}};\nconst unsigned gg_recomp_count = {len(funcs)};\n")
    print(f"functions: {len(funcs)} (dynamic entries read: {dyn}), banks: {sorted(by_bank)}, C lines: {total_lines}")


main()
