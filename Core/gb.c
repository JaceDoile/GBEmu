// Game Boy / Game Boy Color emulation core.
// CPU is M-cycle stepped: every bus access advances the rest of the system
// by 4 clocks, which keeps timer/PPU/DMA timing consistent with hardware.
#include "gb.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

#define STATE_MAGIC   0x53534247u  /* "GBSS" */
#define STATE_VERSION 1

enum { MBC_NONE, MBC_1, MBC_2, MBC_3, MBC_5 };

typedef struct {
    uint8_t  on;
    uint16_t length;
    uint8_t  vol, env_timer;
    int32_t  timer;
    uint8_t  pos;
    uint8_t  sample;       // wave channel current sample
    uint16_t lfsr;
    uint8_t  sweep_timer, sweep_enabled, neg_used;
    uint16_t shadow;
} Chan;

// Everything in here is plain data and is what a save state captures.
typedef struct {
    uint32_t magic, version, size, rom_crc;

    // CPU
    uint8_t  a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t  ime, ime_delay, halted, halt_bug, stopped;

    // Memory (CGB: 2 VRAM banks, 8 WRAM banks)
    uint8_t cgb, double_speed, key1, vbk, svbk;
    uint8_t vram[0x4000];
    uint8_t wram[0x8000];
    uint8_t oam[0xA0];
    uint8_t hram[0x80];
    uint8_t io[0x80];
    uint8_t ie;
    uint8_t eram[0x20000];

    // Cartridge controller
    uint8_t  ram_enable, mbc1_mode, bank1, bank2, ram_bank;
    uint16_t rom_bank;
    uint32_t rom_off0, rom_off1;

    // MBC3 clock: S M H DL DH
    uint8_t  rtc[5], rtc_latched[5], rtc_latch_prev;
    uint32_t rtc_cycles;

    // Timer
    uint16_t div;
    uint8_t  tima_overflow;

    // PPU
    uint16_t dot;
    uint8_t  mode, stat_line, win_line, win_trig, frame_done;
    uint16_t fb[GB_WIDTH * GB_HEIGHT];   // DMG: shade 0-3, CGB: RGB555
    uint8_t  bgpal[64], objpal[64], bcps, ocps;

    // CGB VRAM DMA
    uint16_t hdma_src, hdma_dst;
    uint8_t  hdma_len, hdma_active;

    // OAM DMA
    uint8_t  dma_active, dma_idx, dma_delay;
    uint16_t dma_src;

    // Serial
    uint16_t serial_cycles;

    // APU
    Chan     ch[4];
    uint8_t  fs_step;

    uint64_t cycles;
} State;

struct GB {
    State s;

    uint8_t *rom;
    size_t   rom_size;
    uint32_t rom_crc;
    uint8_t  mbc, has_battery, has_rtc, is_cgb;
    uint32_t ram_size;
    char     title[17];
    int      sram_dirty;

    uint8_t  buttons;
    int      pending_stall;
    int      color_correct;
    uint32_t palette[4];
    uint32_t rgba[GB_WIDTH * GB_HEIGHT];
    uint32_t cgb_lut[32768];

    // Audio output (not part of save state)
    int      audio_enabled;
    double   sample_rate;
    double   sample_phase;
    float    acc_l, acc_r;
    int      acc_n;
    float    hp_l, hp_r, hp_factor;
    int16_t  ring[16384 * 2];
    _Atomic size_t ring_w, ring_r;

    char     serial[4096];
    size_t   serial_len;
};

#define RING_FRAMES 16384

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void update_banks(GB *g) {
    State *s = &g->s;
    uint32_t nb = (uint32_t)(g->rom_size / 0x4000);
    if (nb == 0) nb = 1;
    uint32_t b0 = 0, b1 = 1;
    switch (g->mbc) {
    case MBC_1: {
        uint32_t lo = s->bank1 & 0x1F; if (lo == 0) lo = 1;
        b1 = ((uint32_t)s->bank2 << 5) | lo;
        b0 = s->mbc1_mode ? ((uint32_t)s->bank2 << 5) : 0;
        s->ram_bank = s->mbc1_mode ? s->bank2 : 0;
        break;
    }
    case MBC_2: b1 = s->rom_bank & 0x0F; if (b1 == 0) b1 = 1; break;
    case MBC_3: b1 = s->rom_bank & 0x7F; if (b1 == 0) b1 = 1; break;
    case MBC_5: b1 = s->rom_bank & 0x1FF; break;
    default: b1 = 1; break;
    }
    s->rom_off0 = (b0 % nb) * 0x4000;
    s->rom_off1 = (b1 % nb) * 0x4000;
}

/* ------------------------------------------------------------------ */
/* Real-time clock (MBC3)                                              */
/* ------------------------------------------------------------------ */

static void rtc_add_seconds(State *s, int64_t secs) {
    if (secs <= 0) return;
    if (s->rtc[4] & 0x40) return; // halted
    uint64_t days = (uint64_t)(s->rtc[3] | ((s->rtc[4] & 1) << 8));
    uint64_t total = (uint64_t)(s->rtc[0] % 60) + (uint64_t)(s->rtc[1] % 60) * 60 +
                     (uint64_t)(s->rtc[2] % 24) * 3600 + days * 86400 + (uint64_t)secs;
    s->rtc[0] = (uint8_t)(total % 60); total /= 60;
    s->rtc[1] = (uint8_t)(total % 60); total /= 60;
    s->rtc[2] = (uint8_t)(total % 24); total /= 24;
    if (total > 511) { s->rtc[4] |= 0x80; total %= 512; }  // day overflow flag
    s->rtc[3] = (uint8_t)(total & 0xFF);
    s->rtc[4] = (uint8_t)((s->rtc[4] & 0xFE) | ((total >> 8) & 1));
}

static void rtc_tick(GB *g, int clocks) {
    State *s = &g->s;
    if (!g->has_rtc || (s->rtc[4] & 0x40)) return;
    s->rtc_cycles += (uint32_t)clocks;
    if (s->rtc_cycles >= GB_CLOCK) {
        s->rtc_cycles -= GB_CLOCK;
        // tick seconds with hardware wrap behaviour for out-of-range values
        s->rtc[0] = (s->rtc[0] + 1) & 0x3F;
        if (s->rtc[0] == 60) {
            s->rtc[0] = 0; s->rtc[1] = (s->rtc[1] + 1) & 0x3F;
            if (s->rtc[1] == 60) {
                s->rtc[1] = 0; s->rtc[2] = (s->rtc[2] + 1) & 0x1F;
                if (s->rtc[2] == 24) {
                    s->rtc[2] = 0;
                    uint16_t d = (uint16_t)(s->rtc[3] | ((s->rtc[4] & 1) << 8));
                    d++;
                    if (d > 511) { d = 0; s->rtc[4] |= 0x80; }
                    s->rtc[3] = d & 0xFF;
                    s->rtc[4] = (uint8_t)((s->rtc[4] & 0xFE) | (d >> 8));
                }
            }
        }
    }
}

void gb_rtc_advance(GB *g, int64_t seconds) {
    if (g->has_rtc) rtc_add_seconds(&g->s, seconds);
}

/* ------------------------------------------------------------------ */
/* APU                                                                 */
/* ------------------------------------------------------------------ */

static const uint8_t DUTY[4][8] = {
    {0,0,0,0,0,0,0,1}, {1,0,0,0,0,0,0,1}, {1,0,0,0,0,1,1,1}, {0,1,1,1,1,1,1,0}
};

static inline uint16_t ch_freq(State *s, int ch) {
    uint8_t base = (uint8_t)(0x10 + ch * 5);
    return (uint16_t)(s->io[base + 3] | ((s->io[base + 4] & 7) << 8));
}

static inline int dac_on(State *s, int ch) {
    if (ch == 2) return (s->io[0x1A] & 0x80) != 0;
    return (s->io[0x10 + ch * 5 + 2] & 0xF8) != 0;
}

static uint16_t sweep_calc(State *s) {
    Chan *c = &s->ch[0];
    uint8_t shift = s->io[0x10] & 7;
    uint16_t delta = c->shadow >> shift;
    uint16_t nf;
    if (s->io[0x10] & 8) { nf = (uint16_t)(c->shadow - delta); c->neg_used = 1; }
    else nf = (uint16_t)(c->shadow + delta);
    if (nf > 2047) c->on = 0;
    return nf;
}

static void noise_period(State *s, Chan *c) {
    uint8_t r = s->io[0x22] & 7, shift = s->io[0x22] >> 4;
    int32_t div = r ? r * 16 : 8;
    c->timer += div << shift;
}

