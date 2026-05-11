// mario_gfx.c — minimal SDL2 graphics + input shim for mario.alc.
//
// alcove's libffi bridge has tight constraints (max 8 args, no
// struct-by-value, no callbacks, no varargs), so this shim flattens SDL2
// behind a small set of int-returning functions that draw chunky
// pixel-art sprites built from filled rectangles. The .alc side knows
// nothing about SDL — it just calls gfx-* functions and sprite IDs.
//
// Build via the sibling Makefile (links against SDL2). Loaded by mario.alc
// through `(ffi-fn "libmariogfx.so" ...)`. macOS produces the .dylib and
// symlinks .so to it so the same alcove code works on both platforms.

#include <SDL.h>
#include <SDL_mixer.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIEW_W 800
#define VIEW_H 480
#define SCALE_MIN 1
#define SCALE_MAX 8

static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;

static int g_left = 0, g_right = 0, g_jump = 0;
static int g_quit_pressed = 0;
static int g_should_close = 0;

// ---------------------------------------------------------------------------
// Audio: square-wave chiptune synth.
//
// SDL fills our `audio_cb` from its own thread at AUDIO_RATE Hz, mono S16.
// The callback advances a note index through `g_melody`, generating a
// 50% square wave at the current note's frequency. Rests are silence.
// One "tick" = TICK_MS milliseconds; each note has a tick count.
// ---------------------------------------------------------------------------

// ===========================================================================
// Approximate-NES synth.
//
// We run three voices: two pulse-wave channels (square @ 50% duty) and one
// triangle-wave channel. The NES has 2 pulse + 1 triangle + 1 noise + DPCM;
// we cover the first three. SFX share a fourth oscillator that can be either
// square (sweeps / coin / jump) or noise (brick break).
//
// Tempo: 200 BPM, 16th-note grid → 75 ms per "tick".
// Each voice has its own note table; tables are arranged so all three loop
// in lock-step (same total ticks per period).
// ===========================================================================

#define AUDIO_RATE   44100
#define TICK_MS      75            // 200 BPM, 16th note grid
#define MELODY_AMP   2400
#define HARM_AMP     1500
#define BASS_AMP     2800          // triangle is quieter perceived than square
#define SFX_AMP      4500

// Note frequencies (Hz, rounded — equal temperament from A4=440).
// Octave 1 (bass)
#define A1   55
#define C2   65
#define D2   73
#define E2   82
#define F2   87
#define G2   98
#define A2  110
#define B2  123
#define C3  131
#define D3  147
#define E3  165
#define F3  175
#define G3  196
#define A3  220
// Octave 4-6 (melody)
#define C4  262
#define D4  294
#define E4  330
#define F4  349
#define G4  392
#define A4  440
#define AS4 466    // A#4 / Bb4
#define B4  494
#define C5  523
#define CS5 554
#define D5  587
#define DS5 622
#define E5  659
#define F5  698
#define G5  784
#define GS5 831
#define A5  880
#define B5  988
#define C6  1047
#define D6  1175
#define E6  1319
#define G6  1568
#define C7  2093
#define E7  2637
#define G7  3136

typedef struct { int freq; int ticks; } note_t;

// ---------------------------------------------------------------------------
// Original chiptune — three voices over a I–vi–IV–V progression in C major,
// 8 bars total, loops cleanly. Written for this project; swap freely.
//
// All three tables sum to the same number of ticks (64) so the voices stay
// phase-locked across the loop.
// ---------------------------------------------------------------------------
static const note_t g_melody[] = {
    // Bar 1 (C):  bouncy upward run
    {C5,1},{E5,1},{G5,1},{E5,1},{C5,1},{E5,1},{G5,1},{ 0,1},
    // Bar 2 (Am): pivot, descending
    {A4,1},{C5,1},{E5,1},{C5,1},{A4,1},{C5,1},{E5,1},{ 0,1},
    // Bar 3 (F):  step up, hold
    {F4,1},{A4,1},{C5,1},{F5,1},{C5,1},{A4,1},{F4,1},{ 0,1},
    // Bar 4 (G):  lead-in to repeat
    {G4,1},{B4,1},{D5,1},{G5,1},{D5,1},{B4,1},{G4,1},{ 0,1},
    // Bar 5 (C):  upper octave, brighter
    {E5,1},{G5,1},{C6,1},{G5,1},{E5,1},{G5,1},{C6,1},{ 0,1},
    // Bar 6 (Am): bell-like
    {C5,1},{E5,1},{A5,1},{E5,1},{C5,1},{E5,1},{A5,1},{ 0,1},
    // Bar 7 (F):  walking down
    {C5,1},{A4,1},{F4,1},{A4,1},{C5,1},{F5,1},{A5,1},{ 0,1},
    // Bar 8 (G):  resolve toward C
    {D5,1},{G4,1},{B4,1},{D5,1},{G5,1},{D5,1},{B4,1},{ 0,1},
};
#define MELODY_LEN ((int)(sizeof(g_melody)/sizeof(g_melody[0])))

// Harmony — moves in thirds with the melody, sparser rhythm so the lead breathes.
static const note_t g_harmony[] = {
    {E4,2},{ 0,1},{C5,2},{ 0,1},{G4,2},
    {C4,2},{ 0,1},{A4,2},{ 0,1},{E4,2},
    {A3,2},{ 0,1},{F4,2},{ 0,1},{C4,2},
    {D4,2},{ 0,1},{G4,2},{ 0,1},{B4,2},
    {G4,2},{ 0,1},{E5,2},{ 0,1},{C5,2},
    {E4,2},{ 0,1},{A4,2},{ 0,1},{C5,2},
    {F4,2},{ 0,1},{A4,2},{ 0,1},{F4,2},
    {B4,2},{ 0,1},{D5,2},{ 0,1},{G4,2},
};
#define HARMONY_LEN ((int)(sizeof(g_harmony)/sizeof(g_harmony[0])))

