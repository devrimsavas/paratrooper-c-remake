/*
 * main_test.c - headless, deterministic front end used to verify the
 * translation against the original binary (see tools/verify.py).
 *
 * Time and input are driven by the number of BIOS calls instead of the
 * wall clock, so the translated game and the reference emulator see
 * exactly the same inputs. Every BIOS call prints one trace line with the
 * registers, and every 64th also a checksum of the data segment and
 * video memory.
 *
 * Built two ways:
 *   paratrooper_test       translated game + this file (has main)
 *   libbios_harness.so     -DHARNESS: BIOS only, driven from Python
 */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "machine.h"

static unsigned long n_ints, n_int16, n_ticks;
static unsigned long max_ints = 400000;
static uint16_t key_queue[16];
static int key_head, key_count;
static uint32_t lcg = 12345;

static void key_push(uint16_t k)
{
    if (key_count < 16) {
        key_queue[(key_head + key_count) % 16] = k;
        key_count++;
    }
}

int machine_key_peek(uint16_t *k) { if (!key_count) return 0; *k = key_queue[key_head]; return 1; }
int machine_key_pop(uint16_t *k)
{
    if (!key_count) return 0;
    *k = key_queue[key_head]; key_head = (key_head + 1) % 16; key_count--; return 1;
}
void machine_key_wait(void) { key_push(0x3920); }
uint32_t machine_ticks(void) { return 1000 + (uint32_t)(n_ticks++ / 3); }
void machine_speaker(uint16_t d, int on) { (void)d; (void)on; }
void machine_sync(void) { sync_at = cycles + 1000000000ull; }

void machine_fatal(const char *msg, unsigned value)
{
    printf("FATAL %s 0x%X\n", msg, value);
    fflush(stdout);
    exit(1);
}

static uint32_t crc32_region(uint32_t start, uint32_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= mem[(start + i) & 0xFFFFF];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

/* Scripted player: every 150 keyboard polls press a pseudo-random key
 * (gun keys, or space to start a new game from the title screen). */
static void script_input(void)
{
    static const uint16_t keys[] = { 0x4B00, 0x4800, 0x4D00, 0x4800, 0x4800, 0x3920 };
    n_int16++;
    if (n_int16 == 2000)
        key_push(0x3920);
    else if (n_int16 > 2000 && n_int16 % 150 == 0) {
        lcg = lcg * 1103515245u + 12345u;
        key_push(keys[(lcg >> 16) % 6]);
    }
}

/* DUMP_EVERY=N writes video memory to screen_<n>.bin every N BIOS calls. */
static void dump_screen_if_requested(void)
{
    static long every = -1;
    if (every < 0) {
        const char *e = getenv("DUMP_EVERY");
        every = e ? atol(e) : 0;
    }
    if (every > 0 && n_ints % (unsigned long)every == 0) {
        char name[64];
        snprintf(name, sizeof name, "screen_%07lu.bin", n_ints);
        FILE *f = fopen(name, "wb");
        if (f) { fwrite(&mem[0xB8000], 1, 0x4000, f); fclose(f); }
    }
}

void machine_trace_int(uint8_t n)
{
    if (n == 0x16)
        script_input();
    unsigned flags = CF | ZF << 1 | SF << 2 | OF << 3 | DF << 4;
    printf("%lu %02X AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X F=%02X",
           n_ints, n, AX, BX, CX, DX, SI, DI, BP, SP, DS, ES, flags);
    if (n_ints % 64 == 0)
        printf(" DATA=%08X VIDEO=%08X", crc32_region((uint32_t)(PSP_SEG + 0x11) << 4, 0x10000),
               crc32_region(0xB8000, 0x4000));
    printf("\n");
    dump_screen_if_requested();
    if (++n_ints >= max_ints) {
        fflush(stdout);
        exit(0);
    }
}

void harness_set_max(unsigned long m) { max_ints = m; }

#ifndef HARNESS
int main(int argc, char **argv)
{
    if (argc > 1)
        max_ints = strtoul(argv[1], NULL, 10);
    machine_reset();
    game_run(0x0000);
    return 0;
}
#endif
