/* interp.c - Z80 interpreter.
 *
 * Used (1) as the fallback for code the static recompiler has not covered and
 * (2) as the reference the recompiled C is checked against.  Implements every
 * documented and undocumented opcode, including IX/IY halves, DDCB/FDCB and the
 * undocumented X/Y flag bits.  Validated with zexdoc (see tests/).
 *
 * Memory and I/O go through Z80_RD/Z80_WR/Z80_IN/Z80_OUT so the same core can
 * run inside the CP/M test harness. */
#include "gg.h"

#include "z80ops.h"

uint8_t SZ[256], SZP[256];

void z80_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t f = (uint8_t)((i & (FS | FX | FY)) | (i == 0 ? FZ : 0));
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        SZ[i] = f;
        SZP[i] = (uint8_t)(f | (p ? 0 : FP));
    }
}

/* base T-states for unprefixed opcodes (conditional extras added in code) */
static const uint8_t cyc_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};

static int exec_cb(Z80 *z, int idx)
{
    uint16_t addr = 0;
    if (idx) { addr = (uint16_t)(*rp_hl(z, idx) + (int8_t)fetch(z)); }
    uint8_t op = fetch(z);
    if (!idx) z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
    uint8_t v;
    if (idx) {
        v = rd(z, addr);
        if (x == 1) { bit(z, y, v, (uint8_t)(addr >> 8)); return 20; }
        v = cb_op(z, x, y, v);
        wr(z, addr, v);
        if (r != 6) set_r(z, r, 0, v);  /* undocumented copy to register */
        return 23;
    }
    if (r == 6) {
        v = rd(z, z->hl);
        if (x == 1) { bit(z, y, v, (uint8_t)(z->hl >> 8)); return 12; }
        v = cb_op(z, x, y, v);
        wr(z, z->hl, v);
        return 15;
    }
    v = get_r(z, r, 0);
    if (x == 1) { bit(z, y, v, v); return 8; }
    v = cb_op(z, x, y, v);
    set_r(z, r, 0, v);
    return 8;
}

static int exec_ed(Z80 *z)
{
    uint8_t op = fetch(z);
    z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        switch (zz) {
        case 0: { uint8_t v = op_in_c(z); if (y != 6) set_r(z, y, 0, v); return 12; }
        case 1: Z80_OUT(z->c, y == 6 ? 0 : get_r(z, y, 0)); return 12;
        case 2: z->hl = q ? adc16(z, z->hl, get_rp(z, p, 0)) : sbc16(z, z->hl, get_rp(z, p, 0)); return 15;
        case 3: { uint16_t a = fetch16(z); if (q) set_rp(z, p, 0, rd16(z, a)); else wr16(z, a, get_rp(z, p, 0)); return 20; }
        case 4: op_neg(z); return 8;
        case 5: z->pc = pop(z); z->iff1 = z->iff2; return 14;  /* RETN / RETI */
        case 6: z->im = (uint8_t)((y & 3) == 0 || (y & 3) == 1 ? 0 : (y & 3) - 1); return 8;
        case 7:
            switch (y) {
            case 0: z->i = A; return 9;
            case 1: z->r = A; return 9;
            case 2: op_ld_a_ir(z, z->i); return 9;
            case 3: op_ld_a_ir(z, z->r); return 9;
            case 4: op_rrd(z); return 18;
            case 5: op_rld(z); return 18;
            default: return 8;
            }
        }
    }
    if (x == 2 && zz <= 3 && y >= 4) {
        int dir = (y & 1) ? -1 : 1, rep = y >= 6;
        switch (zz) {
        case 0: blk_ld(z, dir); if (rep && z->bc) { z->pc -= 2; return 21; } return 16;
        case 1: { uint8_t r = blk_cp(z, dir); if (rep && z->bc && r) { z->pc -= 2; return 21; } return 16; }
        case 2: blk_in(z, dir); if (rep && z->b) { z->pc -= 2; return 21; } return 16;
        case 3: blk_out(z, dir); if (rep && z->b) { z->pc -= 2; return 21; } return 16;
        }
    }
    return 8; /* undefined ED: NOP */
}

