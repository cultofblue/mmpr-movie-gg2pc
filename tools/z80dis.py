"""Table-driven Z80 instruction decoder.

decode(read, pc) -> Insn
  read(addr) returns the byte at a CPU address (16-bit).
Insn fields:
  length   number of bytes
  text     mnemonic with operands; 16-bit address operands are written as
           {T} so a caller can substitute a label
  kind     'seq' | 'jp' | 'jr' | 'call' | 'ret' | 'rst' | 'jpind' | 'halt' | 'retn'
  cond     True when the control transfer is conditional
  target   absolute target address for jp/jr/call/rst (else None)
  ref      absolute 16-bit immediate/memory operand (non-flow), or None
  undoc    True for undocumented forms (assemblers disagree on syntax)
"""

R = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
RP = ["bc", "de", "hl", "sp"]
RP2 = ["bc", "de", "hl", "af"]
CC = ["nz", "z", "nc", "c", "po", "pe", "p", "m"]
ALU = ["add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp "]
ROT = ["rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"]
IM = ["0", "0", "1", "2", "0", "0", "1", "2"]
BLI = {
    (4, 0): "ldi", (4, 1): "cpi", (4, 2): "ini", (4, 3): "outi",
    (5, 0): "ldd", (5, 1): "cpd", (5, 2): "ind", (5, 3): "outd",
    (6, 0): "ldir", (6, 1): "cpir", (6, 2): "inir", (6, 3): "otir",
    (7, 0): "lddr", (7, 1): "cpdr", (7, 2): "indr", (7, 3): "otdr",
}


class Insn:
    __slots__ = ("pc", "length", "text", "kind", "cond", "target", "ref", "undoc", "bytes")

    def __init__(self):
        self.kind = "seq"
        self.cond = False
        self.target = None
        self.ref = None
        self.undoc = False

    def __repr__(self):
        return f"<{self.pc:04x} {self.text}>"


def _s8(v):
    return v - 256 if v >= 128 else v


