/* z80ops.h - Z80 instruction semantics shared by the interpreter (interp.c)
 * and the recompiled C (generated/ C files).  Keeping one copy guarantees both
 * execute identically, which the differential test relies on. */
#ifndef Z80OPS_H
#define Z80OPS_H
#include "gg.h"

#ifndef Z80_RD
#define Z80_RD(a)     gg_read8(a)
#define Z80_WR(a, v)  gg_write8((a), (v))
#define Z80_IN(p)     gg_in(p)
#define Z80_OUT(p, v) gg_out((p), (v))
#endif

enum { FC = 0x01, FN = 0x02, FP = 0x04, FX = 0x08, FH = 0x10, FY = 0x20, FZ = 0x40, FS = 0x80 };
#define FV FP

extern uint8_t SZ[256], SZP[256];   /* S, Z, X, Y (and P) for a result byte */

#define A  (z->a)
#define F  (z->f)

static inline uint8_t  rd(Z80 *z, uint16_t a) { (void)z; return Z80_RD(a); }
static inline void     wr(Z80 *z, uint16_t a, uint8_t v) { (void)z; Z80_WR(a, v); }
static inline uint16_t rd16(Z80 *z, uint16_t a) { return (uint16_t)(rd(z, a) | (rd(z, (uint16_t)(a + 1)) << 8)); }
static inline void     wr16(Z80 *z, uint16_t a, uint16_t v) { wr(z, a, (uint8_t)v); wr(z, (uint16_t)(a + 1), (uint8_t)(v >> 8)); }
static inline uint8_t  fetch(Z80 *z) { return rd(z, z->pc++); }
static inline uint16_t fetch16(Z80 *z) { uint16_t v = rd16(z, z->pc); z->pc += 2; return v; }
static inline void     push(Z80 *z, uint16_t v) { z->sp -= 2; wr16(z, z->sp, v); }
static inline uint16_t pop(Z80 *z) { uint16_t v = rd16(z, z->sp); z->sp += 2; return v; }

/* ---------------- ALU ---------------- */
static inline void add8(Z80 *z, uint8_t v, int c)
{
    unsigned r = A + v + c;
    F = (uint8_t)(SZ[r & 0xFF] | ((A ^ v ^ r) & FH) | ((((A ^ ~v) & (A ^ r)) & 0x80) ? FV : 0) | ((r >> 8) & FC));
    A = (uint8_t)r;
}
static inline uint8_t sub8f(Z80 *z, uint8_t v, int c)
{
    unsigned r = (unsigned)A - v - c;
    F = (uint8_t)(SZ[r & 0xFF] | FN | ((A ^ v ^ r) & FH) | ((((A ^ v) & (A ^ r)) & 0x80) ? FV : 0) | ((r >> 8) & FC));
    return (uint8_t)r;
}
static inline void alu(Z80 *z, int op, uint8_t v)
{
    switch (op) {
    case 0: add8(z, v, 0); break;
    case 1: add8(z, v, F & FC); break;
    case 2: A = sub8f(z, v, 0); break;
    case 3: A = sub8f(z, v, F & FC); break;
    case 4: A &= v; F = SZP[A] | FH; break;
    case 5: A ^= v; F = SZP[A]; break;
    case 6: A |= v; F = SZP[A]; break;
    case 7: sub8f(z, v, 0); F = (uint8_t)((F & ~(FX | FY)) | (v & (FX | FY))); break;
    }
}
static inline uint8_t inc8(Z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    F = (uint8_t)((F & FC) | SZ[r] | ((r & 0x0F) == 0 ? FH : 0) | (r == 0x80 ? FV : 0));
    return r;
}
static inline uint8_t dec8(Z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    F = (uint8_t)((F & FC) | FN | SZ[r] | ((r & 0x0F) == 0x0F ? FH : 0) | (r == 0x7F ? FV : 0));
    return r;
}
static inline uint16_t add16(Z80 *z, uint16_t a, uint16_t b)
{
    unsigned r = (unsigned)a + b;
    F = (uint8_t)((F & (FS | FZ | FV)) | (((a ^ b ^ r) >> 8) & FH) | ((r >> 16) & FC) | ((r >> 8) & (FX | FY)));
    return (uint16_t)r;
}
static inline uint16_t adc16(Z80 *z, uint16_t a, uint16_t b)
{
    unsigned r = (unsigned)a + b + (F & FC);
    F = (uint8_t)(((r >> 8) & (FS | FX | FY)) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ b ^ r) >> 8) & FH) |
                  ((((a ^ ~b) & (a ^ r)) & 0x8000) ? FV : 0) | ((r >> 16) & FC));
    return (uint16_t)r;
}
static inline uint16_t sbc16(Z80 *z, uint16_t a, uint16_t b)
{
    unsigned r = (unsigned)a - b - (F & FC);
    F = (uint8_t)(FN | ((r >> 8) & (FS | FX | FY)) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ b ^ r) >> 8) & FH) |
                  ((((a ^ b) & (a ^ r)) & 0x8000) ? FV : 0) | ((r >> 16) & FC));
    return (uint16_t)r;
}
static inline uint8_t rot(Z80 *z, int op, uint8_t v)
{
    uint8_t r, c;
    switch (op) {
    case 0: c = v >> 7; r = (uint8_t)((v << 1) | c); break;               /* RLC */
    case 1: c = v & 1;  r = (uint8_t)((v >> 1) | (c << 7)); break;        /* RRC */
    case 2: c = v >> 7; r = (uint8_t)((v << 1) | (F & FC)); break;        /* RL  */
    case 3: c = v & 1;  r = (uint8_t)((v >> 1) | ((F & FC) << 7)); break; /* RR  */
    case 4: c = v >> 7; r = (uint8_t)(v << 1); break;                     /* SLA */
    case 5: c = v & 1;  r = (uint8_t)((v >> 1) | (v & 0x80)); break;      /* SRA */
    case 6: c = v >> 7; r = (uint8_t)((v << 1) | 1); break;               /* SLL */
    default: c = v & 1; r = (uint8_t)(v >> 1); break;                     /* SRL */
    }
    F = (uint8_t)(SZP[r] | c);
    return r;
}
static inline void bit(Z80 *z, int n, uint8_t v, uint8_t xy)
{
    uint8_t m = v & (uint8_t)(1 << n);
    F = (uint8_t)((F & FC) | FH | (m ? 0 : (FZ | FP)) | (m & FS) | (xy & (FX | FY)));
}
static inline void daa(Z80 *z)
{
    uint8_t a = A, d = 0, c = F & FC, h;
    if ((F & FH) || (a & 0x0F) > 9) d = 0x06;
    if (c || a > 0x99) { d |= 0x60; c = FC; }
    if (F & FN) { h = (F & FH) && (a & 0x0F) < 6 ? FH : 0; A = (uint8_t)(a - d); }
    else        { h = (a & 0x0F) > 9 ? FH : 0;             A = (uint8_t)(a + d); }
    F = (uint8_t)(SZP[A] | h | (F & FN) | c);
}