int z80_step(Z80 *z)
{
    if (z->halted) { z->cycles += 4; return 4; }

    int idx = 0, extra = 0;
    uint8_t op = fetch(z);
    z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    while (op == 0xDD || op == 0xFD) {
        idx = op == 0xDD ? 1 : 2;
        extra += 4;
        op = fetch(z);
        z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    }
    int cyc;
    if (op == 0xCB) { cyc = exec_cb(z, idx) + (idx ? 0 : 0); z->cycles += (unsigned)cyc; return cyc; }
    if (op == 0xED) { cyc = exec_ed(z); z->cycles += (unsigned)cyc; return cyc; }  /* DD ED = ED */

    cyc = cyc_main[op] + extra;
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;

    /* (HL) operand address, with displacement for IX/IY */
#define MEMADDR() (idx ? (cyc += 8, (uint16_t)(*rp_hl(z, idx) + (int8_t)fetch(z))) : z->hl)

    switch (x) {
    case 0:
        switch (zz) {
        case 0:
            switch (y) {
            case 0: break;
            case 1: op_ex_af(z); break;
            case 2: { int8_t d = (int8_t)fetch(z); if (--z->b) { z->pc += d; cyc += 5; } break; }
            case 3: { int8_t d = (int8_t)fetch(z); z->pc += d; break; }
            default: { int8_t d = (int8_t)fetch(z); if (cond(z, y - 4)) { z->pc += d; cyc += 5; } break; }
            }
            break;
        case 1:
            if (!q) set_rp(z, p, idx, fetch16(z));
            else *rp_hl(z, idx) = add16(z, *rp_hl(z, idx), get_rp(z, p, idx));
            break;
        case 2:
            switch (op) {
            case 0x02: wr(z, z->bc, A); break;
            case 0x12: wr(z, z->de, A); break;
            case 0x22: wr16(z, fetch16(z), *rp_hl(z, idx)); break;
            case 0x32: wr(z, fetch16(z), A); break;
            case 0x0A: A = rd(z, z->bc); break;
            case 0x1A: A = rd(z, z->de); break;
            case 0x2A: *rp_hl(z, idx) = rd16(z, fetch16(z)); break;
            case 0x3A: A = rd(z, fetch16(z)); break;
            }
            break;
        case 3: set_rp(z, p, idx, (uint16_t)(get_rp(z, p, idx) + (q ? -1 : 1))); break;
        case 4: case 5:
            if (y == 6) {
                uint16_t a = idx ? (cyc += 8, (uint16_t)(*rp_hl(z, idx) + (int8_t)fetch(z))) : z->hl;
                wr(z, a, zz == 4 ? inc8(z, rd(z, a)) : dec8(z, rd(z, a)));
            } else {
                set_r(z, y, idx, zz == 4 ? inc8(z, get_r(z, y, idx)) : dec8(z, get_r(z, y, idx)));
            }
            break;
        case 6:
            if (y == 6) {
                uint16_t a = idx ? (cyc += 5, (uint16_t)(*rp_hl(z, idx) + (int8_t)fetch(z))) : z->hl;
                wr(z, a, fetch(z));
            } else set_r(z, y, idx, fetch(z));
            break;
        case 7:
            switch (y) {
            case 0: op_rlca(z); break;
            case 1: op_rrca(z); break;
            case 2: op_rla(z); break;
            case 3: op_rra(z); break;
            case 4: daa(z); break;
            case 5: op_cpl(z); break;
            case 6: op_scf(z); break;
            case 7: op_ccf(z); break;
            }
            break;
        }
        break;
    case 1:
        if (op == 0x76) { z->halted = 1; break; }
        if (y == 6) { uint16_t a = MEMADDR(); wr(z, a, get_r(z, zz, 0)); }
        else if (zz == 6) { uint16_t a = MEMADDR(); set_r(z, y, 0, rd(z, a)); }
        else set_r(z, y, idx, get_r(z, zz, idx));
        break;
    case 2:
        if (zz == 6) { uint16_t a = MEMADDR(); alu(z, y, rd(z, a)); }
        else alu(z, y, get_r(z, zz, idx));
        break;
    case 3:
        switch (zz) {
        case 0: if (cond(z, y)) { z->pc = pop(z); cyc += 6; } break;
        case 1:
            if (!q) {
                uint16_t v = pop(z);
                if (p == 3) z->af = v; else set_rp(z, p, idx, v);
            } else switch (p) {
                case 0: z->pc = pop(z); break;
                case 1: op_exx(z); break;
                case 2: z->pc = *rp_hl(z, idx); break;
                case 3: z->sp = *rp_hl(z, idx); break;
            }
            break;
        case 2: { uint16_t a = fetch16(z); if (cond(z, y)) z->pc = a; break; }
        case 3:
            switch (y) {
            case 0: z->pc = fetch16(z); break;
            case 1: break; /* CB handled above */
            case 2: Z80_OUT(fetch(z), A); break;
            case 3: A = Z80_IN(fetch(z)); break;
            case 4: op_ex_sp(z, rp_hl(z, idx)); break;
            case 5: { uint16_t t = z->de; z->de = z->hl; z->hl = t; break; }
            case 6: z->iff1 = z->iff2 = 0; z->ei_delay = 0; break;
            case 7: z->iff1 = z->iff2 = 1; z->ei_delay = 1; break;
            }
            break;
        case 4: { uint16_t a = fetch16(z); if (cond(z, y)) { push(z, z->pc); z->pc = a; cyc += 7; } break; }
        case 5:
            if (!q) push(z, p == 3 ? z->af : get_rp(z, p, idx));
            else if (p == 0) { uint16_t a = fetch16(z); push(z, z->pc); z->pc = a; }
            break;
        case 6: alu(z, y, fetch(z)); break;
        case 7: push(z, z->pc); z->pc = (uint16_t)(y * 8); break;
        }
        break;
    }
#undef MEMADDR
    z->cycles += (unsigned)cyc;
    return cyc;
}

int z80_irq(Z80 *z)
{
    if (!z->iff1 || z->ei_delay) return 0;
    z->iff1 = z->iff2 = 0;
    if (z->halted) { z->halted = 0; }
    push(z, z->pc);
    z->pc = 0x0038;  /* the Game Gear only uses IM 1 */
    z->cycles += 13;
    return 13;
}

void z80_run_until_return(Z80 *z, uint16_t sp_at_entry)
{
    /* The caller has already pushed a return address, so the call is over
     * when SP rises above the entry value. */
    while ((int16_t)(z->sp - sp_at_entry) <= 0)
        z80_step(z);
}