def decode(read, pc):
    ins = Insn()
    ins.pc = pc
    p = pc
    idx = None  # None, 'ix', 'iy'
    op = read(p)
    p += 1
    while op in (0xDD, 0xFD):
        idx = "ix" if op == 0xDD else "iy"
        op = read(p)
        p = (p + 1) & 0xFFFF
        if op in (0xDD, 0xFD):
            ins.undoc = True

    disp = None

    def imm8():
        nonlocal p
        v = read(p)
        p = (p + 1) & 0xFFFF
        return v

    def imm16():
        nonlocal p
        v = read(p) | (read((p + 1) & 0xFFFF) << 8)
        p = (p + 2) & 0xFFFF
        return v

    def get_disp():
        nonlocal disp
        if disp is None:
            disp = _s8(imm8())
        return disp

    def ireg(i, allow_hl_sub=True):
        """8-bit register i with index substitution."""
        if idx is None:
            return R[i]
        if i == 6:
            d = get_disp()
            return f"({idx}{'+' if d >= 0 else '-'}{abs(d)})"
        if allow_hl_sub and i in (4, 5):
            ins.undoc = True
            return idx + ("h" if i == 4 else "l")
        return R[i]

    def irp(i):
        if idx is not None and i == 2:
            return idx
        return RP[i]

    def irp2(i):
        if idx is not None and i == 2:
            return idx
        return RP2[i]

    def H(v):
        if v <= 9:
            return str(v)
        h = f"{v:x}"
        return f"0{h}h" if h[0] in "abcdef" else f"{h}h"

    if op == 0xCB:
        if idx is not None:
            d = get_disp()
            op2 = imm8()
            x, y, z = op2 >> 6, (op2 >> 3) & 7, op2 & 7
            m = f"({idx}{'+' if d >= 0 else '-'}{abs(d)})"
            if z != 6 or (x == 0 and y == 6):
                ins.undoc = True
            if x == 0:
                t = f"{ROT[y]} {m}"
            elif x == 1:
                t = f"bit {y},{m}"
            elif x == 2:
                t = f"res {y},{m}"
            else:
                t = f"set {y},{m}"
            if z != 6 and x != 1:
                t += f",{R[z]}"
        else:
            op2 = imm8()
            x, y, z = op2 >> 6, (op2 >> 3) & 7, op2 & 7
            if x == 0:
                if y == 6:
                    ins.undoc = True
                t = f"{ROT[y]} {R[z]}"
            elif x == 1:
                t = f"bit {y},{R[z]}"
            elif x == 2:
                t = f"res {y},{R[z]}"
            else:
                t = f"set {y},{R[z]}"
        ins.text = t
    elif op == 0xED:
        op2 = imm8()
        x, y, z = op2 >> 6, (op2 >> 3) & 7, op2 & 7
        q, pp = y & 1, y >> 1
        t = None
        if x == 1:
            if z == 0:
                t = "in (c)" if y == 6 else f"in {R[y]},(c)"
                if y == 6:
                    ins.undoc = True
            elif z == 1:
                t = "out (c),0" if y == 6 else f"out (c),{R[y]}"
                if y == 6:
                    ins.undoc = True
            elif z == 2:
                t = f"{'sbc' if q == 0 else 'adc'} hl,{RP[pp]}"
            elif z == 3:
                nn = imm16()
                ins.ref = nn
                t = f"ld ({{T}}),{RP[pp]}" if q == 0 else f"ld {RP[pp]},({{T}})"
            elif z == 4:
                t = "neg"
                ins.undoc = y != 0
            elif z == 5:
                if y == 1:
                    t = "reti"
                    ins.kind = "ret"
                else:
                    t = "retn"
                    ins.kind = "ret"
                    ins.undoc = y != 0
            elif z == 6:
                t = f"im {IM[y]}"
                ins.undoc = y not in (0, 2, 3)
                if t == "im 0/1":
                    t = "im 0"
            else:
                t = ["ld i,a", "ld r,a", "ld a,i", "ld a,r", "rrd", "rld", "nop", "nop"][y]
                ins.undoc = y >= 6
        elif x == 2 and z <= 3 and y >= 4:
            t = BLI[(y, z)]
        if t is None:
            t = "nop"
            ins.undoc = True
        ins.text = t
    else:
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        q, pp = y & 1, y >> 1
        t = None
        if x == 0:
            if z == 0:
                if y == 0:
                    t = "nop"
                elif y == 1:
                    t = "ex af,af'"
                else:
                    d = _s8(imm8())
                    tgt = (p + d) & 0xFFFF
                    ins.target = tgt
                    ins.kind = "jr"
                    if y == 2:
                        t = "djnz {T}"
                        ins.cond = True
                    elif y == 3:
                        t = "jr {T}"
                    else:
                        t = f"jr {CC[y - 4]},{{T}}"
                        ins.cond = True
            elif z == 1:
                if q == 0:
                    nn = imm16()
                    ins.ref = nn
                    t = f"ld {irp(pp)},{{T}}"
                else:
                    t = f"add {irp(2)},{irp(pp)}"
            elif z == 2:
                if q == 0:
                    if pp == 0:
                        t = "ld (bc),a"
                    elif pp == 1:
                        t = "ld (de),a"
                    elif pp == 2:
                        nn = imm16(); ins.ref = nn
                        t = f"ld ({{T}}),{irp(2)}"
                    else:
                        nn = imm16(); ins.ref = nn
                        t = "ld ({T}),a"
                else:
                    if pp == 0:
                        t = "ld a,(bc)"
                    elif pp == 1:
                        t = "ld a,(de)"
                    elif pp == 2:
                        nn = imm16(); ins.ref = nn
                        t = f"ld {irp(2)},({{T}})"
                    else:
                        nn = imm16(); ins.ref = nn
                        t = "ld a,({T})"
            elif z == 3:
                t = f"{'inc' if q == 0 else 'dec'} {irp(pp)}"
            elif z == 4:
                t = f"inc {ireg(y)}"
            elif z == 5:
                t = f"dec {ireg(y)}"
            elif z == 6:
                r = ireg(y)
                t = f"ld {r},{H(imm8())}"
            else:
                t = ["rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"][y]
        elif x == 1:
            if z == 6 and y == 6:
                t = "halt"
                ins.kind = "halt"
            else:
                # with (ix+d), the other operand uses plain h/l
                if idx is not None and (y == 6 or z == 6):
                    a = ireg(y, False) if y == 6 else R[y]
                    b = ireg(z, False) if z == 6 else R[z]
                else:
                    a, b = ireg(y), ireg(z)
                t = f"ld {a},{b}"
        elif x == 2:
            t = f"{ALU[y]}{ireg(z)}"
        else:
            if z == 0:
                t = f"ret {CC[y]}"
                ins.kind = "ret"
                ins.cond = True
            elif z == 1:
                if q == 0:
                    t = f"pop {irp2(pp)}"
                else:
                    if pp == 0:
                        t = "ret"
                        ins.kind = "ret"
                    elif pp == 1:
                        t = "exx"
                    elif pp == 2:
                        t = f"jp ({irp(2)})"
                        ins.kind = "jpind"
                    else:
                        t = f"ld sp,{irp(2)}"
            elif z == 2:
                nn = imm16()
                ins.target = nn
                ins.kind = "jp"
                ins.cond = True
                t = f"jp {CC[y]},{{T}}"
            elif z == 3:
                if y == 0:
                    nn = imm16()
                    ins.target = nn
                    ins.kind = "jp"
                    t = "jp {T}"
                elif y == 2:
                    t = f"out ({H(imm8())}),a"
                elif y == 3:
                    t = f"in a,({H(imm8())})"
                elif y == 4:
                    t = f"ex (sp),{irp(2)}"
                elif y == 5:
                    t = "ex de,hl"
                elif y == 6:
                    t = "di"
                else:
                    t = "ei"
            elif z == 4:
                nn = imm16()
                ins.target = nn
                ins.kind = "call"
                ins.cond = True
                t = f"call {CC[y]},{{T}}"
            elif z == 5:
                if q == 0:
                    t = f"push {irp2(pp)}"
                else:
                    nn = imm16()
                    ins.target = nn
                    ins.kind = "call"
                    t = "call {T}"
            elif z == 6:
                t = f"{ALU[y]}{H(imm8())}"
            else:
                ins.kind = "rst"
                ins.target = y * 8
                t = f"rst {H(y * 8)}"
        ins.text = t
    if idx is not None and "ix" not in ins.text and "iy" not in ins.text:
        ins.undoc = True  # prefix with no effect
    ins.length = (p - pc) & 0xFFFF
    ins.bytes = bytes(read((pc + i) & 0xFFFF) for i in range(ins.length))
    return ins