/* register access with index substitution: idx 0 = HL, 1 = IX, 2 = IY */
static inline uint16_t *rp_hl(Z80 *z, int idx) { return idx == 0 ? &z->hl : idx == 1 ? &z->ix : &z->iy; }

static inline uint8_t get_r(Z80 *z, int r, int idx)
{
    switch (r) {
    case 0: return z->b; case 1: return z->c; case 2: return z->d; case 3: return z->e;
    case 4: return idx == 0 ? z->h : idx == 1 ? z->ixh : z->iyh;
    case 5: return idx == 0 ? z->l : idx == 1 ? z->ixl : z->iyl;
    case 7: return z->a;
    }
    return 0;
}
static inline void set_r(Z80 *z, int r, int idx, uint8_t v)
{
    switch (r) {
    case 0: z->b = v; break; case 1: z->c = v; break; case 2: z->d = v; break; case 3: z->e = v; break;
    case 4: if (idx == 0) z->h = v; else if (idx == 1) z->ixh = v; else z->iyh = v; break;
    case 5: if (idx == 0) z->l = v; else if (idx == 1) z->ixl = v; else z->iyl = v; break;
    case 7: z->a = v; break;
    }
}
static inline int cond(Z80 *z, int cc)
{
    switch (cc) {
    case 0: return !(F & FZ); case 1: return F & FZ;
    case 2: return !(F & FC); case 3: return F & FC;
    case 4: return !(F & FP); case 5: return F & FP;
    case 6: return !(F & FS); default: return F & FS;
    }
}
static inline uint16_t get_rp(Z80 *z, int p, int idx)
{
    switch (p) { case 0: return z->bc; case 1: return z->de; case 2: return *rp_hl(z, idx); default: return z->sp; }
}
static inline void set_rp(Z80 *z, int p, int idx, uint16_t v)
{
    switch (p) { case 0: z->bc = v; break; case 1: z->de = v; break; case 2: *rp_hl(z, idx) = v; break; default: z->sp = v; }
}