static void apu_trigger(GB *g, int n) {
    State *s = &g->s;
    Chan *c = &s->ch[n];
    uint8_t base = (uint8_t)(0x10 + n * 5);
    c->on = (uint8_t)dac_on(s, n);
    if (c->length == 0) {
        c->length = (n == 2) ? 256 : 64;
        // trigger in the first half of a length period with length enabled: one less
        if ((s->io[base + 4] & 0x40) && (s->fs_step & 1)) c->length--;
    }
    if (n == 2) {
        c->timer = (2048 - ch_freq(s, 2)) * 2 + 6;
        c->pos = 0;
    } else if (n == 3) {
        c->timer = 0; noise_period(s, c);
        c->lfsr = 0x7FFF;
    } else {
        c->timer = (2048 - ch_freq(s, n)) * 4;
    }
    if (n != 2) {
        c->vol = s->io[base + 2] >> 4;
        c->env_timer = s->io[base + 2] & 7;
        if (c->env_timer == 0) c->env_timer = 8;
    }
    if (n == 0) {
        uint8_t period = (s->io[0x10] >> 4) & 7, shift = s->io[0x10] & 7;
        c->shadow = ch_freq(s, 0);
        c->sweep_timer = period ? period : 8;
        c->sweep_enabled = (period || shift);
        c->neg_used = 0;
        if (shift) sweep_calc(s);
    }
}

static void apu_frame_step(GB *g) {
    State *s = &g->s;
    if (!(s->io[0x26] & 0x80)) return;
    uint8_t step = s->fs_step;
    s->fs_step = (s->fs_step + 1) & 7;
    if (!(step & 1)) {
        for (int n = 0; n < 4; n++) {
            Chan *c = &s->ch[n];
            if ((s->io[0x10 + n * 5 + 4] & 0x40) && c->length > 0) {
                if (--c->length == 0) c->on = 0;
            }
        }
    }
    if (step == 2 || step == 6) {
        Chan *c = &s->ch[0];
        if (--c->sweep_timer == 0) {
            uint8_t period = (s->io[0x10] >> 4) & 7;
            c->sweep_timer = period ? period : 8;
            if (c->sweep_enabled && period) {
                uint16_t nf = sweep_calc(s);
                if (nf <= 2047 && (s->io[0x10] & 7)) {
                    c->shadow = nf;
                    s->io[0x13] = nf & 0xFF;
                    s->io[0x14] = (uint8_t)((s->io[0x14] & 0xF8) | (nf >> 8));
                    sweep_calc(s);
                }
            }
        }
    }
    if (step == 7) {
        for (int n = 0; n < 4; n++) {
            if (n == 2) continue;
            Chan *c = &s->ch[n];
            uint8_t env = s->io[0x10 + n * 5 + 2];
            uint8_t period = env & 7;
            if (!period) continue;
            if (--c->env_timer == 0) {
                c->env_timer = period;
                if ((env & 8) && c->vol < 15) c->vol++;
                else if (!(env & 8) && c->vol > 0) c->vol--;
            }
        }
    }
}

static void audio_push(GB *g, int16_t l, int16_t r) {
    size_t w = atomic_load_explicit(&g->ring_w, memory_order_relaxed);
    size_t rd = atomic_load_explicit(&g->ring_r, memory_order_acquire);
    if (w - rd >= RING_FRAMES) return; // full: drop
    size_t i = (w % RING_FRAMES) * 2;
    g->ring[i] = l; g->ring[i + 1] = r;
    atomic_store_explicit(&g->ring_w, w + 1, memory_order_release);
}

static void apu_tick(GB *g, int clocks) {
    State *s = &g->s;
    int powered = (s->io[0x26] & 0x80) != 0;
    float out[4] = {0, 0, 0, 0};

    if (powered) {
        for (int n = 0; n < 2; n++) {
            Chan *c = &s->ch[n];
            c->timer -= clocks;
            while (c->timer <= 0) {
                c->timer += (2048 - ch_freq(s, n)) * 4;
                c->pos = (c->pos + 1) & 7;
            }
            if (c->on && dac_on(s, n))
                out[n] = DUTY[s->io[0x11 + n * 5] >> 6][c->pos] ? c->vol / 15.0f : 0.0f;
        }
        {
            Chan *c = &s->ch[2];
            c->timer -= clocks;
            while (c->timer <= 0) {
                c->timer += (2048 - ch_freq(s, 2)) * 2;
                c->pos = (c->pos + 1) & 31;
                uint8_t byte = s->io[0x30 + (c->pos >> 1)];
                c->sample = (c->pos & 1) ? (byte & 0xF) : (byte >> 4);
            }
            if (c->on && dac_on(s, 2)) {
                static const uint8_t shifts[4] = {4, 0, 1, 2};
                out[2] = (c->sample >> shifts[(s->io[0x1C] >> 5) & 3]) / 15.0f;
            }
        }
        {
            Chan *c = &s->ch[3];
            c->timer -= clocks;
            while (c->timer <= 0) {
                noise_period(s, c);
                uint16_t x = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                c->lfsr = (uint16_t)((c->lfsr >> 1) | (x << 14));
                if (s->io[0x22] & 8) c->lfsr = (uint16_t)((c->lfsr & ~0x40) | (x << 6));
            }
            if (c->on && dac_on(s, 3))
                out[3] = (~c->lfsr & 1) ? c->vol / 15.0f : 0.0f;
        }
    }

    if (!g->audio_enabled) return;

    float l = 0, r = 0;
    uint8_t nr51 = s->io[0x25];
    for (int n = 0; n < 4; n++) {
        if (nr51 & (0x10 << n)) l += out[n];
        if (nr51 & (0x01 << n)) r += out[n];
    }
    l *= (((s->io[0x24] >> 4) & 7) + 1) / 8.0f;
    r *= ((s->io[0x24] & 7) + 1) / 8.0f;
    g->acc_l += l; g->acc_r += r; g->acc_n++;

    g->sample_phase += clocks * g->sample_rate;
    if (g->sample_phase >= GB_CLOCK) {
        g->sample_phase -= GB_CLOCK;
        float ml = g->acc_l / g->acc_n / 4.0f, mr = g->acc_r / g->acc_n / 4.0f;
        g->acc_l = g->acc_r = 0; g->acc_n = 0;
        // DC-blocking high-pass, like the capacitor on real hardware
        float ol = ml - g->hp_l, orr = mr - g->hp_r;
        g->hp_l = ml - ol * g->hp_factor;
        g->hp_r = mr - orr * g->hp_factor;
        int il = (int)(ol * 30000.0f), ir = (int)(orr * 30000.0f);
        il = il > 32767 ? 32767 : (il < -32768 ? -32768 : il);
        ir = ir > 32767 ? 32767 : (ir < -32768 ? -32768 : ir);
        audio_push(g, (int16_t)il, (int16_t)ir);
    }
}

static uint8_t apu_read(GB *g, uint8_t r) {
    static const uint8_t mask[0x30] = {
        0x80,0x3F,0x00,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF,
        0x00,0x00,0x70, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    };
    State *s = &g->s;
    if (r >= 0x30) return s->io[r];
    if (r == 0x26) {
        uint8_t v = (s->io[0x26] & 0x80) | 0x70;
        for (int n = 0; n < 4; n++) if (s->ch[n].on) v |= (uint8_t)(1 << n);
        return v;
    }
    return s->io[r] | mask[r - 0x10];
}

static void apu_write(GB *g, uint8_t r, uint8_t v) {
    State *s = &g->s;
    if (r >= 0x30) { s->io[r] = v; return; }
    if (r == 0x26) {
        if (!(v & 0x80)) {
            for (int i = 0x10; i < 0x26; i++) s->io[i] = 0;
            for (int n = 0; n < 4; n++) { s->ch[n].on = 0; if (s->cgb) s->ch[n].length = 0; }
        } else if (!(s->io[0x26] & 0x80)) {
            s->fs_step = 0;
            for (int n = 0; n < 4; n++) s->ch[n].pos = 0;
        }
        s->io[0x26] = v & 0x80;
        return;
    }
    if (!(s->io[0x26] & 0x80)) {
        // DMG: length counters stay writable while powered off (not on CGB)
        if (s->cgb) return;
        if (r == 0x11) s->ch[0].length = 64 - (v & 0x3F);
        else if (r == 0x16) s->ch[1].length = 64 - (v & 0x3F);
        else if (r == 0x1B) s->ch[2].length = 256 - v;
        else if (r == 0x20) s->ch[3].length = 64 - (v & 0x3F);
        return;
    }
    if (r == 0x14 || r == 0x19 || r == 0x1E || r == 0x23) {
        int n = (r - 0x14) / 5;
        Chan *c = &s->ch[n];
        int was_en = (s->io[r] & 0x40) != 0;
        s->io[r] = v;
        // enabling length in the first half of a length period clocks it once
        if (!was_en && (v & 0x40) && (s->fs_step & 1) && c->length > 0) {
            if (--c->length == 0 && !(v & 0x80)) c->on = 0;
        }
        if (v & 0x80) apu_trigger(g, n);
        return;
    }
    uint8_t prev = s->io[r];
    s->io[r] = v;
    switch (r) {
    case 0x10:
        // leaving negate mode after a negate calculation disables channel 1
        if ((prev & 8) && !(v & 8) && s->ch[0].neg_used) s->ch[0].on = 0;
        break;
    case 0x11: s->ch[0].length = 64 - (v & 0x3F); break;
    case 0x16: s->ch[1].length = 64 - (v & 0x3F); break;
    case 0x1B: s->ch[2].length = 256 - v; break;
    case 0x20: s->ch[3].length = 64 - (v & 0x3F); break;
    case 0x12: if (!(v & 0xF8)) s->ch[0].on = 0; break;
    case 0x17: if (!(v & 0xF8)) s->ch[1].on = 0; break;
    case 0x21: if (!(v & 0xF8)) s->ch[3].on = 0; break;
    case 0x1A: if (!(v & 0x80)) s->ch[2].on = 0; break;
    }
}

