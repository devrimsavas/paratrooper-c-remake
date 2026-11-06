/*
 * bios.c - the small part of the IBM PC BIOS and hardware that Paratrooper uses:
 *   INT 10h  video: set mode, palette, cursor position, teletype output
 *   INT 16h  keyboard: read key, check for key
 *   INT 1Ah  timer: read tick count
 *   ports    PIT channel 2 (0x42/0x43) + port 0x61 for the PC speaker,
 *            0x201 game port (reported as "no joystick buttons pressed")
 */
#include <string.h>
#include "cpu.h"
#include "machine.h"

extern const uint8_t font8x8[128][8];

Cpu r;
uint8_t mem[0x100000];
uint64_t cycles;
uint64_t sync_at;
VideoState video;

#define CGA_BASE 0xB8000u

/* ---------------------------------------------------------------- video */

static void cga_put_pixel(int x, int y, int color, int xor_mode)
{
    uint32_t off = CGA_BASE + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 2));
    int shift = 6 - 2 * (x & 3);
    if (xor_mode)
        mem[off] ^= (uint8_t)((color & 3) << shift);
    else
        mem[off] = (uint8_t)((mem[off] & ~(3 << shift)) | ((color & 3) << shift));
}

static void cga_scroll_up(void)
{
    /* One text row = 8 pixel lines = 4 lines (320 bytes) in each bank. */
    for (int bank = 0; bank < 2; bank++) {
        uint8_t *p = &mem[CGA_BASE + bank * 0x2000];
        memmove(p, p + 320, 8000 - 320);
        memset(p + 8000 - 320, 0, 320);
    }
}

static void tty_newline(void)
{
    if (++video.row >= 25) {
        video.row = 24;
        if (video.mode == 4 || video.mode == 5)
            cga_scroll_up();
    }
}

static void tty_output(uint8_t ch, uint8_t color)
{
    switch (ch) {
    case 7:  return;                                  /* bell */
    case 8:  if (video.col) video.col--; return;      /* backspace */
    case 10: tty_newline(); return;
    case 13: video.col = 0; return;
    }
    if (video.mode == 4 || video.mode == 5) {
        const uint8_t *glyph = font8x8[ch & 0x7F];
        int x0 = video.col * 8, y0 = video.row * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int on = (glyph[y] >> (7 - x)) & 1;
                if (color & 0x80) {
                    if (on) cga_put_pixel(x0 + x, y0 + y, color, 1);
                } else {
                    cga_put_pixel(x0 + x, y0 + y, on ? color : 0, 0);
                }
            }
    } else {
        uint32_t off = CGA_BASE + (uint32_t)(video.row * 160 + video.col * 2);
        mem[off] = ch;
    }
    if (++video.col >= (video.mode == 4 || video.mode == 5 ? 40 : 80)) {
        video.col = 0;
        tty_newline();
    }
}

static void int10(void)
{
    switch (AH) {
    case 0x00:                           /* set video mode */
        video.mode = AL & 0x7F;
        video.row = video.col = 0;
        if (!(AL & 0x80))
            memset(&mem[CGA_BASE], 0, 0x4000);
        break;
    case 0x02:                           /* set cursor position */
        video.row = DH;
        video.col = DL;
        break;
    case 0x0B:                           /* set colour palette */
        if (BH == 0) {
            video.background = BL & 0x0F;
            video.intensity = (BL >> 4) & 1;
        } else {
            video.palette = BL & 1;
        }
        break;
    case 0x0C:                           /* write pixel: AL colour (bit 7 = XOR), CX x, DX y */
        if ((video.mode == 4 || video.mode == 5) && CX < 320 && DX < 200)
            cga_put_pixel(CX, DX, AL & 3, AL & 0x80);
        break;
    case 0x0D:                           /* read pixel */
        if ((video.mode == 4 || video.mode == 5) && CX < 320 && DX < 200) {
            uint32_t off = CGA_BASE + (uint32_t)((DX & 1) * 0x2000 + (DX >> 1) * 80 + (CX >> 2));
            AL = (mem[off] >> (6 - 2 * (CX & 3))) & 3;
        } else {
            AL = 0;
        }
        break;
    case 0x0E:                           /* teletype output */
        tty_output(AL, BL);
        break;
    default:
        machine_fatal("unhandled INT 10h function", AH);
    }
}