/* ---- ED-prefixed helpers (shared so interp and recompiled code agree) ---- */
static inline void blk_ld(Z80 *z, int dir)
{
    uint8_t v = rd(z, z->hl);
    wr(z, z->de, v);
    z->hl += dir; z->de += dir; z->bc--;
    uint8_t n = (uint8_t)(v + A);
    F = (uint8_t)((F & (FS | FZ | FC)) | (z->bc ? FV : 0) | (n & FX) | ((n << 4) & FY));
}
static inline uint8_t blk_cp(Z80 *z, int dir)   /* returns A - (HL) */
{
    uint8_t v = rd(z, z->hl);
    uint8_t r = (uint8_t)(A - v);
    uint8_t h = (uint8_t)((A ^ v ^ r) & FH);
    z->hl += dir; z->bc--;
    uint8_t n = (uint8_t)(r - (h ? 1 : 0));
    F = (uint8_t)((F & FC) | FN | (SZ[r] & (FS | FZ)) | h | (z->bc ? FV : 0) | (n & FX) | ((n << 4) & FY));
    return r;
}
static inline void blk_in(Z80 *z, int dir)
{
    uint8_t v = Z80_IN(z->c);
    wr(z, z->hl, v);
    z->hl += dir; z->b--;
    F = (uint8_t)(SZ[z->b] | ((v & 0x80) ? FN : 0));
}
static inline void blk_out(Z80 *z, int dir)
{
    uint8_t v = rd(z, z->hl);
    z->b--;
    Z80_OUT(z->c, v);
    z->hl += dir;
    F = (uint8_t)(SZ[z->b] | ((v & 0x80) ? FN : 0));
}
static inline void op_rrd(Z80 *z)
{
    uint8_t m = rd(z, z->hl);
    wr(z, z->hl, (uint8_t)((A << 4) | (m >> 4)));
    A = (uint8_t)((A & 0xF0) | (m & 0x0F));
    F = (uint8_t)((F & FC) | SZP[A]);
}
static inline void op_rld(Z80 *z)
{
    uint8_t m = rd(z, z->hl);
    wr(z, z->hl, (uint8_t)((m << 4) | (A & 0x0F)));
    A = (uint8_t)((A & 0xF0) | (m >> 4));
    F = (uint8_t)((F & FC) | SZP[A]);
}
static inline void op_neg(Z80 *z) { uint8_t v = A; A = 0; A = sub8f(z, v, 0); }
static inline uint8_t op_in_c(Z80 *z) { uint8_t v = Z80_IN(z->c); F = (uint8_t)((F & FC) | SZP[v]); return v; }
static inline void op_ld_a_ir(Z80 *z, uint8_t v) { A = v; F = (uint8_t)((F & FC) | SZ[A] | (z->iff2 ? FV : 0)); }
/* unprefixed accumulator ops */
static inline void op_rlca(Z80 *z) { A = (uint8_t)((A << 1) | (A >> 7)); F = (uint8_t)((F & (FS | FZ | FP)) | (A & (FX | FY | FC))); }
static inline void op_rrca(Z80 *z) { uint8_t c = A & 1; A = (uint8_t)((A >> 1) | (c << 7)); F = (uint8_t)((F & (FS | FZ | FP)) | (A & (FX | FY)) | c); }
static inline void op_rla(Z80 *z)  { uint8_t c = A >> 7; A = (uint8_t)((A << 1) | (F & FC)); F = (uint8_t)((F & (FS | FZ | FP)) | (A & (FX | FY)) | c); }
static inline void op_rra(Z80 *z)  { uint8_t c = A & 1; A = (uint8_t)((A >> 1) | ((F & FC) << 7)); F = (uint8_t)((F & (FS | FZ | FP)) | (A & (FX | FY)) | c); }
static inline void op_cpl(Z80 *z)  { A = (uint8_t)~A; F = (uint8_t)((F & (FS | FZ | FP | FC)) | FH | FN | (A & (FX | FY))); }
static inline void op_scf(Z80 *z)  { F = (uint8_t)((F & (FS | FZ | FP)) | FC | (A & (FX | FY))); }
static inline void op_ccf(Z80 *z)  { F = (uint8_t)(((F & (FS | FZ | FP | FC)) | ((F & FC) << 4) | (A & (FX | FY))) ^ FC); }
static inline void op_exx(Z80 *z)
{
    uint16_t t;
    t = z->bc; z->bc = z->bc_; z->bc_ = t;
    t = z->de; z->de = z->de_; z->de_ = t;
    t = z->hl; z->hl = z->hl_; z->hl_ = t;
}
static inline void op_ex_af(Z80 *z) { uint16_t t = z->af; z->af = z->af_; z->af_ = t; }
static inline void op_ex_sp(Z80 *z, uint16_t *r) { uint16_t t = rd16(z, z->sp); wr16(z, z->sp, *r); *r = t; }
/* CB-group op on a value: x = 0 rot, 2 res, 3 set (bit handled by caller) */
static inline uint8_t cb_op(Z80 *z, int x, int y, uint8_t v)
{
    if (x == 0) return rot(z, y, v);
    if (x == 2) return (uint8_t)(v & ~(1 << y));
    return (uint8_t)(v | (1 << y));
}

#define INC_R(z, n) ((z)->r = (uint8_t)(((z)->r & 0x80) | (((z)->r + (n)) & 0x7F)))

#endif
