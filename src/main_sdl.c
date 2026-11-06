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

/* ---- modern backdrop, drawn behind the game screen ---- */

#define BG_W 640
#define BG_H 400
#define BG_GROUND 380            /* the game's ground line is at y = 190 (x2) */

static SDL_Texture *background;

static uint32_t bg_rand(uint32_t *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return *seed >> 8;
}

static uint32_t rgb_lerp(uint32_t a, uint32_t b, int t, int range)
{
    int r = ((a >> 16 & 255) * (range - t) + (b >> 16 & 255) * t) / range;
    int g = ((a >> 8 & 255) * (range - t) + (b >> 8 & 255) * t) / range;
    int bl = ((a & 255) * (range - t) + (b & 255) * t) / range;
    return 0xFF000000u | (uint32_t)(r << 16 | g << 8 | bl);
}

/* Mountain ridge as a smoothed random walk; returns its height per column. */
static void make_ridge(int *height, uint32_t seed, int base, int rough)
{
    int h = base;
    for (int x = 0; x < BG_W; x++) {
        h += (int)(bg_rand(&seed) % (2 * rough + 1)) - rough;
        if (h < base / 2) h = base / 2;
        if (h > base * 2) h = base * 2;
        height[x] = h;
    }
    for (int pass = 0; pass < 6; pass++)
        for (int x = 1; x < BG_W - 1; x++)
            height[x] = (height[x - 1] + 2 * height[x] + height[x + 1]) / 4;
}

static void make_background(void)
{
    static uint32_t bg[BG_W * BG_H];
    static int ridge[BG_W];
    uint32_t seed = 1982;

    /* sky: deep navy at the top fading to dusk purple at the horizon */
    for (int y = 0; y < BG_H; y++) {
        uint32_t c = y < BG_GROUND ? rgb_lerp(0x070B1E, 0x3A2A55, y, BG_GROUND) : 0xFF05060C;
        for (int x = 0; x < BG_W; x++)
            bg[y * BG_W + x] = c;
    }
    /* stars, fewer and dimmer towards the horizon */
    for (int i = 0; i < 260; i++) {
        int x = (int)(bg_rand(&seed) % BG_W);
        int y = (int)(bg_rand(&seed) % (BG_GROUND * 3 / 4));
        int v = 255 - y * 160 / BG_GROUND - (int)(bg_rand(&seed) % 60);
        bg[y * BG_W + x] = 0xFF000000u | (uint32_t)(v << 16 | v << 8 | (v < 200 ? v + 55 : 255));
    }
    /* moon */
    for (int y = -14; y <= 14; y++)
        for (int x = -14; x <= 14; x++)
            if (x * x + y * y <= 196)
                bg[(140 + y) * BG_W + 590 + x] = (x + 5) * (x + 5) + (y - 4) * (y - 4) <= 150
                    ? 0xFFE8E4D0 : 0xFFB8B4A0;
    /* two mountain ranges: far (lighter) and near (darker) */
    make_ridge(ridge, 7, 90, 6);
    for (int x = 0; x < BG_W; x++)
        for (int y = BG_GROUND - ridge[x]; y < BG_GROUND; y++)
            bg[y * BG_W + x] = 0xFF2A2446;
    make_ridge(ridge, 31, 45, 4);
    for (int x = 0; x < BG_W; x++)
        for (int y = BG_GROUND - ridge[x]; y < BG_GROUND; y++)
            bg[y * BG_W + x] = 0xFF16142A;

    background = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, BG_W, BG_H);
    SDL_UpdateTexture(background, NULL, bg, BG_W * sizeof(uint32_t));
}

/* ---- sprite replacement ----
 *
 * The game draws its objects by copying fixed bitmaps from its data segment
 * into CGA memory at byte-aligned positions (1 byte = 4 pixels). Every frame
 * we search the CGA screen for these bitmaps; where one is found, its
 * original pixels are hidden and a high-resolution (4x) picture is drawn on
 * top instead. The game code itself is not changed at all.
 *
 *   helicopter  DS:0x08BE / 0x09AE   6 bytes x 10 rows, 4 rotor frames each
 *   paratrooper DS:0x0C7E            3 bytes x 22 rows (parachute + soldier)
 *   soldier     DS:0x0CA8            3 bytes x 8 rows  (falling or landed)
 *   walking     DS:0x213B / 0x214B   1 byte  x 8 rows  (step frames)
 *   gun         DS:0x0000 -> 21 ptrs 8 bytes x 13 rows, fixed place on screen
 *   bunker      white block          x 144..175, y 166..189
 */