// Triangle bass — quarter-note pulses on chord roots, then the fifth.
// Pattern: root-root-fifth-fifth per bar (a classic chiptune bassline shape).
static const note_t g_bass[] = {
    {C3,2},{C3,2},{G2,2},{G2,2},   // C major
    {A2,2},{A2,2},{E2,2},{E2,2},   // A minor
    {F2,2},{F2,2},{C3,2},{C3,2},   // F major
    {G2,2},{G2,2},{D3,2},{D3,2},   // G major
    {C3,2},{C3,2},{G2,2},{G2,2},
    {A2,2},{A2,2},{E2,2},{E2,2},
    {F2,2},{F2,2},{C3,2},{C3,2},
    {G2,2},{G2,2},{D3,2},{D3,2},
};
#define BASS_LEN ((int)(sizeof(g_bass)/sizeof(g_bass[0])))

static SDL_AudioDeviceID g_audio_dev = 0;
static int g_music_on = 0;

// Three independent voice cursors.
static int g_m_idx = 0, g_m_samples = 0, g_m_phase = 0, g_m_period = 0;
static int g_h_idx = 0, g_h_samples = 0, g_h_phase = 0, g_h_period = 0;
static int g_b_idx = 0, g_b_samples = 0, g_b_phase = 0, g_b_period = 0;

// ---------------------------------------------------------------------------
// SFX sequencer — each effect is a short list of (freq, ms) steps.
// kind=0 coin, 1 stomp, 2 jump, 3 bump, 4 break (noise), 5 1-up, 6 death.
// ---------------------------------------------------------------------------
typedef struct { int freq; int ms; } sfx_step_t;

static const sfx_step_t sfx_coin[]  = {{B5, 60}, {E6, 350}};
static const sfx_step_t sfx_stomp[] = {{300, 30}, {220, 40}, {160, 50}};
static const sfx_step_t sfx_jump[]  = {{350, 25}, {500, 25}, {650, 25},
                                       {780, 25}, {900, 25}, {990, 60}};
static const sfx_step_t sfx_bump[]  = {{180, 80}};
static const sfx_step_t sfx_break[] = {{0, 120}};                 // noise
static const sfx_step_t sfx_1up[]   = {{E5, 90}, {G5, 90}, {E6, 90},
                                       {C6, 90}, {D6, 90}, {G6, 220}};
static const sfx_step_t sfx_death[] = {{C5, 90}, {B4, 90}, {AS4, 90},
                                       {A4, 90}, {GS5, 90}, {G5, 220}};

typedef struct { const sfx_step_t *steps; int count; int is_noise; } sfx_def_t;
static const sfx_def_t sfx_table[] = {
    {sfx_coin,  2, 0},
    {sfx_stomp, 3, 0},
    {sfx_jump,  6, 0},
    {sfx_bump,  1, 0},
    {sfx_break, 1, 1},
    {sfx_1up,   6, 0},
    {sfx_death, 6, 0},
};
#define SFX_COUNT ((int)(sizeof(sfx_table)/sizeof(sfx_table[0])))

static const sfx_def_t *g_sfx_def = NULL;
static int g_sfx_step = 0;
static int g_sfx_step_samples_left = 0;
static int g_sfx_phase = 0;
static int g_sfx_period = 0;
static uint32_t g_noise_lfsr = 0xACE1u;

// Triangle wave at given phase (0..period-1). Returns -amp..+amp linearly.
static inline int tri_sample(int phase, int period, int amp) {
    int half = period / 2;
    if (half <= 0) return 0;
    int p = phase < half ? phase : (period - phase);
    // p ranges 0..half; map to -amp..+amp
    return (p * 2 * amp / half) - amp;
}

// Advance one voice cursor — generic helper used for melody and harmony.
static inline int square_step(const note_t *table, int table_len,
                              int *idx, int *samples_left,
                              int *phase, int *period,
                              int tick_samples, int amp) {
    if (*samples_left <= 0) {
        const note_t *n = &table[*idx];
        *idx = (*idx + 1) % table_len;
        *samples_left = tick_samples * n->ticks;
        *period = (n->freq > 0) ? (AUDIO_RATE / n->freq) : 0;
        *phase = 0;
    }
    int v = 0;
    if (*period > 0) {
        v = (*phase < *period / 2) ? amp : -amp;
        (*phase)++;
        if (*phase >= *period) *phase = 0;
    }
    (*samples_left)--;
    return v;
}

static void audio_cb(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int samples = len / 2;
    int tick_samples = (AUDIO_RATE * TICK_MS) / 1000;
    for (int i = 0; i < samples; i++) {
        int mix = 0;
        if (g_music_on) {
            mix += square_step(g_melody,  MELODY_LEN,
                               &g_m_idx, &g_m_samples, &g_m_phase, &g_m_period,
                               tick_samples, MELODY_AMP);
            mix += square_step(g_harmony, HARMONY_LEN,
                               &g_h_idx, &g_h_samples, &g_h_phase, &g_h_period,
                               tick_samples, HARM_AMP);
            // Triangle voice (bass): advance the cursor like the others, but
            // emit a triangle-wave sample instead of a square.
            if (g_b_samples <= 0) {
                const note_t *n = &g_bass[g_b_idx];
                g_b_idx = (g_b_idx + 1) % BASS_LEN;
                g_b_samples = tick_samples * n->ticks;
                g_b_period  = (n->freq > 0) ? (AUDIO_RATE / n->freq) : 0;
                g_b_phase   = 0;
            }
            if (g_b_period > 0) {
                mix += tri_sample(g_b_phase, g_b_period, BASS_AMP);
                g_b_phase++;
                if (g_b_phase >= g_b_period) g_b_phase = 0;
            }
            g_b_samples--;
        }

        // --- SFX one-shot
        if (g_sfx_def) {
            if (g_sfx_step_samples_left <= 0) {
                if (g_sfx_step >= g_sfx_def->count) {
                    g_sfx_def = NULL;        // done
                } else {
                    const sfx_step_t *s = &g_sfx_def->steps[g_sfx_step++];
                    g_sfx_step_samples_left = (AUDIO_RATE * s->ms) / 1000;
                    g_sfx_period = (s->freq > 0) ? (AUDIO_RATE / s->freq) : 0;
                    g_sfx_phase = 0;
                }
            }
            if (g_sfx_def) {
                int v = 0;
                if (g_sfx_def->is_noise) {
                    // 16-bit LFSR — bit 0 toggles each sample for white noise.
                    g_noise_lfsr ^= g_noise_lfsr << 7;
                    g_noise_lfsr ^= g_noise_lfsr >> 9;
                    g_noise_lfsr ^= g_noise_lfsr << 8;
                    v = (g_noise_lfsr & 1) ? SFX_AMP : -SFX_AMP;
                } else if (g_sfx_period > 0) {
                    v = (g_sfx_phase < g_sfx_period / 2) ? SFX_AMP : -SFX_AMP;
                    g_sfx_phase++;
                    if (g_sfx_phase >= g_sfx_period) g_sfx_phase = 0;
                }
                mix += v;
                g_sfx_step_samples_left--;
            }
        }

        // Clamp — four voices summed can exceed int16 range.
        if (mix >  32000) mix =  32000;
        if (mix < -32000) mix = -32000;
        out[i] = (int16_t)mix;
    }
}