/* ------------------------------------------------------------------ */
/* PPU                                                                 */
/* ------------------------------------------------------------------ */

static void stat_update(GB *g) {
    State *s = &g->s;
    uint8_t stat = s->io[0x41];
    int line = 0;
    if (s->io[0x40] & 0x80) {
        if ((stat & 0x40) && s->io[0x44] == s->io[0x45]) line = 1;
        if ((stat & 0x08) && s->mode == 0) line = 1;
        if ((stat & 0x10) && s->mode == 1) line = 1;
        if ((stat & 0x20) && s->mode == 2) line = 1;
    }
    if (line && !s->stat_line) s->io[0x0F] |= 2;
    s->stat_line = (uint8_t)line;
}

static inline uint16_t tile_addr(uint8_t tile, int unsigned_mode) {
    return unsigned_mode ? (uint16_t)(tile * 16) : (uint16_t)(0x1000 + (int8_t)tile * 16);
}

static inline uint16_t cgb_color(const uint8_t *pal, int palette, int ci) {
    int i = palette * 8 + ci * 2;
    return (uint16_t)((pal[i] | (pal[i + 1] << 8)) & 0x7FFF);
}

// Draws one background or window pixel run into the line buffers.
static void draw_tiles(GB *g, uint16_t *line, uint8_t *bgidx, uint8_t *bgprio,
                       uint16_t map, int x0, int scroll_x, uint8_t y) {
    State *s = &g->s;
    uint8_t lcdc = s->io[0x40], bgp = s->io[0x47];
    for (int x = x0; x < GB_WIDTH; x++) {
        uint8_t px = (uint8_t)(x + scroll_x);
        uint16_t mi = (uint16_t)(map + (y >> 3) * 32 + (px >> 3));
        uint8_t tile = s->vram[mi];
        uint8_t attr = s->cgb ? s->vram[0x2000 + mi] : 0;
        int row = (attr & 0x40) ? 7 - (y & 7) : (y & 7);
        uint16_t a = (uint16_t)(tile_addr(tile, lcdc & 0x10) + row * 2 + ((attr & 0x08) ? 0x2000 : 0));
        int bit = (attr & 0x20) ? (px & 7) : 7 - (px & 7);
        uint8_t ci = (uint8_t)((((s->vram[a + 1] >> bit) & 1) << 1) | ((s->vram[a] >> bit) & 1));
        bgidx[x] = ci;
        bgprio[x] = attr & 0x80;
        line[x] = s->cgb ? cgb_color(s->bgpal, attr & 7, ci) : (uint16_t)((bgp >> (ci * 2)) & 3);
    }
}

static void render_line(GB *g) {
    State *s = &g->s;
    uint8_t ly = s->io[0x44], lcdc = s->io[0x40];
    uint16_t *line = &s->fb[ly * GB_WIDTH];
    uint8_t bgidx[GB_WIDTH], bgprio[GB_WIDTH];

    // On CGB, LCDC bit 0 removes BG priority instead of hiding the BG.
    if ((lcdc & 1) || s->cgb) {
        draw_tiles(g, line, bgidx, bgprio, (lcdc & 0x08) ? 0x1C00 : 0x1800, 0,
                   s->io[0x43], (uint8_t)(ly + s->io[0x42]));
        int wx = s->io[0x4B] - 7;
        if ((lcdc & 0x20) && s->win_trig && s->io[0x4B] <= 166) {
            draw_tiles(g, line, bgidx, bgprio, (lcdc & 0x40) ? 0x1C00 : 0x1800,
                       wx < 0 ? 0 : wx, -wx, s->win_line);
            s->win_line++;
        }
    } else {
        memset(bgidx, 0, sizeof bgidx);
        memset(bgprio, 0, sizeof bgprio);
        for (int x = 0; x < GB_WIDTH; x++) line[x] = 0;
    }

    if (lcdc & 0x02) {
        int h = (lcdc & 0x04) ? 16 : 8;
        int list[10], n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int sy = s->oam[i * 4] - 16;
            if (ly >= sy && ly < sy + h) list[n++] = i;
        }
        // DMG priority: lower X first, then OAM index. CGB: OAM index only.
        if (!s->cgb) {
            for (int i = 1; i < n; i++) {
                int v = list[i], j = i - 1;
                while (j >= 0 && s->oam[list[j] * 4 + 1] > s->oam[v * 4 + 1]) { list[j + 1] = list[j]; j--; }
                list[j + 1] = v;
            }
        }
        uint8_t claimed[GB_WIDTH];
        memset(claimed, 0, sizeof claimed);
        int master = s->cgb ? (lcdc & 1) : 1;
        for (int k = 0; k < n; k++) {
            const uint8_t *o = &s->oam[list[k] * 4];
            int sy = o[0] - 16, sx = o[1] - 8;
            uint8_t tile = o[2], attr = o[3];
            int row = ly - sy;
            if (attr & 0x40) row = h - 1 - row;
            if (h == 16) tile &= 0xFE;
            uint16_t a = (uint16_t)(tile * 16 + row * 2 + ((s->cgb && (attr & 0x08)) ? 0x2000 : 0));
            uint8_t lo = s->vram[a], hi = s->vram[a + 1];
            uint8_t pal = (attr & 0x10) ? s->io[0x49] : s->io[0x48];
            for (int p = 0; p < 8; p++) {
                int x = sx + p;
                if (x < 0 || x >= GB_WIDTH || claimed[x]) continue;
                int bit = (attr & 0x20) ? p : 7 - p;
                uint8_t ci = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
                if (!ci) continue;
                claimed[x] = 1;
                if (master && bgidx[x] && ((attr & 0x80) || bgprio[x])) continue;
                line[x] = s->cgb ? cgb_color(s->objpal, attr & 7, ci) : (uint16_t)((pal >> (ci * 2)) & 3);
            }
        }
    }
}

static uint8_t dma_source_read(GB *g, uint16_t a);

static void hdma_block(GB *g) {
    State *s = &g->s;
    for (int i = 0; i < 16; i++) {
        uint8_t v = dma_source_read(g, s->hdma_src++);
        s->vram[s->vbk * 0x2000 + ((s->hdma_dst++) & 0x1FFF)] = v;
    }
    s->hdma_dst &= 0x1FFF;
    if (--s->hdma_len == 0) s->hdma_active = 0;
}

static void ppu_tick(GB *g, int clocks) {
    State *s = &g->s;
    if (!(s->io[0x40] & 0x80)) return;
    s->dot = (uint16_t)(s->dot + clocks);
    uint8_t ly = s->io[0x44];
    if (ly < 144) {
        if (s->mode == 2 && s->dot >= 80) s->mode = 3;
        else if (s->mode == 3 && s->dot >= 252) {
            render_line(g);
            s->mode = 0;
            if (s->hdma_active && !s->halted) hdma_block(g);
        }
    }
    if (s->dot >= 456) {
        s->dot -= 456;
        ly++;
        if (ly == 154) { ly = 0; s->win_line = 0; s->win_trig = 0; }
        s->io[0x44] = ly;
        if (ly == 144) {
            s->mode = 1;
            s->io[0x0F] |= 1;
            s->frame_done = 1;
        } else if (ly < 144) {
            s->mode = 2;
            if (ly == s->io[0x4A]) s->win_trig = 1;
        }
    }
    stat_update(g);
}

/* ------------------------------------------------------------------ */
/* Memory bus                                                          */
/* ------------------------------------------------------------------ */

static uint8_t joypad_read(GB *g) {
    uint8_t p1 = g->s.io[0x00], v = 0x0F;
    uint8_t b = g->buttons;
    if (!(p1 & 0x10)) v &= (uint8_t)~((b >> 4) & 0x0F);
    if (!(p1 & 0x20)) v &= (uint8_t)~(b & 0x0F);
    return (uint8_t)(0xC0 | (p1 & 0x30) | v);
}

