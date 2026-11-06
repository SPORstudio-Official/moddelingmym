/* Linux test harness: runs the shared game core headlessly, drives it with a
   script, dumps frames as BMP and mixes audio to WAV for inspection.        */
#include "da.h"
#include <stdio.h>

static FB fb;
static Input in;

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int rowsize = fb.w * 3;
    int pad = (4 - (rowsize % 4)) % 4;
    int datasize = (rowsize + pad) * fb.h;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    int fsize = 54 + datasize;
    memcpy(hdr + 2, &fsize, 4);
    int off = 54; memcpy(hdr + 10, &off, 4);
    int ih = 40; memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &fb.w, 4);
    memcpy(hdr + 22, &fb.h, 4);
    short planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &datasize, 4);
    fwrite(hdr, 1, 54, f);
    unsigned char *row = calloc(1, (size_t)(rowsize + pad));
    for (int y = fb.h - 1; y >= 0; y--) {
        for (int x = 0; x < fb.w; x++) {
            u32 c = fb.px[y * fb.w + x];
            row[x * 3 + 0] = (unsigned char)(c & 0xff);
            row[x * 3 + 1] = (unsigned char)((c >> 8) & 0xff);
            row[x * 3 + 2] = (unsigned char)((c >> 16) & 0xff);
        }
        fwrite(row, 1, (size_t)(rowsize + pad), f);
    }
    free(row);
    fclose(f);
    printf("  shot -> %s\n", path);
}

static void write_wav(const char *path, float seconds) {
    int frames = (int)(seconds * MIX_SR);
    short *buf = calloc((size_t)frames * 2, sizeof(short));
    audio_mix(buf, frames);
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return; }
    int datasz = frames * 4;
    int riff = 36 + datasz;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    int fsz = 16; fwrite(&fsz, 4, 1, f);
    short fmt = 1, ch = 2; int sr = MIX_SR; int byterate = sr * 4;
    short align = 4, bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datasz, 4, 1, f);
    fwrite(buf, 4, (size_t)frames, f);
    fclose(f);
    free(buf);
    printf("  audio -> %s (%.1fs)\n", path, seconds);
}

static void step_frame(void) {
    in.dt = 1.0f / 60.0f;
    in.time += in.dt;
    game_frame(&in);
    in.mouse_pressed = 0;
    in.mouse_released = 0;
    memset(in.key_pressed, 0, sizeof in.key_pressed);
}

int main(int argc, char **argv) {
    fb.w = FB_W; fb.h = FB_H;
    fb.px = calloc((size_t)fb.w * fb.h, sizeof(u32));
    memset(&in, 0, sizeof in);
    game_init(&fb);

    if (argc < 2) { printf("usage: harness script.txt\n"); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { printf("no script %s\n", argv[1]); return 1; }
    char line[512], cmd[64];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int a = 0, b = 0;
        char path[256];
        if (sscanf(line, "%63s %d %d", cmd, &a, &b) < 1) continue;
        if (!strcmp(cmd, "frames")) { for (int i = 0; i < a; i++) step_frame(); }
        else if (!strcmp(cmd, "move")) { in.mouse_x = a; in.mouse_y = b; step_frame(); }
        else if (!strcmp(cmd, "click")) {
            in.mouse_x = a; in.mouse_y = b; in.mouse_down = 1; in.mouse_pressed = 1;
            step_frame();
            in.mouse_down = 0; in.mouse_released = 1;
            step_frame();
        }
        else if (!strcmp(cmd, "down")) { in.mouse_x = a; in.mouse_y = b; in.mouse_down = 1; in.mouse_pressed = 1; step_frame(); }
        else if (!strcmp(cmd, "up")) { in.mouse_down = 0; in.mouse_released = 1; step_frame(); }
        else if (!strcmp(cmd, "key")) { in.key_pressed[a & 127] = 1; in.key_down[a & 127] = 1; step_frame(); in.key_down[a & 127] = 0; }
        else if (!strcmp(cmd, "shot")) { sscanf(line, "%*s %255s", path); write_bmp(path); }
        else if (!strcmp(cmd, "kill")) { game_debug_kill(); step_frame(); }
        else if (!strcmp(cmd, "solved")) { game_debug_force_line(); step_frame(); }
        else if (!strcmp(cmd, "night")) { game_debug_night(a); step_frame(); }
        else if (!strcmp(cmd, "inf")) { game_debug_interference((float)a / 100.0f); step_frame(); }
        else if (!strcmp(cmd, "audio")) { float s = 2.0f; sscanf(line, "%*s %255s %f", path, &s); write_wav(path, s); }
        else if (!strcmp(cmd, "dump")) { char st[512]; sscanf(line, "%*s %511[^\n]", st); printf("  %s | ", st); game_debug(st, 511); printf("%s\n", st); }
        else printf("  ? %s\n", cmd);
    }
    fclose(f);
    free(fb.px);
    return 0;
}
