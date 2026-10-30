// Game Boy / Game Boy Color emulation core — portable C99/C11, no dependencies.
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_WIDTH  160
#define GB_HEIGHT 144
#define GB_CLOCK  4194304
#define GB_FRAME_CYCLES 70224

enum {
    GB_BTN_A = 1, GB_BTN_B = 2, GB_BTN_SELECT = 4, GB_BTN_START = 8,
    GB_BTN_RIGHT = 16, GB_BTN_LEFT = 32, GB_BTN_UP = 64, GB_BTN_DOWN = 128
};

enum {
    GB_OK = 0,
    GB_ERR_SIZE = -1,        // file too small / corrupt
    GB_ERR_MAPPER = -3,      // unsupported cartridge hardware
    GB_ERR_STATE = -4,       // save state invalid or for another game
    GB_ERR_NO_SAVE = -5      // cartridge has no battery-backed RAM
};

typedef struct GB GB;

GB  *gb_create(void);
void gb_destroy(GB *gb);

// Copies the ROM. Returns GB_OK or an error.
int  gb_load_rom(GB *gb, const uint8_t *rom, size_t size);
void gb_reset(GB *gb);   // power cycle (keeps cartridge RAM)

// Runs one video frame (~1/59.73 s of emulated time).
void gb_run_frame(GB *gb);

// 160x144 pixels, each 0xFFRRGGBB (use little-endian 32-bit, alpha first).
const uint32_t *gb_framebuffer(GB *gb);
// Four colours from lightest to darkest, 0xRRGGBB (original Game Boy games).
void gb_set_palette(GB *gb, const uint32_t colors[4]);
// Game Boy Color: mimic the real LCD's colour response (default on).
void gb_set_color_correction(GB *gb, int enabled);
int  gb_is_cgb(GB *gb);   // running in Game Boy Color mode

void gb_set_buttons(GB *gb, uint8_t mask);

// Audio: interleaved stereo int16. Single-producer/single-consumer safe:
// gb_run_frame on one thread, gb_audio_read on the audio thread.
void   gb_set_sample_rate(GB *gb, double rate);
void   gb_set_audio_enabled(GB *gb, int enabled);
size_t gb_audio_read(GB *gb, int16_t *out, size_t frames);
size_t gb_audio_buffered(GB *gb);   // frames waiting
void   gb_audio_clear(GB *gb);

// Cartridge info
const char *gb_title(GB *gb);
int     gb_has_battery(GB *gb);
int     gb_has_rtc(GB *gb);
size_t  gb_sram_size(GB *gb);
int     gb_sram_dirty(GB *gb);       // RAM written since last clear
void    gb_sram_clear_dirty(GB *gb);
uint32_t gb_rom_crc32(GB *gb);

// Battery saves in the common ".sav" layout used by BGB, mGBA, VBA-M,
// SameBoy, Gambatte(.sav) and RetroArch(.srm): raw cartridge RAM, plus for
// MBC3+RTC carts an optional 48-byte clock footer.
// Returns number of bytes written (0 on error). unix_now = current time.
size_t gb_save_export_size(GB *gb, int include_rtc);
size_t gb_save_export(GB *gb, uint8_t *out, size_t cap, int64_t unix_now, int include_rtc);
// Accepts raw RAM of any size (truncated/padded), with or without 44/48-byte
// RTC footers. Advances the clock by the time elapsed since the footer stamp.
int    gb_save_import(GB *gb, const uint8_t *data, size_t len, int64_t unix_now);

// Save states (this emulator only)
size_t gb_state_size(GB *gb);
int    gb_state_save(GB *gb, uint8_t *out, size_t cap);
int    gb_state_load(GB *gb, const uint8_t *data, size_t len);

void   gb_rtc_advance(GB *gb, int64_t seconds);

// Debug / testing
const char *gb_serial_output(GB *gb);
uint8_t gb_peek(GB *gb, uint16_t addr);

#ifdef __cplusplus
}
#endif
#endif