static uint8_t io_read(GB *g, uint8_t r) {
    State *s = &g->s;
    switch (r) {
    case 0x00: return joypad_read(g);
    case 0x01: return s->io[0x01];
    case 0x02: return s->io[0x02] | 0x7E;
    case 0x04: return (uint8_t)(s->div >> 8);
    case 0x05: case 0x06: return s->io[r];
    case 0x07: return s->io[0x07] | 0xF8;
    case 0x0F: return s->io[0x0F] | 0xE0;
    case 0x40: return s->io[0x40];
    case 0x41: {
        uint8_t mode = (s->io[0x40] & 0x80) ? s->mode : 0;
        uint8_t coinc = (s->io[0x44] == s->io[0x45]) ? 4 : 0;
        return (uint8_t)(0x80 | (s->io[0x41] & 0x78) | coinc | mode);
    }
    case 0x42: case 0x43: case 0x44: case 0x45: case 0x46:
    case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        return s->io[r];
    }
    if (r >= 0x10 && r < 0x40) return apu_read(g, r);
    if (s->cgb) {
        switch (r) {
        case 0x4D: return (uint8_t)(0x7E | (s->double_speed ? 0x80 : 0) | (s->key1 & 1));
        case 0x4F: return (uint8_t)(0xFE | s->vbk);
        case 0x55: return s->hdma_active ? (uint8_t)((s->hdma_len - 1) & 0x7F) : 0xFF;
        case 0x68: return (uint8_t)(s->bcps | 0x40);
        case 0x69: return s->bgpal[s->bcps & 0x3F];
        case 0x6A: return (uint8_t)(s->ocps | 0x40);
        case 0x6B: return s->objpal[s->ocps & 0x3F];
        case 0x70: return (uint8_t)(0xF8 | s->svbk);
        }
    }
    return 0xFF;
}

static void timer_inc(State *s) {
    if (++s->io[0x05] == 0) s->tima_overflow = 1;
}

static const uint16_t TAC_BIT[4] = {0x200, 0x8, 0x20, 0x80};

static void io_write(GB *g, uint8_t r, uint8_t v) {
    State *s = &g->s;
    switch (r) {
    case 0x00: s->io[0x00] = v & 0x30; return;
    case 0x01: s->io[0x01] = v; return;
    case 0x02:
        s->io[0x02] = v & 0x81;
        if ((v & 0x81) == 0x81) {
            s->serial_cycles = 4096;
            if (g->serial_len < sizeof g->serial - 1) {
                g->serial[g->serial_len++] = (char)s->io[0x01];
                g->serial[g->serial_len] = 0;
            }
        }
        return;
    case 0x04: {
        uint16_t old = s->div;
        s->div = 0;
        if ((s->io[0x07] & 4) && (old & TAC_BIT[s->io[0x07] & 3])) timer_inc(s);
        if (old & (s->double_speed ? 0x2000 : 0x1000)) apu_frame_step(g);
        return;
    }
    case 0x05:
        s->io[0x05] = v; s->tima_overflow = 0; return;
    case 0x06: s->io[0x06] = v; return;
    case 0x07: {
        int old = (s->io[0x07] & 4) && (s->div & TAC_BIT[s->io[0x07] & 3]);
        s->io[0x07] = v & 7;
        int nw = (v & 4) && (s->div & TAC_BIT[v & 3]);
        if (old && !nw) timer_inc(s);
        return;
    }
    case 0x0F: s->io[0x0F] = v & 0x1F; return;
    case 0x40: {
        uint8_t was = s->io[0x40] & 0x80;
        s->io[0x40] = v;
        if (was && !(v & 0x80)) {
            s->io[0x44] = 0; s->dot = 0; s->mode = 0;
            s->win_line = 0; s->win_trig = 0;
            for (int i = 0; i < GB_WIDTH * GB_HEIGHT; i++) s->fb[i] = s->cgb ? 0x7FFF : 0;
        } else if (!was && (v & 0x80)) {
            s->dot = 4; s->mode = 2;
            s->win_line = 0;
            s->win_trig = (s->io[0x4A] == 0);
        }
        stat_update(g);
        return;
    }
    case 0x41: s->io[0x41] = v & 0x78; stat_update(g); return;
    case 0x44: return;
    case 0x45: s->io[0x45] = v; stat_update(g); return;
    case 0x46:
        s->io[0x46] = v;
        s->dma_src = (uint16_t)(v << 8);
        s->dma_idx = 0;
        s->dma_delay = 1;
        s->dma_active = 1;
        return;
    case 0x42: case 0x43: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        s->io[r] = v; return;
    }
    if (s->cgb) {
        switch (r) {
        case 0x4D: s->key1 = v & 1; return;
        case 0x4F: s->vbk = v & 1; return;
        case 0x51: s->hdma_src = (uint16_t)((s->hdma_src & 0x00FF) | (v << 8)); return;
        case 0x52: s->hdma_src = (uint16_t)((s->hdma_src & 0xFF00) | (v & 0xF0)); return;
        case 0x53: s->hdma_dst = (uint16_t)((s->hdma_dst & 0x00FF) | ((v & 0x1F) << 8)); return;
        case 0x54: s->hdma_dst = (uint16_t)((s->hdma_dst & 0xFF00) | (v & 0xF0)); return;
        case 0x55:
            if (s->hdma_active && !(v & 0x80)) { s->hdma_active = 0; return; }  // cancel HBlank DMA
            s->hdma_len = (uint8_t)((v & 0x7F) + 1);
            if (v & 0x80) {
                s->hdma_active = 1;
                // if the LCD is off or already in HBlank, the first block goes now
                if (!(s->io[0x40] & 0x80) || s->mode == 0) hdma_block(g);
            } else {
                while (s->hdma_len) { hdma_block(g); s->hdma_active = 0; g->pending_stall += s->double_speed ? 16 : 8; }
            }
            return;
        case 0x68: s->bcps = v & 0xBF; return;
        case 0x69:
            s->bgpal[s->bcps & 0x3F] = v;
            if (s->bcps & 0x80) s->bcps = (uint8_t)(0x80 | ((s->bcps + 1) & 0x3F));
            return;
        case 0x6A: s->ocps = v & 0xBF; return;
        case 0x6B:
            s->objpal[s->ocps & 0x3F] = v;
            if (s->ocps & 0x80) s->ocps = (uint8_t)(0x80 | ((s->ocps + 1) & 0x3F));
            return;
        case 0x70: s->svbk = v & 7; return;
        }
    }
    if (r >= 0x10 && r < 0x40) apu_write(g, r, v);
}

