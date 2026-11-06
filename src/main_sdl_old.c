/*
 * main_sdl.c - SDL2 front end: window, CGA rendering, keyboard, PC speaker
 * sound and speed control (the game runs at original 4.77 MHz speed).
 *
 * Keys:  Left / Keypad 4   turn gun counter-clockwise
 *        Right / Keypad 6  turn gun clockwise
 *        Up / Keypad 8     stop and fire
 *        Space             start game,  I = instructions
 *        Ctrl+S            sound on/off (original feature)
 *        Esc               abort round (sends Ctrl+Break, as on the original)
 *        Alt+Enter         toggle fullscreen
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include "cpu.h"
#include "machine.h"

#define CYCLES_PER_MS    (CPU_HZ / 1000)
#define CYCLES_PER_FRAME (CPU_HZ / 60)
#define AUDIO_RATE       44100

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio_dev;
static uint32_t pixels[320 * 200];
static uint64_t start_ms;
static uint64_t next_frame;
static uint32_t tick_base;

static uint16_t key_queue[16];
static int key_head, key_count;

static volatile int speaker_on;
static volatile double speaker_freq;

static const uint32_t cga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ------------------------------------------------------------ keyboard */

static void key_push(uint16_t key)
{
    if (key_count < 16) {
        key_queue[(key_head + key_count) % 16] = key;
        key_count++;
    }
}

int machine_key_peek(uint16_t *key)
{
    if (!key_count) return 0;
    *key = key_queue[key_head];
    return 1;
}

int machine_key_pop(uint16_t *key)
{
    if (!key_count) return 0;
    *key = key_queue[key_head];
    key_head = (key_head + 1) % 16;
    key_count--;
    return 1;
}

/* PC keyboard scan codes for letters a..z */
static const uint8_t letter_scan[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
};

static void handle_key(const SDL_KeyboardEvent *k)
{
    SDL_Keycode sym = k->keysym.sym;
    SDL_Keymod mod = k->keysym.mod;

    if (sym == SDLK_RETURN && (mod & KMOD_ALT)) {
        Uint32 fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    switch (sym) {
    case SDLK_LEFT:  case SDLK_KP_4: key_push(0x4B00); return;
    case SDLK_RIGHT: case SDLK_KP_6: key_push(0x4D00); return;
    case SDLK_UP:    case SDLK_KP_8: key_push(0x4800); return;
    case SDLK_SPACE:  key_push(0x3920); return;
    case SDLK_RETURN: key_push(0x1C0D); return;
    case SDLK_ESCAPE: key_push(0x0000); return;   /* Ctrl+Break */
    default: break;
    }
    if (sym >= SDLK_a && sym <= SDLK_z) {
        int i = sym - SDLK_a;
        uint8_t scan = letter_scan[i];
        uint8_t ascii;
        if (mod & KMOD_CTRL)
            ascii = (uint8_t)(i + 1);              /* Ctrl+S = 0x13 */
        else if (mod & (KMOD_SHIFT | KMOD_CAPS))
            ascii = (uint8_t)('A' + i);
        else
            ascii = (uint8_t)('a' + i);
        key_push((uint16_t)(scan << 8 | ascii));
    }
}

/* --------------------------------------------------------------- video */

static void present(void)
{
    uint32_t pal[4];
    pal[0] =0x000040; // cga_rgb[video.background];
    for (int i = 1; i < 4; i++) {
        int c = video.palette ? (i == 1 ? 3 : i == 2 ? 5 : 7) : (i * 2);
        pal[i] = cga_rgb[c + (video.intensity ? 8 : 0)];
    }
    if (video.mode == 4 || video.mode == 5) {
        for (int y = 0; y < 200; y++) {
            const uint8_t *line = &mem[0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80];
            uint32_t *dst = &pixels[y * 320];
            for (int x = 0; x < 320; x++)
                dst[x] = pal[(line[x >> 2] >> (6 - 2 * (x & 3))) & 3];
        }
    } else {
        memset(pixels, 0, sizeof pixels);
    }
    SDL_UpdateTexture(texture, NULL, pixels, 320 * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            SDL_Quit();
            exit(0);
        }
        if (e.type == SDL_KEYDOWN)
            handle_key(&e.key);
    }
}

/* -------------------------------------------------------------- timing */

void machine_sync(void)
{
    sync_at = cycles + CYCLES_PER_MS;
    uint64_t emu_ms = cycles / CYCLES_PER_MS;
    uint64_t real_ms = SDL_GetTicks64() - start_ms;
    if (emu_ms > real_ms + 1)
        SDL_Delay((Uint32)(emu_ms - real_ms));
    else if (real_ms > emu_ms + 250)
        start_ms = SDL_GetTicks64() - emu_ms;   /* host too slow: don't rush to catch up */
    if (cycles >= next_frame) {
        next_frame = cycles + CYCLES_PER_FRAME;
        pump_events();
        present();
    }
}

uint32_t machine_ticks(void)
{
    return (tick_base + (uint32_t)(cycles / CYCLES_PER_TICK)) % 0x1800B0;
}

void machine_key_wait(void)
{
    /* Let emulated time pass while BIOS waits for a key. */
    cycles += CYCLES_PER_MS;
    machine_sync();
}

void machine_trace_int(uint8_t n) { (void)n; }

void machine_fatal(const char *msg, unsigned value)
{
    char text[160];
    snprintf(text, sizeof text, "%s (0x%X)", msg, value);
    fprintf(stderr, "fatal: %s\n", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Paratrooper", text, window);
    SDL_Quit();
    exit(1);
}

/* --------------------------------------------------------------- sound */

void machine_speaker(uint16_t divisor, int on)
{
    double f = 1193182.0 / (divisor ? divisor : 65536);
    SDL_LockAudioDevice(audio_dev);
    speaker_freq = f;
    speaker_on = on && f >= 20.0 && f <= 15000.0;   /* ultrasonic = silence */
    SDL_UnlockAudioDevice(audio_dev);
}

static void audio_callback(void *user, Uint8 *stream, int len)
{
    static double phase;
    (void)user;
    int16_t *out = (int16_t *)stream;
    int n = len / 2;
    double step = speaker_freq / AUDIO_RATE;
    for (int i = 0; i < n; i++) {
        if (speaker_on) {
            out[i] = phase < 0.5 ? 2500 : -2500;
            phase += step;
            if (phase >= 1.0) phase -= 1.0;
        } else {
            out[i] = 0;
        }
    }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    window = SDL_CreateWindow("Paratrooper (1982)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              960, 720, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_RenderSetLogicalSize(renderer, 640, 480);   /* 4:3 like a CRT monitor */
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);

    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audio_callback;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev)
        SDL_PauseAudioDevice(audio_dev, 0);

    /* BIOS clock starts at the current time of day (the game seeds its
     * random numbers from it). */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    tick_base = (uint32_t)((lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec) * 18.2065);

    machine_reset();
    start_ms = SDL_GetTicks64();
    next_frame = 0;
    game_run(0x0000);   /* never returns */
    SDL_Quit();
    return 0;
}