int gfx_music_start(void) {
    if (g_audio_dev) return 1;
    SDL_AudioSpec want = {0}, have = {0};
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_cb;
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev == 0) return 0;
    g_music_on = 1;
    g_m_idx = g_m_samples = g_m_phase = g_m_period = 0;
    g_h_idx = g_h_samples = g_h_phase = g_h_period = 0;
    g_b_idx = g_b_samples = g_b_phase = g_b_period = 0;
    SDL_PauseAudioDevice(g_audio_dev, 0);
    return 1;
}

int gfx_music_stop(void) {
    if (!g_audio_dev) return 0;
    SDL_PauseAudioDevice(g_audio_dev, 1);
    SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    g_music_on = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// File-based music: load any audio file (WAV/OGG/MP3/MOD/XM/IT/S3M) via
// SDL_mixer and play it on a separate channel from the synth.
//
// If you call gfx_music_load_file before gfx_music_start, the synth is
// suppressed and the file plays instead. The file lives on disk and is
// supplied by the user; this code only contains the loader.
// ---------------------------------------------------------------------------

static Mix_Music *g_file_music = NULL;
static int g_mixer_open = 0;

int gfx_music_load_file(const char *path) {
    if (!path || !*path) return 0;
    if (!g_mixer_open) {
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) < 0) {
            return 0;
        }
        g_mixer_open = 1;
    }
    if (g_file_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_file_music);
        g_file_music = NULL;
    }
    g_file_music = Mix_LoadMUS(path);
    return g_file_music ? 1 : 0;
}

int gfx_music_play_file(int loops) {
    if (!g_file_music) return -1;
    // Suppress the procedural synth if it's running so we don't double up.
    g_music_on = 0;
    Mix_VolumeMusic(MIX_MAX_VOLUME / 2);
    return Mix_PlayMusic(g_file_music, loops);
}

int gfx_music_unload_file(void) {
    if (g_file_music) {
        Mix_HaltMusic();
        Mix_FreeMusic(g_file_music);
        g_file_music = NULL;
    }
    if (g_mixer_open) {
        Mix_CloseAudio();
        g_mixer_open = 0;
    }
    return 0;
}

// Trigger an SFX sequence. kind: 0 coin, 1 stomp, 2 jump, 3 bump,
// 4 brick-break (noise), 5 one-up, 6 death.
int gfx_sfx(int kind) {
    if (!g_audio_dev) return 0;
    if (kind < 0 || kind >= SFX_COUNT) return 0;
    SDL_LockAudioDevice(g_audio_dev);
    g_sfx_def = &sfx_table[kind];
    g_sfx_step = 0;
    g_sfx_step_samples_left = 0;       // forces step-load on first sample
    g_sfx_phase = 0;
    g_sfx_period = 0;
    SDL_UnlockAudioDevice(g_audio_dev);
    return 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void on_sigint(int sig) { (void)sig; g_should_close = 1; }

int gfx_init(int width, int height) {
    if (g_win) return 1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) return 0;
    /* Install a SIGINT handler so Ctrl-C from the launching terminal
       cleanly tears down SDL instead of leaving the window hanging.
       SDL installs its own handler for some platforms; install ours
       afterwards so it wins. */
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    if (width  <= 0) width  = VIEW_W;
    if (height <= 0) height = VIEW_H;
    /* Window size = logical size × scale. The logical size is fixed at
       (width, height) via SDL_RenderSetLogicalSize below, so changing
       scale only resizes the window — the game's coordinate system
       stays put. Override with MARIO_SCALE=N in the environment. */
    int scale = 1;
    const char *scale_env = getenv("MARIO_SCALE");
    if (scale_env && *scale_env) {
        int s = atoi(scale_env);
        if (s >= SCALE_MIN && s <= SCALE_MAX) scale = s;
    }
    g_win = SDL_CreateWindow("alcove mario",
                             SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             width * scale, height * scale,
                             SDL_WINDOW_SHOWN);
    if (!g_win) { SDL_Quit(); return 0; }
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED |
                               SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) {
        SDL_DestroyWindow(g_win); g_win = NULL;
        SDL_Quit(); return 0;
    }
    SDL_RenderSetLogicalSize(g_ren, width, height);
    /* On macOS, a freshly-created SDL window from a terminal-launched
       process often doesn't take focus — keyboard events get delivered
       to whatever has focus (the terminal). Raise + ShowWindow + a
       brief event-pump loop coerces the WM to give us focus reliably. */
    SDL_ShowWindow(g_win);
    SDL_RaiseWindow(g_win);
    Uint32 t0 = SDL_GetTicks();
    SDL_Event ev;
    while (SDL_GetTicks() - t0 < 150) {
        while (SDL_PollEvent(&ev)) { /* drain — discards startup events */ }
        SDL_Delay(5);
    }
    return 1;
}

