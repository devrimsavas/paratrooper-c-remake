/*
 * cpu.h - 8086 machine state and instruction helpers used by the
 * mechanically translated game code (game.c).
 *
 * Registers, flags and the 1 MB real-mode address space are emulated
 * exactly; everything else (video, keyboard, timer, speaker) is handled
 * by the BIOS/port layer in bios.c.
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

typedef union {
    uint16_t x;
    struct { uint8_t l, h; } b; /* little-endian host assumed */
} Reg16;

typedef struct {
    Reg16 a, b, c, d;
    uint16_t si, di, bp, sp;
    uint16_t cs, ds, es, ss;
    uint16_t ip;              /* only used for indirect jumps / returns */
    uint8_t cf, zf, sf, of, df;
} Cpu;

extern Cpu r;
extern uint8_t mem[0x100000];
extern uint64_t cycles;      /* emulated 8088 clock cycles */
extern uint64_t sync_at;     /* call machine_sync() when cycles reach this */

void machine_sync(void);
void machine_fatal(const char *msg, unsigned value);
void bios_int(uint8_t n);
uint8_t port_in(uint16_t port);
void port_out(uint16_t port, uint8_t value);

#define AX r.a.x
#define BX r.b.x
#define CX r.c.x
#define DX r.d.x
#define AL r.a.b.l
#define AH r.a.b.h
#define BL r.b.b.l
#define BH r.b.b.h
#define CL r.c.b.l
#define CH r.c.b.h
#define DL r.d.b.l
#define DH r.d.b.h
#define SI r.si
#define DI r.di
#define BP r.bp
#define SP r.sp
#define CS r.cs
#define DS r.ds
#define ES r.es
#define SS r.ss
#define CF r.cf
#define ZF r.zf
#define SF r.sf
#define OF r.of
#define DF r.df

#define CYC(n) do { cycles += (n); if (cycles >= sync_at) machine_sync(); } while (0)

/* ---- memory ---- */
static inline uint32_t lin(uint16_t seg, uint16_t off) {
    return (((uint32_t)seg << 4) + off) & 0xFFFFF;
}
static inline uint8_t rb(uint16_t seg, uint16_t off) { return mem[lin(seg, off)]; }
static inline void wb(uint16_t seg, uint16_t off, uint8_t v) { mem[lin(seg, off)] = v; }
static inline uint16_t rw(uint16_t seg, uint16_t off) {
    return (uint16_t)(mem[lin(seg, off)] | (mem[lin(seg, (uint16_t)(off + 1))] << 8));
}
static inline void ww(uint16_t seg, uint16_t off, uint16_t v) {
    mem[lin(seg, off)] = (uint8_t)v;
    mem[lin(seg, (uint16_t)(off + 1))] = (uint8_t)(v >> 8);
}
static inline void push16(uint16_t v) { SP -= 2; ww(SS, SP, v); }
static inline uint16_t pop16(void) { uint16_t v = rw(SS, SP); SP += 2; return v; }

/* ---- flag-setting arithmetic (8 and 16 bit) ---- */
#define SZ8(v)  do { ZF = (uint8_t)(v) == 0; SF = ((v) >> 7) & 1; } while (0)
#define SZ16(v) do { ZF = (uint16_t)(v) == 0; SF = ((v) >> 15) & 1; } while (0)

static inline uint8_t add8(uint8_t a, uint8_t b) {
    unsigned v = a + b; uint8_t res = (uint8_t)v;
    CF = v > 0xFF; OF = ((a ^ res) & (b ^ res) & 0x80) != 0; SZ8(res); return res;
}
static inline uint16_t add16(uint16_t a, uint16_t b) {
    uint32_t v = (uint32_t)a + b; uint16_t res = (uint16_t)v;
    CF = v > 0xFFFF; OF = ((a ^ res) & (b ^ res) & 0x8000) != 0; SZ16(res); return res;
}
static inline uint8_t sub8(uint8_t a, uint8_t b) {
    uint8_t res = (uint8_t)(a - b);
    CF = a < b; OF = ((a ^ b) & (a ^ res) & 0x80) != 0; SZ8(res); return res;
}
static inline uint16_t sub16(uint16_t a, uint16_t b) {
    uint16_t res = (uint16_t)(a - b);
    CF = a < b; OF = ((a ^ b) & (a ^ res) & 0x8000) != 0; SZ16(res); return res;
}
static inline uint8_t inc8(uint8_t a)   { uint8_t c = CF; uint8_t v = add8(a, 1); CF = c; return v; }
static inline uint16_t inc16(uint16_t a){ uint8_t c = CF; uint16_t v = add16(a, 1); CF = c; return v; }
static inline uint8_t dec8(uint8_t a)   { uint8_t c = CF; uint8_t v = sub8(a, 1); CF = c; return v; }
static inline uint16_t dec16(uint16_t a){ uint8_t c = CF; uint16_t v = sub16(a, 1); CF = c; return v; }
static inline uint8_t neg8(uint8_t a)   { return sub8(0, a); }
static inline uint16_t neg16(uint16_t a){ return sub16(0, a); }

