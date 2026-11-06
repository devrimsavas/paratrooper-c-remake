/*
 * machine.h - interface between the BIOS/port emulation (bios.c) and the
 * front end (main_sdl.c for the real game, main_test.c for verification).
 */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdint.h>

#define CPU_HZ          4772727u            /* original IBM PC clock */
#define CYCLES_PER_TICK 262144u             /* one 18.2 Hz BIOS timer tick */
#define PSP_SEG         0x0800              /* where "DOS" loads the program */

typedef struct {
    uint8_t mode;        /* BIOS video mode (4 = CGA 320x200x4) */
    uint8_t background;  /* colour 0 in mode 4 (0..15) */
    uint8_t palette;     /* 0 = green/red/brown, 1 = cyan/magenta/white */
    uint8_t intensity;   /* bright palette */
    uint8_t row, col;    /* text cursor */
} VideoState;

extern VideoState video;
extern const uint8_t image[];
extern const unsigned image_size;

/* Provided by the front end. */
uint32_t machine_ticks(void);               /* BIOS ticks since midnight */
int  machine_key_peek(uint16_t *key);       /* scan<<8 | ascii */
int  machine_key_pop(uint16_t *key);
void machine_key_wait(void);                /* block until a key is queued */
void machine_speaker(uint16_t divisor, int on);
void machine_trace_int(uint8_t n);          /* debugging hook, may be empty */

/* Provided by bios.c / game.c */
void machine_reset(void);
void game_run(uint16_t entry);

#endif