static uint8_t eram_read(GB *g, uint16_t a) {
    State *s = &g->s;
    if (!s->ram_enable) return 0xFF;
    if (g->mbc == MBC_2) return (uint8_t)(0xF0 | s->eram[a & 0x1FF]);
    if (g->mbc == MBC_3 && s->ram_bank >= 0x08 && s->ram_bank <= 0x0C) {
        static const uint8_t m[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
        return g->has_rtc ? (s->rtc_latched[s->ram_bank - 8] & m[s->ram_bank - 8]) : 0xFF;
    }
    if (!g->ram_size) return 0xFF;
    uint32_t off = (uint32_t)(s->ram_bank & 0x0F) * 0x2000 + (a - 0xA000);
    return s->eram[off % g->ram_size];
}

static void eram_write(GB *g, uint16_t a, uint8_t v) {
    State *s = &g->s;
    if (!s->ram_enable) return;
    if (g->mbc == MBC_2) { s->eram[a & 0x1FF] = v & 0x0F; g->sram_dirty = 1; return; }
    if (g->mbc == MBC_3 && s->ram_bank >= 0x08 && s->ram_bank <= 0x0C) {
        if (!g->has_rtc) return;
        int i = s->ram_bank - 8;
        static const uint8_t m[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
        s->rtc[i] = v & m[i];
        s->rtc_latched[i] = v & m[i];
        if (i == 0) s->rtc_cycles = 0;
        g->sram_dirty = 1;
        return;
    }
    if (!g->ram_size) return;
    uint32_t off = (uint32_t)(s->ram_bank & 0x0F) * 0x2000 + (a - 0xA000);
    s->eram[off % g->ram_size] = v;
    g->sram_dirty = 1;
}

static void mbc_write(GB *g, uint16_t a, uint8_t v) {
    State *s = &g->s;
    switch (g->mbc) {
    case MBC_NONE: return;
    case MBC_1:
        if (a < 0x2000) s->ram_enable = (v & 0x0F) == 0x0A;
        else if (a < 0x4000) s->bank1 = v & 0x1F;
        else if (a < 0x6000) s->bank2 = v & 3;
        else s->mbc1_mode = v & 1;
        break;
    case MBC_2:
        if (a < 0x4000) {
            if (a & 0x100) s->rom_bank = v & 0x0F;
            else s->ram_enable = (v & 0x0F) == 0x0A;
        }
        break;
    case MBC_3:
        if (a < 0x2000) s->ram_enable = (v & 0x0F) == 0x0A;
        else if (a < 0x4000) s->rom_bank = v & 0x7F;
        else if (a < 0x6000) s->ram_bank = v & 0x0F;
        else {
            if (s->rtc_latch_prev == 0 && v == 1) memcpy(s->rtc_latched, s->rtc, 5);
            s->rtc_latch_prev = v;
        }
        break;
    case MBC_5:
        if (a < 0x2000) s->ram_enable = (v & 0x0F) == 0x0A;
        else if (a < 0x3000) s->rom_bank = (uint16_t)((s->rom_bank & 0x100) | v);
        else if (a < 0x4000) s->rom_bank = (uint16_t)((s->rom_bank & 0xFF) | ((v & 1) << 8));
        else if (a < 0x6000) s->ram_bank = v & 0x0F;
        break;
    }
    update_banks(g);
}

static inline uint32_t wram_off(State *s, uint16_t a) {
    a &= 0x1FFF;                       // C000-DFFF and echo E000-FDFF
    if (a < 0x1000) return a;
    uint32_t bank = s->cgb ? (s->svbk ? s->svbk : 1) : 1;
    return bank * 0x1000 + (a - 0x1000);
}
static inline uint32_t vram_off(State *s, uint16_t a) {
    return (uint32_t)(s->vbk * 0x2000) + (a & 0x1FFF);
}

static uint8_t bus_read(GB *g, uint16_t a) {
    State *s = &g->s;
    if (a < 0x4000) return g->rom[(s->rom_off0 + a) % g->rom_size];
    if (a < 0x8000) return g->rom[(s->rom_off1 + (a - 0x4000)) % g->rom_size];
    if (a < 0xA000) return s->vram[vram_off(s, a)];
    if (a < 0xC000) return eram_read(g, a);
    if (a < 0xFE00) return s->wram[wram_off(s, a)];
    if (a < 0xFEA0) return s->dma_active && !s->dma_delay ? 0xFF : s->oam[a - 0xFE00];
    if (a < 0xFF00) return 0x00;
    if (a < 0xFF80) return io_read(g, (uint8_t)(a - 0xFF00));
    if (a < 0xFFFF) return s->hram[a - 0xFF80];
    return s->ie;
}

static void bus_write(GB *g, uint16_t a, uint8_t v) {
    State *s = &g->s;
    if (a < 0x8000) { mbc_write(g, a, v); return; }
    if (a < 0xA000) { s->vram[vram_off(s, a)] = v; return; }
    if (a < 0xC000) { eram_write(g, a, v); return; }
    if (a < 0xFE00) { s->wram[wram_off(s, a)] = v; return; }
    if (a < 0xFEA0) { if (!(s->dma_active && !s->dma_delay)) s->oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00) return;
    if (a < 0xFF80) { io_write(g, (uint8_t)(a - 0xFF00), v); return; }
    if (a < 0xFFFF) { s->hram[a - 0xFF80] = v; return; }
    s->ie = v;
}

static uint8_t dma_source_read(GB *g, uint16_t a) {
    if (a >= 0xE000) a = (uint16_t)(a - 0x2000);  // FE/FF pages mirror WRAM on DMG
    if (a < 0x4000) return g->rom[(g->s.rom_off0 + a) % g->rom_size];
    if (a < 0x8000) return g->rom[(g->s.rom_off1 + (a - 0x4000)) % g->rom_size];
    if (a < 0xA000) return g->s.vram[vram_off(&g->s, a)];
    if (a < 0xC000) return eram_read(g, a);
    return g->s.wram[wram_off(&g->s, a)];
}

/* One CPU machine cycle of everything except the CPU. */
static void tick(GB *g) {
    State *s = &g->s;

    // Timer
    if (s->tima_overflow) {
        s->tima_overflow = 0;
        s->io[0x05] = s->io[0x06];
        s->io[0x0F] |= 4;
    }
    uint16_t old = s->div;
    s->div = (uint16_t)(s->div + 4);
    if ((s->io[0x07] & 4)) {
        uint16_t bit = TAC_BIT[s->io[0x07] & 3];
        if ((old & bit) && !(s->div & bit)) timer_inc(s);
    }
    uint16_t fs_bit = s->double_speed ? 0x2000 : 0x1000;
    if ((old & fs_bit) && !(s->div & fs_bit)) apu_frame_step(g);

    // Serial (no link partner: receive 0xFF)
    if (s->serial_cycles) {
        s->serial_cycles = (uint16_t)(s->serial_cycles > 4 ? s->serial_cycles - 4 : 0);
        if (!s->serial_cycles) {
            s->io[0x01] = 0xFF;
            s->io[0x02] &= 0x7F;
            s->io[0x0F] |= 8;
        }
    }

    // OAM DMA
    if (s->dma_active) {
        if (s->dma_delay) s->dma_delay--;
        else {
            s->oam[s->dma_idx] = dma_source_read(g, (uint16_t)(s->dma_src + s->dma_idx));
            if (++s->dma_idx >= 0xA0) s->dma_active = 0;
        }
    }

    // In double-speed mode the CPU (and timer) run twice as fast as the
    // PPU and APU, so they see half as many clocks per machine cycle.
    int clocks = s->double_speed ? 2 : 4;
    ppu_tick(g, clocks);
    apu_tick(g, clocks);
    rtc_tick(g, clocks);
    s->cycles += (uint64_t)clocks;
}

/* ------------------------------------------------------------------ */
/* CPU                                                                 */
/* ------------------------------------------------------------------ */

static inline uint8_t rd(GB *g, uint16_t a) { tick(g); return bus_read(g, a); }
static inline void wr(GB *g, uint16_t a, uint8_t v) { tick(g); bus_write(g, a, v); }

static inline uint8_t fetch(GB *g) {
    uint8_t v = rd(g, g->s.pc);
    if (g->s.halt_bug) g->s.halt_bug = 0;
    else g->s.pc++;
    return v;
}
static inline uint16_t fetch16(GB *g) {
    uint8_t lo = fetch(g);
    return (uint16_t)(lo | (fetch(g) << 8));
}

#define BC ((uint16_t)((s->b << 8) | s->c))
#define DE ((uint16_t)((s->d << 8) | s->e))
#define HL ((uint16_t)((s->h << 8) | s->l))
#define SET_BC(v) do { uint16_t _v = (v); s->b = _v >> 8; s->c = _v & 0xFF; } while (0)
#define SET_DE(v) do { uint16_t _v = (v); s->d = _v >> 8; s->e = _v & 0xFF; } while (0)
#define SET_HL(v) do { uint16_t _v = (v); s->h = _v >> 8; s->l = _v & 0xFF; } while (0)

static uint8_t get_r(GB *g, int i) {
    State *s = &g->s;
    switch (i) {
    case 0: return s->b; case 1: return s->c; case 2: return s->d; case 3: return s->e;
    case 4: return s->h; case 5: return s->l; case 6: return rd(g, HL); default: return s->a;
    }
}
static void set_r(GB *g, int i, uint8_t v) {
    State *s = &g->s;
    switch (i) {
    case 0: s->b = v; break; case 1: s->c = v; break; case 2: s->d = v; break; case 3: s->e = v; break;
    case 4: s->h = v; break; case 5: s->l = v; break; case 6: wr(g, HL, v); break; default: s->a = v; break;
    }
}

static uint16_t get_rr(State *s, int i) {   // BC DE HL SP
    switch (i) { case 0: return BC; case 1: return DE; case 2: return HL; default: return s->sp; }
}
static void set_rr(State *s, int i, uint16_t v) {
    switch (i) { case 0: SET_BC(v); break; case 1: SET_DE(v); break; case 2: SET_HL(v); break; default: s->sp = v; }
}

static void push16(GB *g, uint16_t v) {
    State *s = &g->s;
    tick(g);
    s->sp--; wr(g, s->sp, v >> 8);
    s->sp--; wr(g, s->sp, v & 0xFF);
}
static uint16_t pop16(GB *g) {
    State *s = &g->s;
    uint8_t lo = rd(g, s->sp++);
    uint8_t hi = rd(g, s->sp++);
    return (uint16_t)(lo | (hi << 8));
}

static void alu(GB *g, int op, uint8_t v) {
    State *s = &g->s;
    uint8_t a = s->a, c = (s->f & FC) ? 1 : 0;
    int r;
    switch (op) {
    case 0: r = a + v;      s->f = (uint8_t)((((r & 0xFF) == 0) ? FZ : 0) | (((a & 0xF) + (v & 0xF)) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0)); s->a = (uint8_t)r; break;
    case 1: r = a + v + c;  s->f = (uint8_t)((((r & 0xFF) == 0) ? FZ : 0) | (((a & 0xF) + (v & 0xF) + c) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0)); s->a = (uint8_t)r; break;
    case 2: r = a - v;      s->f = (uint8_t)((((r & 0xFF) == 0) ? FZ : 0) | FN | ((a & 0xF) < (v & 0xF) ? FH : 0) | (r < 0 ? FC : 0)); s->a = (uint8_t)r; break;
    case 3: r = a - v - c;  s->f = (uint8_t)((((r & 0xFF) == 0) ? FZ : 0) | FN | ((a & 0xF) < (v & 0xF) + c ? FH : 0) | (r < 0 ? FC : 0)); s->a = (uint8_t)r; break;
    case 4: s->a = a & v;   s->f = (uint8_t)((s->a ? 0 : FZ) | FH); break;
    case 5: s->a = a ^ v;   s->f = s->a ? 0 : FZ; break;
    case 6: s->a = a | v;   s->f = s->a ? 0 : FZ; break;
    case 7: r = a - v;      s->f = (uint8_t)((((r & 0xFF) == 0) ? FZ : 0) | FN | ((a & 0xF) < (v & 0xF) ? FH : 0) | (r < 0 ? FC : 0)); break;
    }
}

static int cond(State *s, int cc) {
    switch (cc) {
    case 0: return !(s->f & FZ); case 1: return (s->f & FZ) != 0;
    case 2: return !(s->f & FC); default: return (s->f & FC) != 0;
    }
}

static void cb_op(GB *g) {
    State *s = &g->s;
    uint8_t op = fetch(g);
    int r = op & 7, b = (op >> 3) & 7;
    uint8_t v = get_r(g, r), res, cin = (s->f & FC) ? 1 : 0;
    switch (op >> 6) {
    case 0:
        switch (b) {
        case 0: res = (uint8_t)((v << 1) | (v >> 7)); s->f = (v & 0x80) ? FC : 0; break;     // RLC
        case 1: res = (uint8_t)((v >> 1) | (v << 7)); s->f = (v & 1) ? FC : 0; break;        // RRC
        case 2: res = (uint8_t)((v << 1) | cin); s->f = (v & 0x80) ? FC : 0; break;          // RL
        case 3: res = (uint8_t)((v >> 1) | (cin << 7)); s->f = (v & 1) ? FC : 0; break;      // RR
        case 4: res = (uint8_t)(v << 1); s->f = (v & 0x80) ? FC : 0; break;                  // SLA
        case 5: res = (uint8_t)((v >> 1) | (v & 0x80)); s->f = (v & 1) ? FC : 0; break;      // SRA
        case 6: res = (uint8_t)((v << 4) | (v >> 4)); s->f = 0; break;                       // SWAP
        default: res = v >> 1; s->f = (v & 1) ? FC : 0; break;                               // SRL
        }
        if (!res) s->f |= FZ;
        set_r(g, r, res);
        break;
    case 1: // BIT
        s->f = (uint8_t)((s->f & FC) | FH | ((v & (1 << b)) ? 0 : FZ));
        break;
    case 2: set_r(g, r, (uint8_t)(v & ~(1 << b))); break;
    case 3: set_r(g, r, (uint8_t)(v | (1 << b))); break;
    }
}

static void service_interrupts(GB *g) {
    State *s = &g->s;
    uint8_t pending = s->ie & s->io[0x0F] & 0x1F;
    if (!pending) return;
    s->halted = 0;
    if (!s->ime) return;
    s->ime = 0;
    tick(g); tick(g);
    s->sp--; wr(g, s->sp, s->pc >> 8);
    pending = s->ie & s->io[0x0F] & 0x1F;   // IE may have been overwritten by the push
    s->sp--; wr(g, s->sp, s->pc & 0xFF);
    if (!pending) { s->pc = 0; tick(g); return; }
    int bit = 0;
    while (!(pending & (1 << bit))) bit++;
    s->io[0x0F] &= (uint8_t)~(1 << bit);
    s->pc = (uint16_t)(0x40 + bit * 8);
    tick(g);
}

static void cpu_step(GB *g) {
    State *s = &g->s;

    while (g->pending_stall > 0) { g->pending_stall--; tick(g); }
    service_interrupts(g);
    if (s->halted || s->stopped) {
        tick(g);
        if (s->stopped && g->buttons) s->stopped = 0;
        return;
    }
    if (s->ime_delay) { s->ime_delay = 0; s->ime = 1; }

    uint8_t op = fetch(g);

    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) { // HALT
            if (!s->ime && (s->ie & s->io[0x0F] & 0x1F)) s->halt_bug = 1;
            else s->halted = 1;
            return;
        }
        set_r(g, (op >> 3) & 7, get_r(g, op & 7));
        return;
    }
    if (op >= 0x80 && op < 0xC0) { alu(g, (op >> 3) & 7, get_r(g, op & 7)); return; }

    switch (op) {
    case 0x00: break;
    case 0x01: case 0x11: case 0x21: case 0x31: set_rr(s, op >> 4, fetch16(g)); break;
    case 0x02: wr(g, BC, s->a); break;
    case 0x12: wr(g, DE, s->a); break;
    case 0x22: wr(g, HL, s->a); SET_HL(HL + 1); break;
    case 0x32: wr(g, HL, s->a); SET_HL(HL - 1); break;
    case 0x0A: s->a = rd(g, BC); break;
    case 0x1A: s->a = rd(g, DE); break;
    case 0x2A: s->a = rd(g, HL); SET_HL(HL + 1); break;
    case 0x3A: s->a = rd(g, HL); SET_HL(HL - 1); break;
    case 0x03: case 0x13: case 0x23: case 0x33: set_rr(s, op >> 4, (uint16_t)(get_rr(s, op >> 4) + 1)); tick(g); break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: set_rr(s, op >> 4, (uint16_t)(get_rr(s, op >> 4) - 1)); tick(g); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
        int r = (op >> 3) & 7; uint8_t v = get_r(g, r), n = (uint8_t)(v + 1);
        s->f = (uint8_t)((s->f & FC) | (n ? 0 : FZ) | ((v & 0xF) == 0xF ? FH : 0));
        set_r(g, r, n); break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
        int r = (op >> 3) & 7; uint8_t v = get_r(g, r), n = (uint8_t)(v - 1);
        s->f = (uint8_t)((s->f & FC) | FN | (n ? 0 : FZ) | ((v & 0xF) == 0 ? FH : 0));
        set_r(g, r, n); break;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        set_r(g, (op >> 3) & 7, fetch(g)); break;
    case 0x07: s->f = (s->a & 0x80) ? FC : 0; s->a = (uint8_t)((s->a << 1) | (s->a >> 7)); break;
    case 0x0F: s->f = (s->a & 1) ? FC : 0; s->a = (uint8_t)((s->a >> 1) | (s->a << 7)); break;
    case 0x17: { uint8_t c = (s->f & FC) ? 1 : 0; s->f = (s->a & 0x80) ? FC : 0; s->a = (uint8_t)((s->a << 1) | c); break; }
    case 0x1F: { uint8_t c = (s->f & FC) ? 0x80 : 0; s->f = (s->a & 1) ? FC : 0; s->a = (uint8_t)((s->a >> 1) | c); break; }
    case 0x08: { uint16_t a = fetch16(g); wr(g, a, s->sp & 0xFF); wr(g, (uint16_t)(a + 1), s->sp >> 8); break; }
    case 0x09: case 0x19: case 0x29: case 0x39: {
        uint16_t hl = HL, v = get_rr(s, op >> 4); uint32_t r = (uint32_t)hl + v;
        s->f = (uint8_t)((s->f & FZ) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0));
        SET_HL((uint16_t)r); tick(g); break;
    }
    case 0x10: // STOP (on CGB with KEY1 armed: switch CPU speed)
        fetch(g);
        s->div = 0;
        if (s->cgb && (s->key1 & 1)) {
            s->double_speed ^= 1;
            s->key1 = 0;
            for (int i = 0; i < 2050; i++) tick(g);
        } else s->stopped = 1;
        break;
    case 0x18: { int8_t e = (int8_t)fetch(g); s->pc = (uint16_t)(s->pc + e); tick(g); break; }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t e = (int8_t)fetch(g);
        if (cond(s, (op >> 3) & 3)) { s->pc = (uint16_t)(s->pc + e); tick(g); }
        break;
    }
    case 0x27: { // DAA
        int a = s->a, c = (s->f & FC) != 0;
        if (!(s->f & FN)) {
            if (c || a > 0x99) { a += 0x60; c = 1; }
            if ((s->f & FH) || (a & 0x0F) > 0x09) a += 0x06;
        } else {
            if (c) a -= 0x60;
            if (s->f & FH) a -= 0x06;
        }
        s->a = (uint8_t)a;
        s->f = (uint8_t)((s->f & FN) | (s->a ? 0 : FZ) | (c ? FC : 0));
        break;
    }
    case 0x2F: s->a = (uint8_t)~s->a; s->f |= FN | FH; break;
    case 0x37: s->f = (uint8_t)((s->f & FZ) | FC); break;
    case 0x3F: s->f = (uint8_t)((s->f & FZ) | ((s->f & FC) ? 0 : FC)); break;

    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        tick(g);
        if (cond(s, (op >> 3) & 3)) { s->pc = pop16(g); tick(g); }
        break;
    case 0xC9: s->pc = pop16(g); tick(g); break;
    case 0xD9: s->pc = pop16(g); tick(g); s->ime = 1; break;
    case 0xC1: SET_BC(pop16(g)); break;
    case 0xD1: SET_DE(pop16(g)); break;
    case 0xE1: SET_HL(pop16(g)); break;
    case 0xF1: { uint16_t v = pop16(g); s->a = v >> 8; s->f = v & 0xF0; break; }
    case 0xC5: push16(g, BC); break;
    case 0xD5: push16(g, DE); break;
    case 0xE5: push16(g, HL); break;
    case 0xF5: push16(g, (uint16_t)((s->a << 8) | s->f)); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        uint16_t a = fetch16(g);
        if (cond(s, (op >> 3) & 3)) { s->pc = a; tick(g); }
        break;
    }
    case 0xC3: s->pc = fetch16(g); tick(g); break;
    case 0xE9: s->pc = HL; break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        uint16_t a = fetch16(g);
        if (cond(s, (op >> 3) & 3)) { push16(g, s->pc); s->pc = a; }
        break;
    }
    case 0xCD: { uint16_t a = fetch16(g); push16(g, s->pc); s->pc = a; break; }
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push16(g, s->pc); s->pc = op & 0x38; break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu(g, (op >> 3) & 7, fetch(g)); break;
    case 0xCB: cb_op(g); break;
    case 0xE0: wr(g, (uint16_t)(0xFF00 | fetch(g)), s->a); break;
    case 0xF0: s->a = rd(g, (uint16_t)(0xFF00 | fetch(g))); break;
    case 0xE2: wr(g, (uint16_t)(0xFF00 | s->c), s->a); break;
    case 0xF2: s->a = rd(g, (uint16_t)(0xFF00 | s->c)); break;
    case 0xEA: wr(g, fetch16(g), s->a); break;
    case 0xFA: s->a = rd(g, fetch16(g)); break;
    case 0xE8: case 0xF8: {
        int8_t e = (int8_t)fetch(g);
        uint16_t sp = s->sp, r = (uint16_t)(sp + e);
        s->f = (uint8_t)((((sp & 0xF) + ((uint8_t)e & 0xF)) > 0xF ? FH : 0) |
                         (((sp & 0xFF) + (uint8_t)e) > 0xFF ? FC : 0));
        if (op == 0xE8) { s->sp = r; tick(g); tick(g); }
        else { SET_HL(r); tick(g); }
        break;
    }
    case 0xF9: s->sp = HL; tick(g); break;
    case 0xF3: s->ime = 0; s->ime_delay = 0; break;
    case 0xFB: s->ime_delay = 1; break;
    default:
        // Illegal opcode: hardware locks up. Emulate as a permanent halt.
        s->halted = 1; s->ie = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static const uint32_t DEFAULT_PALETTE[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};