/* ------------------------------------------------------------- keyboard */

static void int16(void)
{
    uint16_t key;
    switch (AH) {
    case 0x00:                           /* wait for and read a key */
        while (!machine_key_pop(&key))
            machine_key_wait();
        AX = key;
        break;
    case 0x01:                           /* is a key available? (ZF = 1: no) */
        if (machine_key_peek(&key)) {
            AX = key;
            ZF = 0;
        } else {
            ZF = 1;
        }
        break;
    default:
        machine_fatal("unhandled INT 16h function", AH);
    }
}

/* ---------------------------------------------------------------- timer */

static void int1a(void)
{
    if (AH != 0)
        machine_fatal("unhandled INT 1Ah function", AH);
    uint32_t t = machine_ticks();
    CX = (uint16_t)(t >> 16);
    DX = (uint16_t)t;
    AL = 0;                              /* midnight flag */
}

void bios_int(uint8_t n)
{
    machine_trace_int(n);
    switch (n) {
    case 0x10: int10(); break;
    case 0x16: int16(); break;
    case 0x1A: int1a(); break;
    default:   machine_fatal("unhandled interrupt", n);
    }
}

/* ---------------------------------------------------------------- ports */

static uint16_t pit2_divisor;
static int pit2_high_next;               /* PIT lo/hi byte flip-flop */
static uint8_t port61;

uint8_t port_in(uint16_t port)
{
    switch (port) {
    case 0x61:  return port61;
    case 0x201: return 0xFF;             /* no buttons pressed, axes idle */
    default:    return 0xFF;
    }
}

void port_out(uint16_t port, uint8_t value)
{
    switch (port) {
    case 0x42:
        if (pit2_high_next)
            pit2_divisor = (uint16_t)((pit2_divisor & 0x00FF) | (value << 8));
        else
            pit2_divisor = (uint16_t)((pit2_divisor & 0xFF00) | value);
        pit2_high_next = !pit2_high_next;
        machine_speaker(pit2_divisor, (port61 & 3) == 3);
        break;
    case 0x43:
        pit2_high_next = 0;
        break;
    case 0x61:
        port61 = value;
        machine_speaker(pit2_divisor, (port61 & 3) == 3);
        break;
    case 0x201:                          /* starts joystick one-shots: ignored */
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ start-up */

void machine_reset(void)
{
    memset(mem, 0, sizeof mem);
    memset(&r, 0, sizeof r);
    memset(&video, 0, sizeof video);
    video.mode = 3;
    pit2_divisor = 0;
    pit2_high_next = 0;
    port61 = 0;
    cycles = 0;
    sync_at = 0;

    /* BIOS data area: equipment word says "80x25 colour" (bits 4-5 = 10b),
     * so the game skips its "Do you have a CGA?" question. */
    mem[0x410] = 0x21;
    mem[0x411] = 0x00;

    /* Load the program like DOS loads a .COM file. */
    memcpy(&mem[PSP_SEG * 16 + 0x100], image, image_size);
    mem[PSP_SEG * 16 + 0] = 0xCD;        /* INT 20h at PSP:0000 */
    mem[PSP_SEG * 16 + 1] = 0x20;

    /* State after the 16-byte start stub: CS = PSP+0x2C4, AX = DS = PSP. */
    CS = PSP_SEG + 0x2C4;
    DS = ES = SS = PSP_SEG;
    AX = PSP_SEG;
    SP = 0xFFFE;
    ww(SS, 0xFFFE, 0);                   /* DOS return address word */
    ww(SS, 0xFFFC, PSP_SEG + 0x2C4);     /* left on the stack by the stub's RETF */
    ww(SS, 0xFFFA, 0);
}