int gfx_quit(void) {
    if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win);   g_win = NULL; }
    SDL_Quit();
    return 0;
}

// ---------------------------------------------------------------------------
// Event pump and input
// ---------------------------------------------------------------------------

int gfx_pump(void) {
    SDL_Event e;
    g_quit_pressed = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { g_should_close = 1; }
        else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            int down = (e.type == SDL_KEYDOWN) ? 1 : 0;
            switch (e.key.keysym.sym) {
                case SDLK_LEFT:  case SDLK_a: g_left  = down; break;
                case SDLK_RIGHT: case SDLK_d: g_right = down; break;
                case SDLK_SPACE: case SDLK_z:
                case SDLK_UP:    case SDLK_w: case SDLK_k:
                    g_jump = down; break;
                case SDLK_ESCAPE: case SDLK_q:
                    if (down) { g_quit_pressed = 1; g_should_close = 1; }
                    break;
                default: break;
            }
        }
    }
    return 0;
}

int gfx_left(void)         { return g_left; }
int gfx_right(void)        { return g_right; }
int gfx_jump(void)         { return g_jump; }
int gfx_quit_pressed(void) { return g_quit_pressed; }
int gfx_should_close(void) { return g_should_close; }

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

static void set_color(int rgb) {
    Uint8 r = (rgb >> 16) & 0xFF;
    Uint8 g = (rgb >>  8) & 0xFF;
    Uint8 b =  rgb        & 0xFF;
    SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
}

int gfx_clear(int rgb) {
    if (!g_ren) return 0;
    set_color(rgb);
    SDL_RenderClear(g_ren);
    return 0;
}

int gfx_present(void) { if (g_ren) SDL_RenderPresent(g_ren); return 0; }

int gfx_fill(int x, int y, int w, int h, int rgb) {
    if (!g_ren) return 0;
    set_color(rgb);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(g_ren, &r);
    return 0;
}

// ---------------------------------------------------------------------------
// Sprite catalogue
//
// Sprites are colored rectangles laid out at chunky-pixel resolution.
// All sprite IDs are mirrored as T-* tile constants in mario.alc.
// ---------------------------------------------------------------------------

#define SPR_MARIO        0
#define SPR_MARIO_R      1
#define SPR_MARIO_L      2
#define SPR_GROUND       3
#define SPR_BRICK        4
#define SPR_QBLOCK       5
#define SPR_USED_BLOCK   6
#define SPR_PIPE_TL      7
#define SPR_PIPE_TR      8
#define SPR_PIPE_BL      9
#define SPR_PIPE_BR     10
#define SPR_GOOMBA      11
#define SPR_COIN        12
#define SPR_FLAG_POLE   13
#define SPR_FLAG        14
#define SPR_DEAD_GOOMBA 15
#define SPR_CASTLE      16
#define SPR_CLOUD       17

static void px(int x, int y, int w, int h, int rgb) { gfx_fill(x, y, w, h, rgb); }