// RGB555 -> 0xFFRRGGBB. With correction, approximates the washed-out
// colours of the real GBC screen so games look as their artists intended.
static void build_lut(GB *g) {
    for (int c = 0; c < 32768; c++) {
        int r = c & 31, gg = (c >> 5) & 31, b = (c >> 10) & 31;
        int R, G, B;
        if (g->color_correct) {
            R = r * 26 + gg * 4 + b * 2;
            G = gg * 24 + b * 8;
            B = r * 6 + gg * 4 + b * 22;
            R = (R > 960 ? 960 : R) >> 2;
            G = (G > 960 ? 960 : G) >> 2;
            B = (B > 960 ? 960 : B) >> 2;
        } else {
            R = (r << 3) | (r >> 2); G = (gg << 3) | (gg >> 2); B = (b << 3) | (b >> 2);
        }
        g->cgb_lut[c] = 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
    }
}

static void convert_frame(GB *g) {
    const uint16_t *fb = g->s.fb;
    if (g->s.cgb) for (int i = 0; i < GB_WIDTH * GB_HEIGHT; i++) g->rgba[i] = g->cgb_lut[fb[i] & 0x7FFF];
    else for (int i = 0; i < GB_WIDTH * GB_HEIGHT; i++) g->rgba[i] = 0xFF000000u | g->palette[fb[i] & 3];
}

