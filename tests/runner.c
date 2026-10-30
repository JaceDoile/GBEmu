// Headless test runner: runs a ROM for N frames, reports serial output,
// optionally dumps the screen as a PGM.
#include "../Core/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: runner rom [frames] [out.ppm]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("rom"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(n); fread(rom, 1, n, f); fclose(f);
    int frames = argc > 2 ? atoi(argv[2]) : 3600;

    GB *gb = gb_create();
    int r = gb_load_rom(gb, rom, n);
    if (r) { printf("load error %d\n", r); return 2; }
    int16_t audio[4096];
    int result = 1;
    for (int i = 0; i < frames; i++) {
        gb_run_frame(gb);
        while (gb_audio_read(gb, audio, 2048)) {}
        const char *out = gb_serial_output(gb);
        if (strstr(out, "Passed") || strstr(out, "Failed")) { if (strstr(out, "Passed") && !strstr(out, "Failed")) result = 0; break; }
        // mem_timing-2 style: signature at A001 then status at A000
        if (gb_peek(gb, 0xA001) == 0xDE && gb_peek(gb, 0xA002) == 0xB0 && gb_peek(gb, 0xA003) == 0x61 && gb_peek(gb, 0xA000) != 0x80) {
            result = gb_peek(gb, 0xA000) != 0;
            printf("[A000 status %d] ", gb_peek(gb, 0xA000));
            break;
        }
    }
    printf("%s\n", gb_serial_output(gb));
    if (argc > 3) {
        FILE *o = fopen(argv[3], "wb");
        fprintf(o, "P6 160 144 255\n");
        const uint32_t *fb = gb_framebuffer(gb);
        for (int i = 0; i < 160 * 144; i++) { uint8_t px[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]}; fwrite(px, 1, 3, o); }
        fclose(o);
    }
    gb_destroy(gb);
    return result;
}