#define GAME_DS ((uint32_t)(PSP_SEG + 0x11) << 4)

/* drawing canvas for building the 4x textures */
static uint32_t *cv;
static int cv_w, cv_h;

static void cv_begin(int w, int h)
{
    cv_w = w; cv_h = h;
    cv = calloc((size_t)w * h, sizeof(uint32_t));
}

static void cv_mirror(void)
{
    for (int y = 0; y < cv_h; y++)
        for (int x = 0; x < cv_w / 2; x++) {
            uint32_t t = cv[y * cv_w + x];
            cv[y * cv_w + x] = cv[y * cv_w + cv_w - 1 - x];
            cv[y * cv_w + cv_w - 1 - x] = t;
        }
}

static SDL_Texture *cv_texture(void)
{
    SDL_Texture *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, cv_w, cv_h);
    SDL_UpdateTexture(t, NULL, cv, cv_w * (int)sizeof(uint32_t));
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}

static void cv_end(void) { free(cv); cv = NULL; }

static void hb_put(int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= cv_w || y >= cv_h) return;
    uint32_t *d = &cv[y * cv_w + x];
    unsigned a = argb >> 24, da = *d >> 24;
    if (a == 255 || da == 0) { if (a) *d = argb; return; }
    unsigned r = ((argb >> 16 & 255) * a + (*d >> 16 & 255) * (255 - a)) / 255;
    unsigned g = ((argb >> 8 & 255) * a + (*d >> 8 & 255) * (255 - a)) / 255;
    unsigned b = ((argb & 255) * a + (*d & 255) * (255 - a)) / 255;
    unsigned na = a + da * (255 - a) / 255;
    *d = na << 24 | r << 16 | g << 8 | b;
}

static void hb_rect(int x0, int y0, int x1, int y1, uint32_t c)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            hb_put(x, y, c);
}

/* Filled ellipse with top-to-bottom shading between two colours. */
static void hb_ellipse(int cx, int cy, int rx, int ry, uint32_t top, uint32_t bottom)
{
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++)
            if (x * x * ry * ry + y * y * rx * rx <= rx * rx * ry * ry)
                hb_put(cx + x, cy + y, rgb_lerp(top, bottom, y + ry, 2 * ry + 1));
}

/* Thick line: a square brush of size `thick` stepped along the line. */
static void hb_line(int x0, int y0, int x1, int y1, int thick, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
        hb_rect(x - thick / 2, y - thick / 2, x - thick / 2 + thick - 1, y - thick / 2 + thick - 1, c);
    }
}

/* ---------- pictures ---------- */

static SDL_Texture *heli_tex[2][4];  /* [0 = nose right, 1 = nose left][rotor frame] */
static SDL_Texture *trooper_tex, *soldier_tex[3], *bunker_tex;
static SDL_Texture *gun_tex[21];