// ---------------------------------------------------------------------------
// Hero pixel-art — side-view profile that mirrors with `dir`. Designed
// for dir=+1 (facing right); the MX macro flips x positions for dir=-1.
//
// Profile cues: cap bulges forward (toward the face), single visible eye
// on the forward side, nose pokes out, mustache curls under the nose,
// front arm visible (back arm tucked behind), boots have a toe extension
// pointing in the facing direction.
//
// 4 frames at 24x30 px:
//   0 = stand, 1 = walk-A, 2 = walk-B, 3 = jump
// ---------------------------------------------------------------------------
static void draw_hero(int x, int y, int dir, int frame) {
    int cap_hi  = 0xFF5050;
    int cap     = 0xCC1818;
    int cap_dk  = 0x801010;
    int skin_hi = 0xFFD0A0;
    int skin    = 0xF8B080;
    int skin_dk = 0xC08060;
    int hair    = 0x402010;
    int eye_w   = 0xF0F0F0;
    int eye_b   = 0x101020;
    int blue_hi = 0x60A0FF;
    int blue    = 0x2860C9;
    int blue_dk = 0x103080;
    int yellow  = 0xFCE000;
    int boot    = 0x6B3D1A;
    int boot_dk = 0x402010;

    // MX: x offset that mirrors when dir<0. Designed for sprite width 24.
    // off = x-offset from left when facing right; w = rect width.
    #define MX(off, w) ((dir > 0) ? (x + (off)) : (x + 24 - (off) - (w)))

    // ---- Cap (rows 0-7). Bulges forward and droops a brim.
    // Back of cap (smaller)
    px(MX(6, 4),  y+1,  4, 2, cap);
    // Cap mid-section
    px(MX(4, 8),  y+3,  8, 2, cap);
    // Cap front bulge
    px(MX(8, 10), y+1,  10, 2, cap);
    px(MX(8, 12), y+3,  12, 2, cap_hi);    // highlight band
    px(MX(4, 16), y+5,  16, 2, cap);
    // Brim — extends further forward than backward
    px(MX(2, 18), y+7,  18, 2, cap_dk);
    px(MX(18, 4), y+5,   4, 2, cap_dk);    // front lip droops

    // ---- Head (rows 8-14). Skin in profile, hair at the back.
    px(MX(4, 2),  y+8,  2, 5, hair);       // sideburn / back of head
    px(MX(6, 12), y+8, 12, 6, skin);       // face
    px(MX(6, 1),  y+8,  1, 6, skin_dk);    // shadow at back of face
    px(MX(15, 2), y+8,  2, 1, skin_hi);    // forehead highlight (front)

    // Single visible eye on the front side
    px(MX(13, 2), y+10, 2, 2, eye_w);
    px(MX(14, 1), y+10, 1, 2, eye_b);

    // Nose pokes out the front
    px(MX(17, 2), y+11, 2, 2, skin);
    px(MX(18, 1), y+12, 1, 1, skin_dk);    // nostril shadow

    // Mustache curls under nose toward the front
    px(MX(10, 6), y+13, 6, 2, hair);
    px(MX(15, 3), y+13, 3, 1, hair);       // mustache curl tip

    // Chin
    px(MX(8, 6),  y+14, 6, 1, skin);

    // ---- Body (rows 15-23)
    // Shirt collar (front higher than back)
    px(MX(6, 12), y+15, 12, 2, cap_dk);
    px(MX(15, 3), y+14, 3, 1, cap_dk);
    // Shirt
    px(MX(4, 16), y+17, 16, 3, cap);
    px(MX(4, 1),  y+17,  1, 3, cap_dk);    // back shadow
    px(MX(18, 2), y+17,  2, 3, cap_hi);    // front highlight

    // Arms
    if (frame == 3) {
        // Jump: both arms raised
        px(MX(2, 4),  y+15, 4, 5, blue);   // back arm
        px(MX(18, 4), y+15, 4, 5, blue);   // front arm
    } else if (frame == 1) {
        // Walk-A: front arm swings back, back arm forward (counter-balance)
        px(MX(3, 4),  y+16, 4, 5, blue);   // back arm forward-ish
        px(MX(16, 4), y+19, 4, 4, blue);   // front arm lower
    } else if (frame == 2) {
        // Walk-B: front arm forward, back arm back
        px(MX(2, 4),  y+19, 4, 4, blue);
        px(MX(17, 4), y+16, 4, 5, blue);
    } else {
        // Stand: arms at sides, front arm slightly forward
        px(MX(3, 4),  y+18, 4, 5, blue);
        px(MX(17, 4), y+18, 4, 5, blue);
    }

    // Overalls
    px(MX(4, 16), y+20, 16, 4, blue);
    px(MX(4, 1),  y+20,  1, 4, blue_dk);
    px(MX(19, 1), y+20,  1, 4, blue_hi);

    // Suspender straps (visible from this angle: one front, one wrapping)
    px(MX(7, 2),  y+15, 2, 6, blue);
    px(MX(15, 2), y+15, 2, 6, blue);
    // Buttons — only the front one is clearly visible
    px(MX(15, 2), y+17, 2, 2, yellow);
    px(MX(7, 2),  y+17, 2, 2, yellow);

    // ---- Legs / boots (rows 24-29). Toe extends in facing direction.
    if (frame == 3) {
        // Jump: legs tucked together
        px(MX(8, 4),  y+22, 4, 4, blue);
        px(MX(14, 4), y+22, 4, 4, blue);
        px(MX(7, 6),  y+24, 6, 4, boot);
        px(MX(13, 6), y+24, 6, 4, boot);
        // Toe extension on the front (jump-tucked toe)
        px(MX(19, 2), y+25, 2, 2, boot);
        px(MX(7, 12), y+27, 12, 1, boot_dk);
    } else if (frame == 1) {
        // Walk-A: front foot planted, back foot mid-step
        px(MX(14, 5), y+24, 5, 3, blue);   // front thigh planted
        px(MX(4, 5),  y+25, 5, 2, blue);   // back thigh raised
        // Front boot — toe extends forward
        px(MX(13, 7), y+27, 7, 2, boot);
        px(MX(19, 3), y+27, 3, 2, boot);   // toe tip
        // Back boot — angled back
        px(MX(3, 6),  y+27, 6, 2, boot);
        px(MX(13, 7), y+28, 7, 1, boot_dk);
        px(MX(3, 6),  y+28, 6, 1, boot_dk);
    } else if (frame == 2) {
        // Walk-B: back foot planted, front foot stepping
        px(MX(4, 5),  y+24, 5, 3, blue);
        px(MX(14, 5), y+25, 5, 2, blue);
        // Back boot under hip
        px(MX(3, 7),  y+27, 7, 2, boot);
        // Front boot — toe lifted, still toes forward
        px(MX(14, 6), y+27, 6, 2, boot);
        px(MX(19, 2), y+27, 2, 1, boot);   // small toe tip
        px(MX(3, 7),  y+28, 7, 1, boot_dk);
        px(MX(14, 6), y+28, 6, 1, boot_dk);
    } else {
        // Stand: feet parallel; toes point in facing direction
        px(MX(6, 4),  y+24, 4, 3, blue);   // back leg
        px(MX(14, 4), y+24, 4, 3, blue);   // front leg
        px(MX(5, 7),  y+27, 7, 2, boot);   // back boot
        px(MX(13, 7), y+27, 7, 2, boot);   // front boot
        px(MX(19, 2), y+27, 2, 2, boot);   // front toe tip
        px(MX(5, 7),  y+28, 7, 1, boot_dk);
        px(MX(13, 9), y+28, 9, 1, boot_dk);
    }

    #undef MX
}