GB *gb_create(void) {
    GB *g = (GB *)calloc(1, sizeof(GB));
    if (!g) return NULL;
    memcpy(g->palette, DEFAULT_PALETTE, sizeof g->palette);
    g->audio_enabled = 1;
    g->color_correct = 1;
    build_lut(g);
    gb_set_sample_rate(g, 48000.0);
    return g;
}

void gb_destroy(GB *g) {
    if (!g) return;
    free(g->rom);
    free(g);
}

void gb_set_sample_rate(GB *g, double rate) {
    if (rate < 8000) rate = 8000;
    g->sample_rate = rate;
    // capacitor discharge per output sample (0.999958 per clock on DMG)
    double f = 1.0;
    double n = GB_CLOCK / rate;
    double base = 0.999958;
    // pow without libm dependency
    double x = base; long k = (long)n;
    while (k) { if (k & 1) f *= x; x *= x; k >>= 1; }
    g->hp_factor = (float)f;
}

static void reset_state(GB *g, int keep_ram) {
    State *s = &g->s;
    uint8_t *eram = NULL;
    uint8_t rtc[5];
    memcpy(rtc, s->rtc, 5);
    if (keep_ram) {
        eram = (uint8_t *)malloc(sizeof s->eram);
        if (eram) memcpy(eram, s->eram, sizeof s->eram);
    }
    memset(s, 0, sizeof *s);
    if (eram) { memcpy(s->eram, eram, sizeof s->eram); free(eram); memcpy(s->rtc, rtc, 5); }
    else memset(s->eram, 0xFF, sizeof s->eram);

    s->magic = STATE_MAGIC; s->version = STATE_VERSION; s->size = sizeof(State);
    s->rom_crc = g->rom_crc;

    s->cgb = g->is_cgb;
    if (s->cgb) {
        // Register values after the CGB boot ROM
        s->a = 0x11; s->f = 0x80; s->b = 0x00; s->c = 0x00;
        s->d = 0xFF; s->e = 0x56; s->h = 0x00; s->l = 0x0D;
        s->div = 0x1EA0;
        memset(s->bgpal, 0xFF, sizeof s->bgpal);
        memset(s->objpal, 0xFF, sizeof s->objpal);
    } else {
        // Register values after the DMG boot ROM
        s->a = 0x01; s->f = 0xB0; s->b = 0x00; s->c = 0x13;
        s->d = 0x00; s->e = 0xD8; s->h = 0x01; s->l = 0x4D;
        s->div = 0xABCC;
    }
    s->sp = 0xFFFE; s->pc = 0x0100;

    s->io[0x00] = 0x30; s->io[0x02] = 0x00; s->io[0x07] = 0x00; s->io[0x0F] = 0x01;
    static const uint8_t snd[] = {
        0x80,0xBF,0xF3,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF, 0x77,0xF3,0x80 };
    for (int i = 0; i < (int)sizeof snd; i++) s->io[0x10 + i] = snd[i];
    static const uint8_t wave[16] = {0x84,0x40,0x43,0xAA,0x2D,0x78,0x92,0x3C,0x60,0x59,0x59,0xB0,0x34,0xB8,0x2E,0xDA};
    memcpy(&s->io[0x30], wave, 16);
    s->ch[0].on = 1; s->ch[0].length = 64 - 0x3F; s->ch[0].vol = 0; s->ch[0].sweep_timer = 8;
    for (int n = 0; n < 4; n++) { s->ch[n].lfsr = 0x7FFF; if (!s->ch[n].env_timer) s->ch[n].env_timer = 8; }

    s->io[0x40] = 0x91; s->io[0x41] = 0x00; s->io[0x47] = 0xFC;
    s->io[0x48] = 0xFF; s->io[0x49] = 0xFF;
    s->io[0x44] = 0; s->mode = 0; s->dot = 0;
    s->win_trig = (s->io[0x4A] == 0);

    s->bank1 = 1; s->rom_bank = 1;
    update_banks(g);
    for (int i = 0; i < GB_WIDTH * GB_HEIGHT; i++) s->fb[i] = s->cgb ? 0x7FFF : 0;
    gb_audio_clear(g);
}