/* Helicopter, 96x40, nose to the right, rotor frame 0..3. */
static void draw_helicopter(int frame)
{
    static const int blade_half[4] = { 44, 30, 6, 30 };   /* main rotor seen turning */
    static const int tail_blade[4] = { 7, 4, 1, 4 };

    for (int x = 14; x <= 94; x++) hb_put(x, 7, 0x60C8D0E0);     /* rotor blur */
    hb_rect(58 - blade_half[frame], 6, 58 + blade_half[frame], 7, 0xFF9AA0A8);
    hb_rect(56, 8, 60, 12, 0xFF3A3F36);                   /* mast */

    for (int x = 6; x <= 46; x++) {                       /* tapered tail boom */
        int half = 1 + (x - 6) / 14;
        hb_rect(x, 19 - half, x, 19 + half, rgb_lerp(0x55663F, 0x2E3824, x - 6, 41));
    }
    for (int y = 9; y <= 21; y++)                         /* fin */
        hb_rect(5, y, 5 + (21 - y) / 2 + 3, y, 0xFF3F4C30);
    for (int y = -6; y <= 6; y++)                         /* tail rotor blur */
        for (int x = -6; x <= 6; x++)
            if (x * x + y * y <= 36) hb_put(8 + x, 13 + y, 0x30C8D0E0);
    hb_rect(8 - tail_blade[frame], 12, 8 + tail_blade[frame], 13, 0xFF9AA0A8);
    hb_rect(2, 18, 3, 19, 0xFFFF3030);                    /* red light */

    hb_ellipse(60, 22, 22, 10, 0x7A8E5E, 0x2A3320);       /* body */
    hb_ellipse(72, 20, 10, 7, 0x9FE6F5, 0x2D6E86);        /* cockpit glass */
    hb_rect(66, 15, 69, 16, 0xFFE8FBFF);
    hb_rect(44, 21, 74, 22, 0xFF26301C);                  /* door line */

    hb_rect(50, 31, 51, 35, 0xFF5A5E62);                  /* skids */
    hb_rect(72, 31, 73, 35, 0xFF5A5E62);
    hb_rect(42, 35, 86, 36, 0xFF4A4E52);
    hb_rect(86, 33, 88, 34, 0xFF4A4E52);
}

/* Soldier, 48 wide x 32 high, top at oy (the original is 12x8 pixels).
 * pose 0 = standing, 1 and 2 = walking steps. */
static void draw_soldier(int oy, int pose)
{
    static const int leg_x[3][2] = { { 17, 25 }, { 20, 27 }, { 15, 22 } };
    for (int i = 0; i < 2; i++) {
        int lx = leg_x[pose][i];
        hb_rect(lx, oy + 19, lx + 3, oy + 28, 0xFF3B4A2C);        /* leg */
        hb_rect(lx - 2 + (i ? 2 : 0), oy + 28, lx + 3 + (i ? 2 : 0), oy + 31, 0xFF1C1C1C);   /* boot */
    }
    hb_rect(13, oy + 9, 15, oy + 18, 0xFF4E5E38);         /* arms */
    hb_rect(30, oy + 9, 32, oy + 18, 0xFF4E5E38);
    for (int y = oy + 8; y <= oy + 19; y++)               /* torso, lit from the left */
        for (int x = 16; x <= 29; x++)
            hb_put(x, y, rgb_lerp(0x667A48, 0x34402A, x - 16, 14));
    hb_rect(16, oy + 17, 29, oy + 18, 0xFF2A2418);        /* belt */
    hb_ellipse(22, oy + 6, 3, 3, 0xE0B090, 0xB08060);     /* face */
    hb_ellipse(22, oy + 3, 6, 3, 0x6E7E50, 0x3A4428);     /* helmet */
    hb_rect(15, oy + 5, 29, oy + 5, 0xFF2E3820);          /* helmet rim */
}

/* Paratrooper, 48x88: canopy (original rows 0..5), lines (6..13), soldier (14..21). */
static void draw_trooper(void)
{
    static const int lines_x[5] = { 1, 12, 24, 35, 46 };
    for (int i = 0; i < 5; i++)                           /* suspension lines */
        hb_line(lines_x[i], 22, 22 + (lines_x[i] - 24) / 6, 62, 1, 0xC0B8B8A8);
    for (int y = -22; y <= 0; y++)                        /* canopy: half dome */
        for (int x = -23; x <= 23; x++)
            if (x * x * 22 * 22 + y * y * 23 * 23 <= 23 * 23 * 22 * 22) {
                int panel = ((x + 23) / 8) & 1;           /* alternating panels */
                uint32_t c = rgb_lerp(panel ? 0xD8D0A8 : 0xBCB088, panel ? 0x8A8060 : 0x706848, y + 22, 23);
                hb_put(24 + x, 24 + y, c);
            }
    hb_rect(1, 23, 46, 24, 0xFF5E5840);                   /* canopy hem */
    draw_soldier(56, 0);
}