// Ground tile — grass strip on top, layered dirt with pebbles below.
static void draw_ground(int x, int y) {
    int grass_hi = 0xA5E060;
    int grass    = 0x6CC04F;
    int grass_dk = 0x3F7822;
    int dirt_hi  = 0xD8A070;
    int dirt     = 0xB97A56;
    int dirt_dk  = 0x7A4828;
    int rock     = 0x4C2A14;

    // --- Grass strip (top 6 px) with tufts poking up
    px(x,     y,     32, 5, grass);
    px(x+1,   y,      2, 1, grass_hi);     // tuft 1
    px(x+4,   y+1,    3, 1, grass_hi);
    px(x+9,   y,      2, 1, grass_hi);     // tuft 2
    px(x+13,  y+1,    2, 1, grass_hi);
    px(x+17,  y,      3, 1, grass_hi);     // tuft 3
    px(x+22,  y+1,    2, 1, grass_hi);
    px(x+26,  y,      2, 1, grass_hi);     // tuft 4
    px(x+29,  y+1,    2, 1, grass_hi);
    px(x,     y+5,   32, 1, grass_dk);     // grass shadow edge

    // --- Topsoil band (a brighter strip just below the grass)
    px(x,     y+6,   32, 2, dirt_hi);

    // --- Dirt body
    px(x,     y+8,   32, 18, dirt);
    // Pebbles / clods
    px(x+3,   y+10,   5, 3, rock);
    px(x+16,  y+13,   6, 4, rock);
    px(x+9,   y+19,   4, 2, rock);
    px(x+23,  y+19,   5, 3, rock);
    px(x+5,   y+22,   3, 2, rock);
    px(x+27,  y+10,   3, 2, rock);
    // Brighter dirt specks
    px(x+11,  y+9,    2, 1, dirt_hi);
    px(x+25,  y+12,   2, 1, dirt_hi);
    px(x+6,   y+16,   2, 1, dirt_hi);
    px(x+19,  y+22,   2, 1, dirt_hi);
    px(x+1,   y+24,   2, 1, dirt_hi);
    // Darker streaks
    px(x+13,  y+17,   2, 4, dirt_dk);
    px(x+28,  y+21,   2, 3, dirt_dk);

    // --- Bottom dark band (deep soil)
    px(x,     y+26,  32, 6, dirt_dk);
    px(x,     y+26,  32, 1, dirt);         // thin highlight on top of dark band
}

// Brick tile — running-bond masonry pattern with bevelled bricks.
// Top row: 2 full bricks. Bottom row: 1 half + 1 full + 1 half (offset).
static void draw_brick(int x, int y) {
    int brick_hi = 0xE88060;
    int brick    = 0xC65A2C;
    int brick_dk = 0x8A3B17;
    int mortar   = 0x4A1F08;

    // --- Background mortar fills every gap
    px(x, y, 32, 32, mortar);

    // --- Top row: two bricks side-by-side
    // Brick A
    px(x+2,  y+2,  13, 12, brick);
    px(x+2,  y+2,  13,  1, brick_hi);   // top highlight
    px(x+2,  y+2,   1, 12, brick_hi);   // left highlight
    px(x+2,  y+13, 13,  1, brick_dk);   // bottom shadow
    px(x+14, y+2,   1, 12, brick_dk);   // right shadow
    // Brick B
    px(x+17, y+2,  13, 12, brick);
    px(x+17, y+2,  13,  1, brick_hi);
    px(x+17, y+2,   1, 12, brick_hi);
    px(x+17, y+13, 13,  1, brick_dk);
    px(x+29, y+2,   1, 12, brick_dk);

    // --- Bottom row: offset by half-brick (half + full + half)
    // Left half-brick
    px(x,    y+18,  7, 12, brick);
    px(x,    y+18,  7,  1, brick_hi);
    px(x,    y+29,  7,  1, brick_dk);
    px(x+6,  y+18,  1, 12, brick_dk);
    // Center brick
    px(x+9,  y+18, 13, 12, brick);
    px(x+9,  y+18, 13,  1, brick_hi);
    px(x+9,  y+18,  1, 12, brick_hi);
    px(x+9,  y+29, 13,  1, brick_dk);
    px(x+21, y+18,  1, 12, brick_dk);
    // Right half-brick
    px(x+24, y+18,  8, 12, brick);
    px(x+24, y+18,  8,  1, brick_hi);
    px(x+24, y+29,  8,  1, brick_dk);
    px(x+24, y+18,  1, 12, brick_hi);
}

static void draw_qblock(int x, int y, int used) {
    int border = 0x8B4513;
    int face   = used ? 0x8B4513 : 0xE0A040;
    px(x, y, 32, 32, face);
    px(x, y,    32, 2, border);
    px(x, y+30, 32, 2, border);
    px(x, y,    2,  32, border);
    px(x+30, y, 2,  32, border);
    if (!used) {
        px(x+12, y+8,  8, 2, 0xFFFFFF);
        px(x+12, y+10, 2, 6, 0xFFFFFF);
        px(x+18, y+10, 2, 4, 0xFFFFFF);
        px(x+12, y+16, 8, 2, 0xFFFFFF);
        px(x+18, y+18, 2, 2, 0xFFFFFF);
        px(x+14, y+22, 4, 2, 0xFFFFFF);
    }
}

static void draw_pipe(int x, int y, int kind) {
    int dark  = 0x0E5A12;
    int mid   = 0x10A050;
    int light = 0x4FE05F;
    int top = (kind == SPR_PIPE_TL || kind == SPR_PIPE_TR);
    int left = (kind == SPR_PIPE_TL || kind == SPR_PIPE_BL);
    if (top) {
        // Lip extends 4px wider in real Mario; here we keep within tile.
        px(x, y, 32, 8, mid);
        px(x, y, 32, 2, dark);
        px(x+2, y+2, 4, 4, light);
        px(x, y+6, 32, 2, dark);
        px(x, y+8, 32, 24, mid);
        px(x+2, y+8, 4, 24, light);
        if (left) px(x+30, y+8, 2, 24, dark);
        else      px(x, y+8, 2, 24, dark);
    } else {
        px(x, y, 32, 32, mid);
        px(x+2, y, 4, 32, light);
        if (left) px(x+30, y, 2, 32, dark);
        else      px(x, y, 2, 32, dark);
    }
}

