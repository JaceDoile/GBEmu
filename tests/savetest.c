// Unit tests for battery-save import/export and save states.
#include "../Core/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

// Build a minimal ROM: header + infinite loop at 0x100
static uint8_t *make_rom(uint8_t type, uint8_t ramcode, size_t *n) {
    *n = 0x8000;
    uint8_t *r = calloc(1, *n);
    r[0x100] = 0x18; r[0x101] = 0xFE;           // JR -2
    memcpy(&r[0x134], "SAVETEST", 8);
    r[0x147] = type; r[0x148] = 0; r[0x149] = ramcode;
    return r;
}

int main(void) {
    size_t n;
    int64_t now = 1800000000;

    /* --- MBC3 + RTC + 32KB RAM (Pokemon Gold/Silver style) --- */
    uint8_t *rom = make_rom(0x10, 3, &n);
    GB *gb = gb_create();
    CHECK(gb_load_rom(gb, rom, n) == GB_OK, "load MBC3+RTC rom");
    CHECK(gb_has_rtc(gb) && gb_has_battery(gb) && gb_sram_size(gb) == 0x8000, "MBC3 cart info");

    // Fake a save from another emulator: 32KB + 48-byte footer, stamped 1 hour ago
    size_t len = 0x8000 + 48;
    uint8_t *sav = malloc(len);
    for (size_t i = 0; i < 0x8000; i++) sav[i] = (uint8_t)(i * 7);
    memset(sav + 0x8000, 0, 48);
    uint32_t rtc[5] = {50, 59, 23, 10, 0};    // 10 days 23:59:50
    for (int i = 0; i < 5; i++) { uint32_t v = rtc[i]; memcpy(sav + 0x8000 + i * 4, &v, 4); memcpy(sav + 0x8000 + 20 + i * 4, &v, 4); }
    int64_t stamp = now - 3600; memcpy(sav + 0x8000 + 40, &stamp, 8);
    CHECK(gb_save_import(gb, sav, len, now) == GB_OK, "import 32KB+48 footer");
    CHECK(gb_peek(gb, 0) == rom[0], "rom still readable");

    uint8_t *out = malloc(0x8000 + 48);
    size_t w = gb_save_export(gb, out, 0x8000 + 48, now, 1);
    CHECK(w == 0x8000 + 48, "export size with RTC");
    CHECK(memcmp(out, sav, 0x8000) == 0, "RAM round-trips");
    uint32_t s, m, h, dl;
    memcpy(&s, out + 0x8000, 4); memcpy(&m, out + 0x8004, 4); memcpy(&h, out + 0x8008, 4); memcpy(&dl, out + 0x800C, 4);
    printf("      clock after +1h: day %u %02u:%02u:%02u\n", dl, h, m, s);
    CHECK(dl == 11 && h == 0 && m == 59 && s == 50, "RTC advanced by elapsed hour (rolls into day 11)");

    // 44-byte (32-bit timestamp) footer variant
    uint8_t *sav44 = malloc(0x8000 + 44);
    memcpy(sav44, sav, 0x8000 + 40);
    uint32_t st32 = (uint32_t)now; memcpy(sav44 + 0x8000 + 40, &st32, 4);
    CHECK(gb_save_import(gb, sav44, 0x8000 + 44, now) == GB_OK, "import 44-byte footer");
    CHECK(gb_save_export_size(gb, 0) == 0x8000, "export without RTC = raw size (.srm)");

    // Raw save with no footer
    CHECK(gb_save_import(gb, sav, 0x8000, now) == GB_OK, "import raw 32KB");

    // Save state round trip
    size_t ss = gb_state_size(gb);
    uint8_t *st = malloc(ss);
    for (int i = 0; i < 10; i++) gb_run_frame(gb);
    CHECK(gb_state_save(gb, st, ss) == GB_OK, "save state");
    for (int i = 0; i < 10; i++) gb_run_frame(gb);
    CHECK(gb_state_load(gb, st, ss) == GB_OK, "load state");
    st[12] ^= 0xFF;  // corrupt ROM checksum field
    CHECK(gb_state_load(gb, st, ss) == GB_ERR_STATE, "reject state from another game");
    gb_destroy(gb);

    /* --- MBC1 8KB: oversized (32KB flash-cart dump) and undersized saves --- */
    free(rom); rom = make_rom(0x03, 2, &n);
    gb = gb_create(); gb_load_rom(gb, rom, n);
    CHECK(gb_sram_size(gb) == 0x2000, "MBC1 8KB");
    CHECK(gb_save_import(gb, sav, 0x8000, now) == GB_OK, "import oversized 32KB into 8KB cart");
    w = gb_save_export(gb, out, 0x8000, now, 1);
    CHECK(w == 0x2000 && memcmp(out, sav, 0x2000) == 0, "truncated correctly, no RTC footer for non-RTC cart");
    CHECK(gb_save_import(gb, sav, 0x800, now) == GB_OK, "import undersized 2KB");
    w = gb_save_export(gb, out, 0x8000, now, 1);
    CHECK(out[0x800] == 0xFF && memcmp(out, sav, 0x800) == 0, "padded with 0xFF");
    gb_destroy(gb);

    /* --- MBC2 512 nibbles --- */
    free(rom); rom = make_rom(0x06, 0, &n);
    gb = gb_create(); gb_load_rom(gb, rom, n);
    CHECK(gb_sram_size(gb) == 512, "MBC2 512 bytes");
    CHECK(gb_save_import(gb, sav, 512, now) == GB_OK, "import MBC2");
    w = gb_save_export(gb, out, 4096, now, 1);
    int ok = w == 512; for (int i = 0; i < 512; i++) if (out[i] != (0xF0 | (sav[i] & 0xF))) ok = 0;
    CHECK(ok, "MBC2 nibbles exported with upper bits set");
    gb_destroy(gb);

    /* --- No battery --- */
    free(rom); rom = make_rom(0x01, 0, &n);
    gb = gb_create(); gb_load_rom(gb, rom, n);
    CHECK(gb_save_import(gb, sav, 0x2000, now) == GB_ERR_NO_SAVE, "reject save for cart without RAM");
    gb_destroy(gb);

    /* --- CGB-only cart runs in colour mode --- */
    rom[0x143] = 0xC0;
    gb = gb_create();
    CHECK(gb_load_rom(gb, rom, n) == GB_OK && gb_is_cgb(gb), "GBC-only cart loads in colour mode");
    gb_destroy(gb);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