/* Bunker, 128x96 (original 32x24): concrete with sandbags. */
static void draw_bunker(void)
{
    for (int y = 0; y < 96; y++)
        for (int x = 0; x < 128; x++)
            hb_put(x, y, rgb_lerp(0x8A8E92, 0x4A4E54, y, 96));
    hb_rect(0, 0, 127, 3, 0xFFB4B8BC);                    /* top edge */
    for (int x = 0; x < 128; x += 16)                     /* slab joints */
        hb_rect(x, 4, x, 60, 0xFF5A5E64);
    hb_rect(30, 22, 97, 29, 0xFF16181C);                  /* gun slit */
    for (int row = 0; row < 2; row++)                     /* sandbags */
        for (int i = -1; i < 9; i++) {
            int cx = i * 16 + 8 + (row ? 8 : 0), cy = 92 - row * 12;
            hb_ellipse(cx, cy, 9, 6, 0xB49C6E, 0x6E5A3C);
        }
}

/* Gun: 192x136 picture covering screen x 136..183, y 132..165. The barrel
 * direction is taken from the original sprite (its farthest cyan pixel). */
#define GUN_X 136
#define GUN_Y 132
static void draw_gun(int index)
{
    const uint8_t *spr = &mem[GAME_DS + mem[GAME_DS + index * 2] + (mem[GAME_DS + index * 2 + 1] << 8)];
    int px = 16, py = 9, tx = px, ty = py, best = -1;    /* pivot in sprite pixels */
    for (int y = 0; y < 13; y++)
        for (int x = 0; x < 32; x++)
            if (((spr[y * 8 + (x >> 2)] >> (6 - 2 * (x & 3))) & 3) == 1) {
                int d = (x - px) * (x - px) + (y - py) * (y - py);
                if (d > best) { best = d; tx = x; ty = y; }
            }
    /* pivot and tip on the 4x canvas (sprite origin is screen 144,145) */
    int cx = (144 + px - GUN_X) * 4 + 2, cy = (145 + py - GUN_Y) * 4 + 2;
    int ex = (144 + tx - GUN_X) * 4 + 2, ey = (145 + ty - GUN_Y) * 4 + 2;
    hb_line(cx, cy, ex, ey, 11, 0xFF20252C);              /* barrel outline */
    hb_line(cx, cy, ex, ey, 7, 0xFF6E7A88);
    hb_line(cx, cy, ex, ey, 2, 0xFFA8B4C0);               /* highlight */
    hb_line(ex - (ex - cx) / 8, ey - (ey - cy) / 8, ex, ey, 13, 0xFF3A424C);   /* muzzle */
    hb_rect(cx - 22, cy + 8, cx + 22, 135, 0xFF2E343C);   /* turret base */
    hb_rect(cx - 20, cy + 8, cx + 20, 135, 0xFF4A5462);
    hb_ellipse(cx, cy + 8, 20, 14, 0x9AA6B4, 0x3E4652);   /* dome */
    hb_ellipse(cx - 7, cy + 2, 5, 3, 0xD0DAE4, 0xA0AAB6); /* shine */
}

static void make_sprites(void)
{
    for (int frame = 0; frame < 4; frame++)
        for (int facing = 0; facing < 2; facing++) {
            cv_begin(96, 40);
            draw_helicopter(frame);
            if (facing) cv_mirror();
            heli_tex[facing][frame] = cv_texture();
            cv_end();
        }
    cv_begin(48, 88); draw_trooper(); trooper_tex = cv_texture(); cv_end();
    for (int pose = 0; pose < 3; pose++) {
        cv_begin(48, 32); draw_soldier(0, pose); soldier_tex[pose] = cv_texture(); cv_end();
    }
    cv_begin(128, 96); draw_bunker(); bunker_tex = cv_texture(); cv_end();
    for (int i = 0; i < 21; i++) {
        cv_begin(192, 136); draw_gun(i); gun_tex[i] = cv_texture(); cv_end();
    }
}

/* ---------- finding sprites on the CGA screen ---------- */

typedef struct { SDL_Texture *tex; SDL_Rect dst; } SpriteHit;
static SpriteHit hits[160];
static int hit_count;