// ---------------------------------------------------------------------------
// Walking enemy — original mushroom-creature design. 2 walk frames + dead.
// ---------------------------------------------------------------------------
static void draw_walker(int x, int y, int dead, int frame) {
    int body_hi = 0xB5651D;
    int body    = 0x8B4513;
    int body_dk = 0x5C2E0D;
    int dark    = 0x40220A;
    int eye_w   = 0xF0F0F0;
    int eye_b   = 0x202020;
    int teeth   = 0xF8E8C8;

    if (dead) {
        // Squashed flat
        px(x+2,  y+24, 28, 6, body);
        px(x+4,  y+22, 24, 2, body_hi);
        px(x+2,  y+28, 28, 2, body_dk);
        return;
    }

    // Head/cap dome (rows 0-13)
    px(x+8,  y+2,  16, 2, body);             // dome top
    px(x+6,  y+4,  20, 2, body);
    px(x+4,  y+6,  24, 2, body);
    px(x+2,  y+8,  28, 6, body);
    px(x+4,  y+4,  4,  2, body_hi);          // top-left highlight
    px(x+4,  y+8,  4,  4, body_hi);          // side highlight
    px(x+22, y+12, 6,  2, body_dk);          // bottom-right shadow

    // Furrowed brow (angry look)
    px(x+6,  y+9,  4, 2, dark);
    px(x+22, y+9,  4, 2, dark);

    // Eyes
    px(x+8,  y+10, 4, 4, eye_w);
    px(x+20, y+10, 4, 4, eye_w);
    px(x+10, y+11, 2, 3, eye_b);             // pupils — centered, looking forward
    px(x+22, y+11, 2, 3, eye_b);

    // Body underneath head (rows 14-23)
    px(x+6,  y+14, 20, 8, body);
    px(x+6,  y+14,  2, 8, body_dk);          // left shadow
    px(x+24, y+14,  2, 8, body_dk);          // right shadow

    // Small fangs/teeth on lower face
    px(x+10, y+20, 2, 2, teeth);
    px(x+14, y+20, 2, 2, teeth);
    px(x+18, y+20, 2, 2, teeth);

    // Feet — alternate position between the two walk frames
    if (frame == 0) {
        // Feet apart (mid-stride)
        px(x+2,  y+24, 10, 6, dark);
        px(x+20, y+24, 10, 6, dark);
        px(x+2,  y+28, 10, 2, body_dk);
    } else {
        // Feet together (other side of stride)
        px(x+6,  y+24, 8, 6, dark);
        px(x+18, y+24, 8, 6, dark);
        px(x+6,  y+28, 8, 2, body_dk);
    }
}

// ---------------------------------------------------------------------------
// Spinning coin — 4 frames (face-on, narrowing, edge, narrowing back).
// frame 0: full-face, brightest
// frame 1: 3/4 view, narrower
// frame 2: edge-on, just a vertical bar
// frame 3: 3/4 view (mirror of frame 1)
// ---------------------------------------------------------------------------
static void draw_coin(int x, int y, int frame) {
    int gold     = 0xFCE000;
    int gold_hi  = 0xFFF8A0;
    int gold_dk  = 0xA07000;
    int outline  = 0x402800;

    int cx = x + 16;     // center
    int cy = y + 16;
    if (frame == 0 || frame == 4) {
        // Full face — round disc with shine
        px(cx-6, cy-12, 12, 2, outline);     // top arc
        px(cx-8, cy-10, 16, 2, outline);
        px(cx-9, cy-8,  18, 16, gold);       // body
        px(cx-7, cy-10, 14, 2, gold);
        px(cx-8, cy+6,  16, 2, outline);
        px(cx-6, cy+10, 12, 2, outline);
        px(cx-2, cy-6,  2, 12, gold_hi);     // vertical shine
        px(cx+4, cy-4,  2,  8, gold_dk);     // shadow band
    } else if (frame == 1) {
        // Narrower — 3/4 view
        px(cx-4, cy-12,  8, 2, outline);
        px(cx-5, cy-10, 10, 2, outline);
        px(cx-6, cy-8,  12, 16, gold);
        px(cx-5, cy+6,  10, 2, outline);
        px(cx-4, cy+10,  8, 2, outline);
        px(cx-1, cy-6,   2, 12, gold_hi);
        px(cx+3, cy-4,   1,  8, gold_dk);
    } else if (frame == 2) {
        // Edge-on — thin bar
        px(cx-2, cy-12, 4, 24, gold_dk);
        px(cx-1, cy-10, 2, 20, gold);
    } else {
        // Narrower opposite — 3/4 view mirrored
        px(cx-4, cy-12,  8, 2, outline);
        px(cx-5, cy-10, 10, 2, outline);
        px(cx-6, cy-8,  12, 16, gold);
        px(cx-5, cy+6,  10, 2, outline);
        px(cx-4, cy+10,  8, 2, outline);
        px(cx-3, cy-6,   2, 12, gold_hi);
        px(cx+1, cy-4,   1,  8, gold_dk);
    }
}

static void draw_flag_pole(int x, int y) {
    int dark = 0x404040;
    int silver = 0xC0C0C0;
    px(x+15, y, 2, 32, silver);
    px(x+14, y, 4, 4, dark);
}

static void draw_flag(int x, int y) {
    int red    = 0xE52521;
    int dark   = 0x6B2F0F;
    int silver = 0xC0C0C0;
    px(x+15, y, 2, 32, silver);
    px(x+2,  y+4, 14, 12, red);
    px(x+2,  y+4, 14, 2, dark);
    px(x+2,  y+14, 14, 2, dark);
    px(x+2,  y+4, 2,  12, dark);
}

static void draw_castle(int x, int y) {
    int stone = 0xC0C0C0;
    int dark  = 0x606060;
    int door  = 0x202020;
    px(x, y, 32, 32, stone);
    px(x, y, 32, 2, dark);
    px(x, y+30, 32, 2, dark);
    px(x, y, 2, 32, dark);
    px(x+30, y, 2, 32, dark);
    px(x+0,  y, 4, 6, dark);
    px(x+8,  y, 4, 6, dark);
    px(x+20, y, 4, 6, dark);
    px(x+28, y, 4, 6, dark);
    px(x+12, y+12, 8, 20, door);
}

static void draw_cloud(int x, int y) {
    int white = 0xFFFFFF;
    px(x+8,  y+4,  16, 16, white);
    px(x+4,  y+8,  24, 8,  white);
    px(x,    y+12, 32, 4,  white);
}