int gb_load_rom(GB *g, const uint8_t *rom, size_t size) {
    if (size < 0x150) return GB_ERR_SIZE;
    uint8_t cgb = rom[0x143];

    uint8_t type = rom[0x147];
    uint8_t mbc, battery = 0, rtc = 0;
    switch (type) {
    case 0x00: mbc = MBC_NONE; break;
    case 0x08: mbc = MBC_NONE; break;
    case 0x09: mbc = MBC_NONE; battery = 1; break;
    case 0x01: case 0x02: mbc = MBC_1; break;
    case 0x03: mbc = MBC_1; battery = 1; break;
    case 0xFF: mbc = MBC_1; battery = 1; break;  // HuC1 (behaves like MBC1, minus IR)
    case 0x05: mbc = MBC_2; break;
    case 0x06: mbc = MBC_2; battery = 1; break;
    case 0x0F: case 0x10: mbc = MBC_3; battery = 1; rtc = 1; break;
    case 0x11: case 0x12: mbc = MBC_3; break;
    case 0x13: mbc = MBC_3; battery = 1; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc = MBC_5; break;
    case 0x1B: case 0x1E: mbc = MBC_5; battery = 1; break;
    default: return GB_ERR_MAPPER;
    }

    static const uint32_t ram_sizes[6] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    uint32_t ram = rom[0x149] < 6 ? ram_sizes[rom[0x149]] : 0;
    if (mbc == MBC_2) ram = 512;
    if (type == 0x10 || type == 0x13 || type == 0x12) { if (!ram) ram = 0x8000; }
    if ((type == 0x03 || type == 0x1B || type == 0x1E || type == 0x09) && !ram) ram = 0x2000;

    // Pad ROM to a multiple of 16 KB so bank math is safe
    size_t padded = (size + 0x3FFF) & ~(size_t)0x3FFF;
    uint8_t *copy = (uint8_t *)malloc(padded);
    if (!copy) return GB_ERR_SIZE;
    memset(copy, 0xFF, padded);
    memcpy(copy, rom, size);

    free(g->rom);
    g->rom = copy;
    g->rom_size = padded;
    g->rom_crc = crc32_buf(rom, size);
    g->mbc = mbc;
    g->is_cgb = (cgb & 0x80) != 0;   // CGB-only and CGB-enhanced carts run in colour mode
    g->has_battery = battery;
    g->has_rtc = rtc;
    g->ram_size = ram;
    g->sram_dirty = 0;
    g->serial_len = 0; g->serial[0] = 0;

    int tl = (cgb & 0x80) ? 15 : 16;
    int n = 0;
    for (int i = 0; i < tl; i++) {
        uint8_t ch = rom[0x134 + i];
        if (!ch) break;
        g->title[n++] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
    }
    while (n > 0 && g->title[n - 1] == ' ') n--;
    g->title[n] = 0;

    memset(&g->s, 0, sizeof g->s);
    memset(g->s.eram, 0xFF, sizeof g->s.eram);
    reset_state(g, 1);
    return GB_OK;
}

void gb_reset(GB *g) { if (g->rom) reset_state(g, 1); }

void gb_run_frame(GB *g) {
    if (!g->rom) return;
    State *s = &g->s;
    s->frame_done = 0;
    uint64_t start = s->cycles;
    while (!s->frame_done && s->cycles - start < GB_FRAME_CYCLES) cpu_step(g);
    convert_frame(g);
}

const uint32_t *gb_framebuffer(GB *g) { return g->rgba; }

void gb_set_palette(GB *g, const uint32_t colors[4]) {
    for (int i = 0; i < 4; i++) g->palette[i] = colors[i] & 0xFFFFFF;
    convert_frame(g);
}

void gb_set_color_correction(GB *g, int enabled) {
    g->color_correct = enabled;
    build_lut(g);
    convert_frame(g);
}

int gb_is_cgb(GB *g) { return g->is_cgb; }

void gb_set_buttons(GB *g, uint8_t mask) {
    // A real D-pad can't press opposite directions at once; some games crash if it does.
    if ((mask & (GB_BTN_LEFT | GB_BTN_RIGHT)) == (GB_BTN_LEFT | GB_BTN_RIGHT)) mask &= (uint8_t)~(GB_BTN_LEFT | GB_BTN_RIGHT);
    if ((mask & (GB_BTN_UP | GB_BTN_DOWN)) == (GB_BTN_UP | GB_BTN_DOWN)) mask &= (uint8_t)~(GB_BTN_UP | GB_BTN_DOWN);
    uint8_t pressed = mask & (uint8_t)~g->buttons;
    g->buttons = mask;
    if (pressed) g->s.io[0x0F] |= 0x10;
}

void gb_set_audio_enabled(GB *g, int enabled) { g->audio_enabled = enabled; }

size_t gb_audio_read(GB *g, int16_t *out, size_t frames) {
    size_t r = atomic_load_explicit(&g->ring_r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&g->ring_w, memory_order_acquire);
    size_t avail = w - r;
    if (frames > avail) frames = avail;
    for (size_t i = 0; i < frames; i++) {
        size_t k = ((r + i) % RING_FRAMES) * 2;
        out[i * 2] = g->ring[k];
        out[i * 2 + 1] = g->ring[k + 1];
    }
    atomic_store_explicit(&g->ring_r, r + frames, memory_order_release);
    return frames;
}

size_t gb_audio_buffered(GB *g) {
    return atomic_load_explicit(&g->ring_w, memory_order_acquire) -
           atomic_load_explicit(&g->ring_r, memory_order_acquire);
}

void gb_audio_clear(GB *g) {
    size_t w = atomic_load_explicit(&g->ring_w, memory_order_acquire);
    atomic_store_explicit(&g->ring_r, w, memory_order_release);
}

const char *gb_title(GB *g) { return g->title; }
int gb_has_battery(GB *g) { return g->has_battery; }
int gb_has_rtc(GB *g) { return g->has_rtc; }
size_t gb_sram_size(GB *g) { return g->ram_size; }
int gb_sram_dirty(GB *g) { return g->sram_dirty; }
void gb_sram_clear_dirty(GB *g) { g->sram_dirty = 0; }
uint32_t gb_rom_crc32(GB *g) { return g->rom_crc; }

/* ---- Battery save import/export ---- */

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

size_t gb_save_export_size(GB *g, int include_rtc) {
    if (!g->ram_size && !g->has_rtc) return 0;
    return g->ram_size + ((include_rtc && g->has_rtc) ? 48 : 0);
}

size_t gb_save_export(GB *g, uint8_t *out, size_t cap, int64_t unix_now, int include_rtc) {
    size_t need = gb_save_export_size(g, include_rtc);
    if (!need || cap < need) return 0;
    State *s = &g->s;
    if (g->mbc == MBC_2) for (uint32_t i = 0; i < 512; i++) out[i] = 0xF0 | (s->eram[i] & 0x0F);
    else memcpy(out, s->eram, g->ram_size);
    if (include_rtc && g->has_rtc) {
        uint8_t *p = out + g->ram_size;
        for (int i = 0; i < 5; i++) put32(p + i * 4, s->rtc[i]);
        for (int i = 0; i < 5; i++) put32(p + 20 + i * 4, s->rtc_latched[i]);
        uint64_t t = (uint64_t)unix_now;
        put32(p + 40, (uint32_t)t);
        put32(p + 44, (uint32_t)(t >> 32));
    }
    return need;
}

int gb_save_import(GB *g, const uint8_t *data, size_t len, int64_t unix_now) {
    if (!g->rom) return GB_ERR_STATE;
    if (!g->ram_size && !g->has_rtc) return GB_ERR_NO_SAVE;
    State *s = &g->s;
    size_t ram = g->ram_size;
    size_t footer = 0;

    if (g->has_rtc && len >= 44) {
        size_t extra = len > ram ? len - ram : 0;
        if (extra == 48 || extra == 44) footer = extra;
        else if (len % 0x2000 == 48 || len % 0x2000 == 44) footer = len % 0x2000;
    }
    size_t body = len - footer;

    // MBC2 RAM is 512 nibbles; some emulators save it expanded to 8 KB.
    size_t copy = body < ram ? body : ram;
    memset(s->eram, 0xFF, ram);
    if (g->mbc == MBC_2) for (size_t i = 0; i < copy; i++) s->eram[i] = data[i] & 0x0F;
    else memcpy(s->eram, data, copy);

    if (footer) {
        const uint8_t *p = data + body;
        static const uint8_t m[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
        for (int i = 0; i < 5; i++) s->rtc[i] = (uint8_t)get32(p + i * 4) & m[i];
        for (int i = 0; i < 5; i++) s->rtc_latched[i] = (uint8_t)get32(p + 20 + i * 4) & m[i];
        int64_t stamp = (int64_t)get32(p + 40);
        if (footer == 48) stamp |= (int64_t)get32(p + 44) << 32;
        if (stamp > 0 && unix_now > stamp) rtc_add_seconds(s, unix_now - stamp);
        s->rtc_cycles = 0;
    }
    g->sram_dirty = 0;
    return GB_OK;
}

/* ---- Save states ---- */

size_t gb_state_size(GB *g) { (void)g; return sizeof(State); }

int gb_state_save(GB *g, uint8_t *out, size_t cap) {
    if (!g->rom || cap < sizeof(State)) return GB_ERR_STATE;
    g->s.magic = STATE_MAGIC; g->s.version = STATE_VERSION;
    g->s.size = sizeof(State); g->s.rom_crc = g->rom_crc;
    memcpy(out, &g->s, sizeof(State));
    return GB_OK;
}

int gb_state_load(GB *g, const uint8_t *data, size_t len) {
    if (!g->rom || len != sizeof(State)) return GB_ERR_STATE;
    uint32_t hdr[4];
    memcpy(hdr, data, sizeof hdr);   // magic, version, size, rom_crc
    if (hdr[0] != STATE_MAGIC || hdr[1] != STATE_VERSION ||
        hdr[2] != sizeof(State) || hdr[3] != g->rom_crc) return GB_ERR_STATE;
    memcpy(&g->s, data, sizeof(State));
    update_banks(g);
    g->sram_dirty = 1;
    gb_audio_clear(g);
    convert_frame(g);
    return GB_OK;
}

const char *gb_serial_output(GB *g) { return g->serial; }
uint8_t gb_peek(GB *g, uint16_t addr) { return bus_read(g, addr); }