static const uint8_t *cga_row(int y) { return &mem[0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80]; }

/* Queue a picture covering w x h original pixels at (x, y). The 320x200
 * game screen is stretched over the 640x480 logical view. */
static void add_hit(SDL_Texture *tex, int x, int y, int w, int h)
{
    if (hit_count >= 160) return;
    SDL_Rect d = { x * 2, y * 12 / 5, w * 2, (y + h) * 12 / 5 - y * 12 / 5 };
    hits[hit_count++] = (SpriteHit){ tex, d };
}

/* Make original pixels transparent: byte column xb, `bytes` wide, `rows` high. */
static void hide(int xb, int y, int bytes, int rows)
{
    for (int k = 0; k < rows; k++)
        if (y + k >= 0 && y + k < 200)
            memset(&pixels[(y + k) * 320 + xb * 4], 0, (size_t)bytes * 4 * sizeof(uint32_t));
}

/* Helicopters: exact match. xb may be partly off screen (-4..78), because
 * the game clips helicopters entering or leaving at the sides. */
static void find_helicopters(void)
{
    for (int y = 0; y <= 190; y++) {
        for (int xb = -4; xb <= 78; xb++) {
            int c0 = xb < 0 ? -xb : 0;              /* first visible sprite column */
            int c1 = xb > 74 ? 80 - xb : 6;         /* one past the last */
            for (int f = 0; f < 8; f++) {
                const uint8_t *spr = &mem[GAME_DS + (f < 4 ? 0x08BE : 0x09AE) + (f & 3) * 60];
                int r = 0;
                while (r < 10 && memcmp(cga_row(y + r) + xb + c0, spr + r * 6 + c0, c1 - c0) == 0)
                    r++;
                if (r < 10) continue;
                int solid = 0;                      /* don't match empty sky at the edges */
                for (int k = 0; k < 10; k++)
                    for (int c = c0; c < c1; c++)
                        solid += spr[k * 6 + c] != 0;
                if (solid < 6) continue;
                add_hit(heli_tex[f >= 4][f & 3], xb * 4, y, 24, 10);
                hide(xb + c0, y, c1 - c0, 10);
                xb += 5;
                break;
            }
        }
    }
}

/* Paratroopers and soldiers: only the sprite's non-empty bytes are compared,
 * because neighbours and bullets overwrite the empty edges. */
static int match_loose(const uint8_t *spr, int xb, int y, int bytes, int rows)
{
    for (int r = 0; r < rows; r++) {
        const uint8_t *scr = cga_row(y + r) + xb;
        for (int c = 0; c < bytes; c++)
            if (spr[r * bytes + c] && spr[r * bytes + c] != scr[c])
                return 0;
    }
    return 1;
}

static void hide_loose(const uint8_t *spr, int xb, int y, int bytes, int rows)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < bytes; c++)
            if (spr[r * bytes + c])
                hide(xb + c, y + r, 1, 1);
}

static void find_troopers(void)
{
    const uint8_t *chute = &mem[GAME_DS + 0x0C7E];       /* 3 x 22 */
    const uint8_t *man = &mem[GAME_DS + 0x0CA8];         /* 3 x 8, = rows 14..21 of chute */
    static uint8_t used[200][80];                        /* soldiers already part of a paratrooper */
    memset(used, 0, sizeof used);
    for (int y = 0; y + 22 <= 200; y++)
        for (int xb = 0; xb <= 77; xb++)
            if (match_loose(chute, xb, y, 3, 22)) {
                add_hit(trooper_tex, xb * 4, y, 12, 22);
                hide_loose(chute, xb, y, 3, 22);
                used[y + 14][xb] = 1;
            }
    for (int y = 0; y + 8 <= 200; y++)
        for (int xb = 0; xb <= 77; xb++)
            if (!used[y][xb] && match_loose(man, xb, y, 3, 8)) {
                add_hit(soldier_tex[0], xb * 4, y, 12, 8);
                hide_loose(man, xb, y, 3, 8);
            }
    /* walking soldiers: one byte wide, drawn where the standing soldier's
     * middle byte would be */
    static const uint16_t walk[2] = { 0x213B, 0x214B };
    for (int y = 0; y + 8 <= 200; y++)
        for (int xb = 1; xb <= 78; xb++)
            for (int k = 0; k < 2; k++) {
                const uint8_t *spr = &mem[GAME_DS + walk[k]];
                int r = 0;
                while (r < 8 && cga_row(y + r)[xb] == spr[r]) r++;
                if (r < 8) continue;
                add_hit(soldier_tex[1 + k], (xb - 1) * 4, y, 12, 8);
                hide(xb, y, 1, 8);
            }
}