static inline uint8_t logic8(uint8_t v)   { CF = 0; OF = 0; SZ8(v); return v; }
static inline uint16_t logic16(uint16_t v){ CF = 0; OF = 0; SZ16(v); return v; }

/* Shifts/rotates. The 8086 does not mask the count; a count of 0 leaves flags alone. */
static inline uint8_t shl8(uint8_t a, uint8_t n) {
    if (!n) return a;
    unsigned v = (unsigned)a << (n > 9 ? 9 : n);
    uint8_t res = (uint8_t)v; CF = (v >> 8) & 1; if (n > 8) CF = 0;
    OF = ((res >> 7) & 1) ^ CF; SZ8(res); return res;
}
static inline uint16_t shl16(uint16_t a, uint8_t n) {
    if (!n) return a;
    uint32_t v = (uint32_t)a << (n > 17 ? 17 : n);
    uint16_t res = (uint16_t)v; CF = (v >> 16) & 1; if (n > 16) CF = 0;
    OF = ((res >> 15) & 1) ^ CF; SZ16(res); return res;
}
static inline uint8_t shr8(uint8_t a, uint8_t n) {
    if (!n) return a;
    CF = n <= 8 ? (a >> (n - 1)) & 1 : 0; OF = (a >> 7) & 1;
    uint8_t res = n < 8 ? (uint8_t)(a >> n) : 0; SZ8(res); return res;
}
static inline uint16_t shr16(uint16_t a, uint8_t n) {
    if (!n) return a;
    CF = n <= 16 ? (a >> (n - 1)) & 1 : 0; OF = (a >> 15) & 1;
    uint16_t res = n < 16 ? (uint16_t)(a >> n) : 0; SZ16(res); return res;
}
static inline uint8_t sar8(uint8_t a, uint8_t n) {
    if (!n) return a;
    int s = (int8_t)a; if (n > 8) n = 8;
    CF = (s >> (n - 1)) & 1; OF = 0;
    uint8_t res = (uint8_t)(s >> n); SZ8(res); return res;
}
static inline uint16_t sar16(uint16_t a, uint8_t n) {
    if (!n) return a;
    int s = (int16_t)a; if (n > 16) n = 16;
    CF = (s >> (n - 1)) & 1; OF = 0;
    uint16_t res = (uint16_t)(s >> n); SZ16(res); return res;
}
static inline uint8_t rol8(uint8_t a, uint8_t n) {
    if (!n) return a;
    uint8_t k = n & 7; uint8_t res = (uint8_t)((a << k) | (a >> ((8 - k) & 7)));
    CF = res & 1; OF = ((res >> 7) & 1) ^ CF; return res;
}
static inline uint16_t rol16(uint16_t a, uint8_t n) {
    if (!n) return a;
    uint8_t k = n & 15; uint16_t res = (uint16_t)((a << k) | (a >> ((16 - k) & 15)));
    CF = res & 1; OF = ((res >> 15) & 1) ^ CF; return res;
}
static inline uint8_t ror8(uint8_t a, uint8_t n) {
    if (!n) return a;
    uint8_t k = n & 7; uint8_t res = (uint8_t)((a >> k) | (a << ((8 - k) & 7)));
    CF = (res >> 7) & 1; OF = CF ^ ((res >> 6) & 1); return res;
}
static inline uint16_t ror16(uint16_t a, uint8_t n) {
    if (!n) return a;
    uint8_t k = n & 15; uint16_t res = (uint16_t)((a >> k) | (a << ((16 - k) & 15)));
    CF = (res >> 15) & 1; OF = CF ^ ((res >> 14) & 1); return res;
}

/* ---- multiply / divide ---- */
static inline void mul8(uint8_t v)  { AX = (uint16_t)(AL * v); CF = OF = AH != 0; }
static inline void mul16(uint16_t v){ uint32_t p = (uint32_t)AX * v; AX = (uint16_t)p; DX = (uint16_t)(p >> 16); CF = OF = DX != 0; }
static inline void idiv8(uint8_t v) {
    int16_t n = (int16_t)AX; int8_t d = (int8_t)v;
    if (d == 0) machine_fatal("divide by zero (idiv8)", 0);
    int q = n / d, m = n % d;
    if (q > 127 || q < -128) machine_fatal("divide overflow (idiv8)", (unsigned)q);
    AL = (uint8_t)q; AH = (uint8_t)m;
}
static inline void idiv16(uint16_t v) {
    int32_t n = (int32_t)(((uint32_t)DX << 16) | AX); int16_t d = (int16_t)v;
    if (d == 0) machine_fatal("divide by zero (idiv16)", 0);
    int32_t q = n / d, m = n % d;
    if (q > 32767 || q < -32768) machine_fatal("divide overflow (idiv16)", (unsigned)q);
    AX = (uint16_t)q; DX = (uint16_t)m;
}

#endif
