"""Bank-aware recursive-descent tracer for the MMPR: The Movie (Game Gear) ROM.

Memory model (Sega mapper, as initialised by the game at $0070):
  slot 0  $0000-$3FFF  bank 0   (fixed; game never rewrites $FFFD)
  slot 1  $4000-$7FFF  bank 1   (fixed; game never rewrites $FFFE)
  slot 2  $8000-$BFFF  bank N   (switched via writes to $FFFF)

A code location is (bank, cpu_addr).  Fixed code is traced with an optional
known slot-2 bank so that "ld a,N / ld ($ffff),a / call $8xxx" resolves.

Usage: python3 -I trace.py ROM OUTDIR
Writes OUTDIR/code_map.json and OUTDIR/bankNN.asm
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from z80dis import decode  # noqa: E402

ROM = open(sys.argv[1], "rb").read()
OUT = sys.argv[2]
NBANKS = len(ROM) // 0x4000

HINTS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "hints.json")))
FETCH = {(h["bank"], h["addr"]) for h in HINTS["table_fetch_helpers"]}
NOT_CODE = {(h["bank"], h["addr"]) for h in HINTS.get("not_code", [])}
STATE_TABLE = 0x1D23
STATE_COUNT = 48  # entries verified by hand; see docs/NOTES.md
DISPATCH = 0x0016  # rst 28h + jp (hl): jump table in HL, index in A


def phys(bank, addr):
    return bank * 0x4000 + (addr & 0x3FFF)


def bank_of(addr, slot2):
    if addr < 0x4000:
        return 0
    if addr < 0x8000:
        return 1
    if addr < 0xC000:
        return slot2
    return None  # RAM


def reader(slot2):
    def r(a):
        b = bank_of(a, slot2)
        if b is None:
            return 0
        return ROM[phys(b, a)]
    return r


code = {}          # phys offset -> (bank, addr, insn)
owner = {}         # phys byte -> phys of instruction start
labels = {}        # (bank, addr) -> kind  ('func','label','table','vector','state')
tables = []        # dicts describing jump tables
unresolved = set() # (from_bank, from_addr, target) banked target with unknown slot2
bank_switches = [] # (bank, addr, value or None)
visited = set()
work = []


def add(addr, slot2, kind=None):
    b = bank_of(addr, slot2)
    if b is None or b >= NBANKS or (b, addr) in NOT_CODE:
        return False
    if kind:
        k = (b, addr)
        prev = labels.get(k)
        if prev is None or prev == "label":
            labels[k] = kind
    work.append((addr, slot2))
    return True


def label(addr, slot2, kind):
    b = bank_of(addr, slot2)
    if b is not None:
        k = (b, addr)
        if labels.get(k) in (None, "label") or kind != "label":
            labels.setdefault(k, kind)


def plausible(addr, slot2):
    if addr < 0x0070 and addr not in (0, 8, 0x10, 0x18, 0x20, 0x28, 0x38):
        return False
    if addr >= 0xC000:
        return False
    if 0x8000 <= addr and slot2 is None:
        return False
    return True


def trace(addr, slot2):
    a_val = None
    hl_val = None
    while True:
        b = bank_of(addr, slot2)
        if b is None or b >= NBANKS:
            return
        if addr >= 0x8000 and slot2 is None:
            return
        # fixed-region code is keyed with slot2 so banked calls can resolve
        key = (phys(b, addr), slot2 if addr < 0x8000 else None)
        if key in visited:
            return
        p = phys(b, addr)
        if p in owner and owner[p] != p:
            # jumps into the middle of an instruction: stop (overlap)
            return
        visited.add(key)
        ins = decode(reader(slot2), addr)
        # never let an instruction run off the end of its slot
        if (addr & 0x3FFF) + ins.length > 0x4000 and addr >= 0x8000:
            return
        code[p] = (b, addr, ins)
        for i in range(ins.length):
            owner.setdefault(p + i, p)
        nxt = (addr + ins.length) & 0xFFFF

        # --- tiny constant propagation for A and HL ---
        t = ins.text
        new_a, new_hl = a_val, hl_val
        if t.startswith("ld a,") and ins.bytes[0] == 0x3E:
            new_a = ins.bytes[1]
        elif t == "xor a":
            new_a = 0
        elif t.startswith("ld a,") or t.startswith(("inc a", "dec a", "pop af", "in a")) \
                or (ins.bytes[0] in range(0x80, 0xC0) and ins.bytes[0] & 0xF8 != 0xB8) \
                or ins.kind in ("call", "rst"):
            new_a = None
        if ins.bytes[0] == 0x21:
            new_hl = ins.ref
        elif "hl" in t.split(",")[0] or t.startswith(("pop hl", "ex de,hl", "inc hl", "dec hl", "add hl")) \
                or ins.kind in ("call", "rst"):
            new_hl = None

        if ins.bytes[:3] == b"\x32\xff\xff":
            bank_switches.append((b, addr, a_val))
            if a_val is not None:
                slot2 = a_val
            else:
                slot2 = None
        if ins.ref is not None and ins.bytes[0] in (0x21, 0x01, 0x11) and ins.ref == 0xFFFF:
            pass

        if ins.kind in ("jp", "jr"):
            tgt = ins.target
            if tgt >= 0x8000 and tgt < 0xC000 and bank_of(tgt, slot2) is None:
                unresolved.add((b, addr, tgt))
            elif plausible(tgt, slot2):
                add(tgt, slot2, "label")
            if not ins.cond:
                return
        elif ins.kind in ("call", "rst"):
            tgt = ins.target
            if (bank_of(tgt, slot2), tgt) in FETCH and hl_val is not None:
                # only a code table if the fetched word is jumped to next
                nxt_ins = decode(reader(slot2), nxt)
                if nxt_ins.kind == "jpind" and nxt_ins.text == "jp (hl)":
                    parse_table(hl_val, slot2, b, addr, a_val)
            if tgt == DISPATCH and hl_val is not None:
                parse_table(hl_val, slot2, b, addr, a_val)
                # handlers 'ret' back here, so execution continues
            if 0x8000 <= tgt < 0xC000 and slot2 is None:
                unresolved.add((b, addr, tgt))
            elif plausible(tgt, slot2):
                add(tgt, slot2, "func")
        elif ins.kind == "ret":
            if not ins.cond:
                return
        elif ins.kind == "jpind":
            return
        elif ins.kind == "halt":
            pass
        a_val, hl_val = new_a, new_hl
        addr = nxt


def parse_table(tbl, slot2, from_bank, from_addr, a_known):
    tb = bank_of(tbl, slot2)
    if tb is None:
        return
    entries = []
    known_code = {phys(*k) for k in labels}
    limit = 0x10000
    for i in range(128):
        ea = tbl + i * 2
        if ea >= limit or ea >= 0xC000 or bank_of(ea, slot2) != tb:
            break
        pe = phys(tb, ea)
        if i > 0 and (pe in owner or pe in known_code):
            break
        v = ROM[pe] | ROM[pe + 1] << 8
        if not plausible(v, slot2):
            break
        if tb >= 2 and not (0x8000 <= v < 0xC000):
            break  # banked tables point inside their own bank
        if tb < 2 and abs(v - tbl) > 0x1000:
            break
        entries.append(v)
        if v > tbl and bank_of(v, slot2) == tb:
            limit = min(limit, v)  # handlers usually follow the table
    tables.append(dict(bank=tb, addr=tbl, slot2=slot2, entries=entries,
                       from_bank=from_bank, from_addr=from_addr))
    label(tbl, slot2, "table")
    for i in range(len(entries) * 2):
        owner.setdefault(phys(tb, tbl) + i, -1)
    for v in entries:
        add(v, slot2, "label")


# ---------------- seeds ----------------
add(0x0000, 2, "vector")
add(0x0038, None, "vector")
add(0x0066, None, "vector")
for r in (0x08, 0x10, 0x18, 0x20, 0x28):
    add(r, None, "func")
states = []
for i in range(STATE_COUNT):
    o = STATE_TABLE + i * 4
    typ, bank, lo, hi = ROM[o:o + 4]
    ent = lo | hi << 8
    states.append(dict(index=i, type=typ, bank=bank, entry=ent))
    add(ent, bank, "state")

for h in HINTS["manual_tables"]:
    parse_table(h["addr"], h["slot2"] if h["addr"] >= 0x8000 else None, h["bank"], None, None)
api = HINTS.get("api_vector")
if api:
    for i in range(api["count"]):
        add(api["start"] + i * 3, None, "func")
for h in HINTS["extra_entries"]:
    add(h["addr"], h.get("slot2"), "func")
passes = 0
while work:
    addr, slot2 = work.pop()
    trace(addr, slot2)
    passes += 1

# ---------------- output ----------------
os.makedirs(OUT, exist_ok=True)


def lname(bank, addr):
    if addr < 0x8000:
        return f"L_{addr:04X}"
    return f"B{bank:02X}_{addr:04X}"


all_labels = dict(labels)
for p, (b, a, ins) in code.items():
    if ins.kind in ("jp", "jr", "call") and ins.target is not None:
        tb = b if ins.target >= 0x8000 and a >= 0x8000 else bank_of(ins.target, None if ins.target >= 0x8000 else 0)
        if tb is not None:
            all_labels.setdefault((tb, ins.target), "label")

code_bytes = {i: 0 for i in range(NBANKS)}
for p, (b, a, ins) in code.items():
    code_bytes[p // 0x4000] += ins.length

tables_by_phys = {}
for t in tables:
    k = phys(t["bank"], t["addr"])
    if not t["entries"]:
        continue
    if k not in tables_by_phys or len(t["entries"]) > len(tables_by_phys[k]["entries"]):
        tables_by_phys[k] = t
tables = list(tables_by_phys.values())


def fmt_bank(bank):
    base = 0x8000 if bank >= 2 else bank * 0x4000
    lines = [f"; bank {bank:02X}  (ROM ${bank * 0x4000:05X}-${bank * 0x4000 + 0x3FFF:05X}), "
             f"mapped at ${base:04X}", f"\torg {base:#06x}".replace("0x", "0") + "h" if False else f"\torg 0{base:04x}h", ""]
    i = 0
    start = bank * 0x4000
    pending_db = []
    pending_at = None

    def flush():
        nonlocal pending_db, pending_at
        while pending_db:
            chunk = pending_db[:16]
            pending_db = pending_db[16:]
            lines.append("\tdb " + ",".join(f"0{x:02x}h" for x in chunk) + f"\t; {pending_at:04X}")
            pending_at += len(chunk)
        pending_at = None

    while i < 0x4000:
        p = start + i
        cpu = base + i
        lab = all_labels.get((bank, cpu))
        if lab:
            flush()
            lines.append(f"{lname(bank, cpu)}:" + ("\t\t; " + lab if lab != "label" else ""))
        if p in tables_by_phys:
            flush()
            t = tables_by_phys[p]
            for v in t["entries"]:
                vb = bank_of(v, t["slot2"])
                local = (vb == bank or (v < 0x8000 and bank < 2 and vb == bank)) and (vb, v) in all_labels
                lines.append(f"\tdw {lname(vb, v)}" if local else f"\tdw 0{v:04x}h\t; -> {lname(vb, v)}")
            i += len(t["entries"]) * 2
            continue
        if p in code:
            flush()
            b, a, ins = code[p]
            # labels landing inside this instruction?
            inner = [k for k in range(1, ins.length) if (bank, cpu + k) in all_labels]
            if ins.undoc or inner:
                why = "undocumented" if ins.undoc else "overlapping label"
                lines.append("\tdb " + ",".join(f"0{x:02x}h" for x in ins.bytes) + f"\t; {cpu:04X} {ins.text.replace('{T}', '?')} ({why})")
                for k in inner:
                    lines.append(f"{lname(bank, cpu + k)}: equ 0{cpu + k:04x}h")
            else:
                txt = ins.text
                v = ins.target if ins.target is not None and ins.kind != "rst" else ins.ref
                if "{T}" in txt:
                    if ins.kind in ("jp", "jr", "call") and ins.target is not None:
                        tgt = ins.target
                        if tgt < 0x4000 and bank == 0 or 0x4000 <= tgt < 0x8000 and bank == 1 or (0x8000 <= tgt < 0xC000 and bank >= 2):
                            txt = txt.replace("{T}", lname(bank, tgt))
                        else:
                            txt = txt.replace("{T}", f"0{tgt:04x}h")
                            tb = bank_of(tgt, None if tgt >= 0x8000 else 0)
                            if tb is not None:
                                txt += f"\t\t; -> {lname(tb, tgt)}"
                    else:
                        txt = txt.replace("{T}", f"0{v:04x}h")
                lines.append(f"\t{txt}" + ("" if "\t;" in txt else f"\t\t; {cpu:04X}"))
            i += ins.length
            continue
        if pending_at is None:
            pending_at = cpu
        pending_db.append(ROM[p])
        i += 1
    flush()
    return "\n".join(lines) + "\n"


for b in range(NBANKS):
    with open(os.path.join(OUT, f"bank{b:02X}.asm"), "w") as f:
        f.write(fmt_bank(b))

funcs = sorted({(b, a) for (b, a), k in labels.items() if k in ("func", "vector", "state")})
jmap = dict(
    rom=dict(size=len(ROM), banks=NBANKS),
    vectors=dict(reset=0x0000, irq=0x0038, nmi=0x0066, irq_line_handler=0x5683, nmi_handler=0x4C7E),
    states=states,
    functions=[dict(bank=b, addr=a) for b, a in funcs],
    labels=[dict(bank=b, addr=a, kind=k) for (b, a), k in sorted(labels.items())],
    jump_tables=tables,
    unresolved_banked_targets=[dict(from_bank=b, from_addr=a, target=t) for b, a, t in sorted(unresolved)],
    bank_switch_sites=[dict(bank=b, addr=a, value=v) for b, a, v in sorted(set(bank_switches), key=lambda x: (x[0], x[1]))],
    code_bytes_per_bank=code_bytes,
    instructions=len(code),
)
with open(os.path.join(OUT, "code_map.json"), "w") as f:
    json.dump(jmap, f, indent=1)

print(f"instructions traced: {len(code)}  functions: {len(funcs)}  tables: {len(tables)}")
print(f"unresolved banked targets: {len(unresolved)}  bank-switch sites: {len(set(bank_switches))}")
for b in range(NBANKS):
    print(f"  bank {b:02X}: {code_bytes[b]:5d} code bytes")