/* Bunker and gun sit at fixed places. */
static void find_base(void)
{
    int bunker = 1;
    for (int y = 166; y <= 189 && bunker; y++)
        for (int c = 36; c < 44; c++)
            if (cga_row(y)[c] != 0xFF) { bunker = 0; break; }
    if (bunker) {
        add_hit(bunker_tex, 144, 166, 32, 24);
        hide(36, 166, 8, 24);
    }
    for (int i = 0; i < 21; i++) {
        const uint8_t *spr = &mem[GAME_DS + mem[GAME_DS + i * 2] + (mem[GAME_DS + i * 2 + 1] << 8)];
        int r = 0;
        while (r < 13 && memcmp(cga_row(145 + r) + 36, spr + r * 8, 8) == 0) r++;
        if (r < 13) continue;
        add_hit(gun_tex[i], GUN_X, GUN_Y, 48, 34);
        hide(36, 145, 8, 13);                            /* gun sprite */
        hide(38, 158, 4, 8);                             /* lower turret */
        break;
    }
}

static void present(void)
{
    uint32_t pal[4];
    pal[0] = 0x00000000;          /* colour 0 is transparent: the backdrop shows through */
    for (int i = 1; i < 4; i++) {
        int c = video.palette ? (i == 1 ? 3 : i == 2 ? 5 : 7) : (i * 2);
        pal[i] = 0xFF000000u | cga_rgb[c + (video.intensity ? 8 : 0)];
    }
    hit_count = 0;
    if (video.mode == 4 || video.mode == 5) {
        for (int y = 0; y < 200; y++) {
            const uint8_t *line = cga_row(y);
            uint32_t *dst = &pixels[y * 320];
            for (int x = 0; x < 320; x++)
                dst[x] = pal[(line[x >> 2] >> (6 - 2 * (x & 3))) & 3];
        }
        find_base();
        find_troopers();
        find_helicopters();
    } else {
        memset(pixels, 0, sizeof pixels);
    }
    SDL_UpdateTexture(texture, NULL, pixels, 320 * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, background, NULL, NULL);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    for (int i = 0; i < hit_count; i++)
        SDL_RenderCopy(renderer, hits[i].tex, NULL, &hits[i].dst);
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

static void show_frame(void)
{
    next_frame = cycles + CYCLES_PER_FRAME;
    pump_events();
    present();
}

void machine_sync(void)
{
    sync_at = cycles + CYCLES_PER_MS;
    uint64_t emu_ms = cycles / CYCLES_PER_MS;
    uint64_t real_ms = SDL_GetTicks64() - start_ms;
    if (emu_ms > real_ms + 1)
        SDL_Delay((Uint32)(emu_ms - real_ms));
    else if (real_ms > emu_ms + 250)
        start_ms = SDL_GetTicks64() - emu_ms;   /* host too slow: don't rush to catch up */
    /* Normally the frame is shown from machine_ticks(), when the game has
     * finished drawing and waits for the timer. This is only a fallback for
     * moments when it doesn't poll the timer (e.g. waiting for a key). */
    if (cycles >= next_frame + 4 * CYCLES_PER_FRAME)
        show_frame();
}

uint32_t machine_ticks(void)
{
    /* The game polls the timer between frames, so the screen is complete
     * here: a good moment to show it (no half-drawn sprites). */
    if (cycles >= next_frame)
        show_frame();
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
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    make_background();

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
    make_sprites();   /* needs the game image in memory (gun sprites) */
    start_ms = SDL_GetTicks64();
    next_frame = 0;
    game_run(0x0000);   /* never returns */
    SDL_Quit();
    return 0;
}