int gfx_draw_dir(int sprite, int x, int y, int dir);

int gfx_draw(int sprite, int x, int y) {
    if (!g_ren) return 0;
    return gfx_draw_dir(sprite, x, y, 1);
}

// Internal dispatch with frame parameter. Static sprites ignore it.
static void draw_sprite(int sprite, int x, int y, int dir, int frame) {
    switch (sprite) {
        case SPR_MARIO:        draw_hero(x, y, dir, frame); break;
        case SPR_MARIO_R:      draw_hero(x, y,  1,  frame); break;
        case SPR_MARIO_L:      draw_hero(x, y, -1,  frame); break;
        case SPR_GROUND:       draw_ground(x, y); break;
        case SPR_BRICK:        draw_brick(x, y); break;
        case SPR_QBLOCK:       draw_qblock(x, y, 0); break;
        case SPR_USED_BLOCK:   draw_qblock(x, y, 1); break;
        case SPR_PIPE_TL: case SPR_PIPE_TR:
        case SPR_PIPE_BL: case SPR_PIPE_BR:
            draw_pipe(x, y, sprite); break;
        case SPR_GOOMBA:       draw_walker(x, y, 0, frame); break;
        case SPR_DEAD_GOOMBA:  draw_walker(x, y, 1, frame); break;
        case SPR_COIN:         draw_coin(x, y, frame); break;
        case SPR_FLAG_POLE:    draw_flag_pole(x, y); break;
        case SPR_FLAG:         draw_flag(x, y); break;
        case SPR_CASTLE:       draw_castle(x, y); break;
        case SPR_CLOUD:        draw_cloud(x, y); break;
        default: break;
    }
}

int gfx_draw_dir(int sprite, int x, int y, int dir) {
    if (!g_ren) return 0;
    draw_sprite(sprite, x, y, dir, 0);
    return 0;
}

// Frame-aware variant — `frame` selects walk-cycle / spin-cycle pose.
int gfx_draw_anim(int sprite, int x, int y, int dir, int frame) {
    if (!g_ren) return 0;
    draw_sprite(sprite, x, y, dir, frame);
    return 0;
}

// ---------------------------------------------------------------------------
// 8x8 bitmap font for HUD text
// ---------------------------------------------------------------------------

// Each glyph is 8 rows of one byte. MSB = leftmost pixel.
typedef struct { int code; uint8_t rows[8]; } Glyph;

static const Glyph g_glyphs[] = {
    {' ', {0,0,0,0,0,0,0,0}},
    {'!', {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}},
    {'-', {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}},
    {':', {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}},
    {'x', {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}},
    {'0', {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}},
    {'1', {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}},
    {'2', {0x3C,0x66,0x06,0x1C,0x30,0x66,0x7E,0x00}},
    {'3', {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}},
    {'4', {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}},
    {'5', {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}},
    {'6', {0x3C,0x60,0x60,0x7C,0x66,0x66,0x3C,0x00}},
    {'7', {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}},
    {'8', {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}},
    {'9', {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}},
    {'A', {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}},
    {'B', {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}},
    {'C', {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}},
    {'D', {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}},
    {'E', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}},
    {'F', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}},
    {'G', {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}},
    {'H', {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}},
    {'I', {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}},
    {'K', {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}},
    {'L', {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}},
    {'M', {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}},
    {'N', {0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00}},
    {'O', {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}},
    {'P', {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}},
    {'R', {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}},
    {'S', {0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C,0x00}},
    {'T', {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}},
    {'U', {0x66,0x66,0x66,0x66,0x66,0x66,0x3E,0x00}},
    {'V', {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}},
    {'W', {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}},
    {'Y', {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}},
    {'Z', {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}},
};

#define N_GLYPHS (int)(sizeof(g_glyphs)/sizeof(g_glyphs[0]))

static const Glyph *find_glyph(int code) {
    if (code >= 'a' && code <= 'z') code -= 32;
    for (int i = 0; i < N_GLYPHS; i++)
        if (g_glyphs[i].code == code) return &g_glyphs[i];
    return NULL;
}

static void draw_glyph(const Glyph *g, int x, int y, int scale, int rgb) {
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g->rows[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfx_fill(x + col * scale, y + row * scale, scale, scale, rgb);
            }
        }
    }
}

// gfx_text takes an int-encoded byte stream by piggybacking on a small
// internal write buffer set via gfx_text_set. This avoids passing strings
// over libffi (which alcove's bridge doesn't reliably support for this
// pattern).
static char g_text_buf[64];

int gfx_text_set(int idx, int ch) {
    if (idx < 0 || idx >= (int)sizeof(g_text_buf)) return 0;
    g_text_buf[idx] = (char)ch;
    return 0;
}

int gfx_text(int x, int y, int len, int scale, int rgb) {
    if (!g_ren) return 0;
    if (scale < 1) scale = 1;
    if (len < 0) len = 0;
    if (len > (int)sizeof(g_text_buf)) len = sizeof(g_text_buf);
    int cx = x;
    for (int i = 0; i < len; i++) {
        const Glyph *gl = find_glyph((unsigned char)g_text_buf[i]);
        if (gl) draw_glyph(gl, cx, y, scale, rgb);
        cx += 9 * scale;
    }
    return 0;
}

int gfx_text_int(int x, int y, int value, int scale, int rgb) {
    if (!g_ren) return 0;
    if (scale < 1) scale = 1;
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", value);
    int cx = x;
    for (int i = 0; i < n; i++) {
        const Glyph *gl = find_glyph((unsigned char)buf[i]);
        if (gl) draw_glyph(gl, cx, y, scale, rgb);
        cx += 9 * scale;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

int gfx_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // Wraps after ~24 days; we only ever take diffs over one frame.
    return (int)((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
}

int gfx_sleep_ms(int ms) {
    if (ms <= 0) return 0;
    SDL_Delay((Uint32)ms);
    return 0;
}
