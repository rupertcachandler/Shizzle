/*
 * ndi-testgen — NDI test pattern generator for Linux
 *
 * Generates NDI video test patterns and audio tones using the official SDK.
 * The missing piece: no Linux tool exists to *produce* NDI streams for testing.
 *
 * Patterns:
 *   smpte75     SMPTE 75% colour bars (default, classic broadcast test)
 *   smpte100    SMPTE 100% colour bars (full saturation)
 *   gradient    Horizontal luminance ramp (16-235)
 *   red         Solid red fill
 *   green       Solid green fill
 *   blue        Solid blue fill
 *   white       Solid white (digital 235)
 *   black       Solid black (digital 16)
 *   moving      Animated circle bouncing around (tests fps/motion)
 *   clock       Timecode overlay on colour bars
 *   checkers    Checkerboard pattern (alignment test)
 *
 * Usage: ./ndi-testgen [options]
 *   --name NAME         NDI source name (default: "Test Pattern")
 *   --groups GROUPS     NDI groups (default: NULL = all)
 *   --pattern PAT       Pattern type (default: smpte75)
 *   --width W           Horizontal resolution (default: 1920)
 *   --height H          Vertical resolution (default: 1080)
 *   --fps N             Frame rate numerator (default: 30000)
 *   --fps-d D           Frame rate denominator (default: 1001)
 *   --audio             Enable 1kHz test tone audio (default: off)
 *   --audio-freq FREQ   Audio frequency in Hz (default: 1000)
 *   --audio-level dB    Audio level in dBFS (default: -20)
 *   --no-video          Disable video (audio-only source)
 *   --clock-video       Clock video to frame rate (default: true)
 *   --clock-audio       Clock audio to sample rate (default: false)
 *   --connections       Print connection count every second
 *   --json              Output status as JSON each frame
 *   --once              Send one frame then exit
 *   --duration SECS     Run for N seconds then exit (0 = forever)
 *   --metadata XML      Send custom metadata string on connect
 *   --help              Show this help
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -Iinclude -o ndi-testgen src/ndi-testgen.c \
 *       -L/usr/lib -lndi -L/usr/lib/x86_64-linux-gnu -ljson-c \
 *       -Wl,-rpath,/usr/lib -lm
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Frau Blücher
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <json-c/json.h>

#include <Processing.NDI.Lib.h>

/* ─── Pattern types ──────────────────────────────────────────────── */

typedef enum {
    PAT_SMPTE75,
    PAT_SMPTE100,
    PAT_GRADIENT,
    PAT_RED,
    PAT_GREEN,
    PAT_BLUE,
    PAT_WHITE,
    PAT_BLACK,
    PAT_MOVING,
    PAT_CLOCK,
    PAT_CHECKERS,
    PAT_COUNT
} pattern_t;

static const char *pattern_names[] = {
    "smpte75", "smpte100", "gradient", "red", "green", "blue",
    "white", "black", "moving", "clock", "checkers"
};

static pattern_t parse_pattern(const char *s) {
    for (int i = 0; i < PAT_COUNT; i++) {
        if (strcasecmp(s, pattern_names[i]) == 0) return (pattern_t)i;
    }
    fprintf(stderr, "Unknown pattern '%s', using smpte75\n", s);
    return PAT_SMPTE75;
}

/* ─── Globals ─────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static NDIlib_send_instance_t g_sender = NULL;

void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ─── SMPTE colour bars ───────────────────────────────────────────── */

/* 75% bars: top 2/3, then middle grey/blue/magenta/cyan sections, bottom PLUGE */
static void draw_smpte75(uint8_t *buf, int w, int h) {
    /* Top section: 2/3 height, 7 colour bars */
    const int top_h = h * 2 / 3;
    const int bar_w = w / 7;
    /* 75% amplitude SMPTE bars in BGRA */
    const uint8_t bars75[7][4] = {
        {192, 192, 192, 255},  /* grey   (75% white) */
        {192,  12, 192, 255},  /* cyan   */
        { 12,  12, 192, 255},  /* blue   */
        {192, 192,  12, 255},  /* yellow */
        { 12, 192,  12, 255},  /* green  */
        {192,  12,  12, 255},  /* red    */
        { 12,  12,  12, 255},  /* black  */
    };

    for (int y = 0; y < top_h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = x / bar_w;
            if (idx > 6) idx = 6;
            uint8_t *p = buf + (y * w + x) * 4;
            p[0] = bars75[idx][0]; p[1] = bars75[idx][1];
            p[2] = bars75[idx][2]; p[3] = bars75[idx][3];
        }
    }

    /* Middle section: 1/6 height, blue/black/magenta/black/cyan/black/grey/blue cascade */
    const int mid_y = top_h;
    const int mid_h = h / 6;
    const int mid_bar_w = w / 7;
    const uint8_t mid_bars[7][4] = {
        { 12,  12, 192, 255},  /* blue    */
        { 12,  12,  12, 255},  /* black   */
        {192,  12, 192, 255},  /* magenta */
        { 12,  12,  12, 255},  /* black   */
        {  0, 192, 192, 255},  /* cyan    */
        { 12,  12,  12, 255},  /* black   */
        {  0,  12,  12, 255},  /* -I      */
    };
    for (int y = 0; y < mid_h; y++) {
        int ry = mid_y + y;
        if (ry >= h) break;
        for (int x = 0; x < w; x++) {
            int idx = x / mid_bar_w;
            if (idx > 6) idx = 6;
            uint8_t *p = buf + (ry * w + x) * 4;
            p[0] = mid_bars[idx][0]; p[1] = mid_bars[idx][1];
            p[2] = mid_bars[idx][2]; p[3] = mid_bars[idx][3];
        }
    }

    /* Bottom section: PLUGE — 1/6 height */
    const int bot_y = top_h + mid_h;
    const int bot_h = h - bot_y;
    /* PLUGE: dark grey segment, then 4 equal blocks below —
       Reference white | Black | -2% dark | +2% light | Black  
       Simplified to useful PLUGE blocks */
    const int seg6 = w / 6;
    const uint8_t pluge[6][4] = {
        {  0,  0,  14, 255},   /* -2% near-black (below black) */
        { 16, 16,  16, 255},   /* black (digital 16) */
        { 29, 29,  29, 255},   /* +2% above black */
        { 16, 16,  16, 255},   /* black */
        { 25, 25, 218, 255},   /* blue-ish (should be white-ish but adding colour) */
        { 16, 16,  16, 255},   /* black */
    };
    for (int y = 0; y < bot_h; y++) {
        int ry = bot_y + y;
        if (ry >= h) break;
        for (int x = 0; x < w; x++) {
            int idx = x / seg6;
            if (idx > 5) idx = 5;
            uint8_t *p = buf + (ry * w + x) * 4;
            p[0] = pluge[idx][0]; p[1] = pluge[idx][1];
            p[2] = pluge[idx][2]; p[3] = pluge[idx][3];
        }
    }
}

/* 100% bars: full saturation, simpler layout */
static void draw_smpte100(uint8_t *buf, int w, int h) {
    const int bar_w = w / 7;
    /* BGRA: byte0=Blue, byte1=Green, byte2=Red, byte3=Alpha */
    /* Correct 100% SMPTE bars: White, Yellow, Cyan, Green, Magenta, Red, Blue */
    const uint8_t b100[7][4] = {
        {255, 255, 255, 255},  /* White:   B=255 G=255 R=255 */
        {  0, 255, 255, 255},  /* Yellow:  B=0   G=255 R=255 */
        {255, 255,   0, 255},  /* Cyan:    B=255 G=255 R=0   */
        {  0, 255,   0, 255},  /* Green:   B=0   G=255 R=0   */
        {255,   0, 255, 255},  /* Magenta: B=255 G=0   R=255 */
        {  0,   0, 255, 255},  /* Red:     B=0   G=0   R=255 */
        {255,   0,   0, 255},  /* Blue:    B=255 G=0   R=0   */
    };

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = x / bar_w;
            if (idx > 6) idx = 6;
            uint8_t *p = buf + (y * w + x) * 4;
            p[0] = b100[idx][0]; p[1] = b100[idx][1];
            p[2] = b100[idx][2]; p[3] = b100[idx][3];
        }
    }
}

/* ─── Other patterns ──────────────────────────────────────────────── */

static void draw_gradient(uint8_t *buf, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t v = (uint8_t)((double)x / w * 219 + 16);  /* 16-235 range */
            uint8_t *p = buf + (y * w + x) * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }
}

static void draw_solid(uint8_t *buf, int w, int h, uint8_t b, uint8_t g, uint8_t r) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = buf + (y * w + x) * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        }
    }
}

static void draw_checkers(uint8_t *buf, int w, int h) {
    const int sz = 64;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int cx = x / sz, cy = y / sz;
            uint8_t v = ((cx + cy) % 2 == 0) ? 235 : 16;
            uint8_t *p = buf + (y * w + x) * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }
}

static void draw_moving(uint8_t *buf, int w, int h, int frame) {
    /* Dark background */
    memset(buf, 0, w * h * 4);
    for (int i = 0; i < w * h; i++) {
        buf[i * 4 + 3] = 255;  /* alpha */
    }

    /* Bouncing circle */
    double fps = 30.0;
    double t = frame / fps;
    double cx = w / 2.0 + (w / 3.0) * sin(t * 2.1);
    double cy = h / 2.0 + (h / 3.0) * cos(t * 1.7);
    double radius = fmin(w, h) / 20.0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = x - cx, dy = y - cy;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist < radius) {
                uint8_t *p = buf + (y * w + x) * 4;
                /* Gradient fill inside circle */
                double bright = 1.0 - (dist / radius);
                p[0] = (uint8_t)(50 * bright);   /* B */
                p[1] = (uint8_t)(200 * bright);   /* G */
                p[2] = (uint8_t)(255 * bright);   /* R */
                p[3] = 255;
            }
        }
    }
}

static void draw_clock(uint8_t *buf, int w, int h, int frame) {
    /* Draw 75% bars as background */
    draw_smpte75(buf, w, h);

    /* Overlay a timestamp box at bottom-right */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[64];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d  F:%d",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, frame);

    /* Simple bitmap-style text renderer for digits */
    const int box_w = 260, box_h = 36;
    int ox = w - box_w - 20, oy = h - box_h - 20;

    /* Draw black box */
    for (int y = 0; y < box_h; y++) {
        for (int x = 0; x < box_w; x++) {
            int px = ox + x, py = oy + y;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint8_t *p = buf + (py * w + px) * 4;
                p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 255;
            }
        }
    }

    /* Draw text pixels — very crude 5x7 font for digits, colon, space, F */
    /* Each character is 5 wide, 7 tall, 1px spacing */
    const int char_w = 5, char_h = 7, spacing = 1;
    const int scale = 2;  /* 2x scale for readability */
    const uint8_t font5x7[][7] = {
        /* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
        /* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        /* 2 */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        /* 3 */ {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
        /* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
        /* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        /* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
        /* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        /* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        /* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    };
    /* : = {0x00,0x04,0x04,0x00,0x04,0x04,0x00} */
    /* F = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10} */
    /* space = all zeros */

    /* Render each character */
    const char *s = ts;
    int cx = 0;
    while (*s && cx < 12) {
        uint8_t glyph[7];
        if (*s >= '0' && *s <= '9') {
            memcpy(glyph, font5x7[*s - '0'], 7);
        } else if (*s == ':') {
            uint8_t colon[] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00};
            memcpy(glyph, colon, 7);
        } else if (*s == 'F') {
            uint8_t f[] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
            memcpy(glyph, f, 7);
        } else {
            /* space or unknown */
            memset(glyph, 0, 7);
        }

        for (int gy = 0; gy < char_h; gy++) {
            for (int gx = 0; gx < char_w; gx++) {
                if (glyph[gy] & (0x10 >> gx)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = ox + 8 + (cx * (char_w + spacing) + gx) * scale + sx;
                            int py = oy + 6 + (gy * scale) + sy;
                            if (px >= 0 && px < w && py >= 0 && py < h) {
                                uint8_t *p = buf + (py * w + px) * 4;
                                p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
                            }
                        }
                    }
                }
            }
        }
        s++;
        cx++;
    }
}

/* ─── Pattern dispatcher ──────────────────────────────────────────── */

static void draw_pattern(uint8_t *buf, int w, int h, pattern_t pat, int frame) {
    switch (pat) {
        case PAT_SMPTE75:   draw_smpte75(buf, w, h); break;
        case PAT_SMPTE100:  draw_smpte100(buf, w, h); break;
        case PAT_GRADIENT:  draw_gradient(buf, w, h); break;
        case PAT_RED:       draw_solid(buf, w, h, 0, 0, 255); break;
        case PAT_GREEN:     draw_solid(buf, w, h, 0, 255, 0); break;
        case PAT_BLUE:      draw_solid(buf, w, h, 255, 0, 0); break;
        case PAT_WHITE:     draw_solid(buf, w, h, 235, 235, 235); break;
        case PAT_BLACK:     draw_solid(buf, w, h, 16, 16, 16); break;
        case PAT_MOVING:    draw_moving(buf, w, h, frame); break;
        case PAT_CLOCK:     draw_clock(buf, w, h, frame); break;
        case PAT_CHECKERS:  draw_checkers(buf, w, h); break;
        default:            draw_smpte75(buf, w, h); break;
    }
}

/* ─── Audio generation ─────────────────────────────────────────────── */

static float db_to_linear(double db) {
    return (float)pow(10.0, db / 20.0);
}

static void generate_audio_tone(float *audio_buf, int no_samples, int no_channels,
                                 int sample_rate, double freq, double level_db,
                                 int64_t sample_offset) {
    float amp = db_to_linear(level_db);
    double phase_per_sample = 2.0 * M_PI * freq / sample_rate;

    for (int s = 0; s < no_samples; s++) {
        double t = phase_per_sample * (double)(sample_offset + s);
        float sample = (float)(amp * sin(t));
        for (int ch = 0; ch < no_channels; ch++) {
            /* FLTP format: channels are interleaved in the stride layout */
            /* For v2 audio frame, p_data is float*, channel_stride_in_bytes
               specifies stride between channel buffers */
            /* We'll fill interleaved here and set up stride properly */
            audio_buf[s * no_channels + ch] = sample;
        }
    }
}

/* ─── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Defaults */
    const char *source_name = "Test Pattern";
    const char *groups = NULL;
    pattern_t pattern = PAT_SMPTE75;
    int width = 1920, height = 1080;
    int fps_n = 30000, fps_d = 1001;  /* 29.97fps default */
    int enable_audio = 0;
    double audio_freq = 1000.0;
    double audio_level_db = -20.0;
    int audio_channels = 2;
    int audio_sample_rate = 48000;
    int no_video = 0;
    int clock_video = 1, clock_audio = 0;
    int show_connections = 0;
    int json_output = 0;
    int once = 0;
    int duration_secs = 0;
    const char *metadata_xml = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            source_name = argv[++i];
        } else if (strcmp(argv[i], "--groups") == 0 && i + 1 < argc) {
            groups = argv[++i];
        } else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            pattern = parse_pattern(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps-d") == 0 && i + 1 < argc) {
            fps_d = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio") == 0) {
            enable_audio = 1;
        } else if (strcmp(argv[i], "--audio-freq") == 0 && i + 1 < argc) {
            audio_freq = atof(argv[++i]);
        } else if (strcmp(argv[i], "--audio-level") == 0 && i + 1 < argc) {
            audio_level_db = atof(argv[++i]);
        } else if (strcmp(argv[i], "--audio-channels") == 0 && i + 1 < argc) {
            audio_channels = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--audio-rate") == 0 && i + 1 < argc) {
            audio_sample_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-video") == 0) {
            no_video = 1;
        } else if (strcmp(argv[i], "--clock-video") == 0) {
            clock_video = 1;
        } else if (strcmp(argv[i], "--no-clock-video") == 0) {
            clock_video = 0;
        } else if (strcmp(argv[i], "--clock-audio") == 0) {
            clock_audio = 1;
        } else if (strcmp(argv[i], "--connections") == 0) {
            show_connections = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--metadata") == 0 && i + 1 < argc) {
            metadata_xml = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "ndi-testgen — NDI test pattern generator\n\n"
                "Usage: %s [options]\n\n"
                "Options:\n"
                "  --name NAME         NDI source name (default: \"Test Pattern\")\n"
                "  --groups GROUPS     NDI groups (default: all)\n"
                "  --pattern PAT       Pattern: smpte75 smpte100 gradient red green\n"
                "                      blue white black moving clock checkers\n"
                "  --width W           Width (default: 1920)\n"
                "  --height H          Height (default: 1080)\n"
                "  --fps N             Frame rate numerator (default: 30000)\n"
                "  --fps-d D           Frame rate denominator (default: 1001)\n"
                "  --audio             Enable 1kHz test tone\n"
                "  --audio-freq F      Audio frequency Hz (default: 1000)\n"
                "  --audio-level dB    Audio level dBFS (default: -20)\n"
                "  --audio-channels N  Audio channels (default: 2)\n"
                "  --audio-rate R      Audio sample rate (default: 48000)\n"
                "  --no-video          Audio-only source\n"
                "  --no-clock-video    Don't clock video to frame rate\n"
                "  --clock-audio       Clock audio to sample rate\n"
                "  --connections       Print connection count every second\n"
                "  --json              Output JSON status each frame\n"
                "  --once              Send one frame then exit\n"
                "  --duration SECS     Run for N seconds (0 = forever)\n"
                "  --metadata XML      Send metadata on connect\n"
                "  --help              This help\n\n"
                "Examples:\n"
                "  %s --pattern smpte75 --audio\n"
                "  %s --pattern clock --connections --json\n"
                "  %s --name \"Camera 1\" --pattern moving --fps 60000 --fps-d 1001\n",
                argv[0], argv[0], argv[0], argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* Validate */
    if (width < 1 || height < 1) {
        fprintf(stderr, "Error: invalid resolution %dx%d\n", width, height);
        return 1;
    }
    if (fps_n < 1 || fps_d < 1) {
        fprintf(stderr, "Error: invalid frame rate %d/%d\n", fps_n, fps_d);
        return 1;
    }
    if (audio_channels < 1 || audio_channels > 64) {
        fprintf(stderr, "Error: invalid audio channels %d\n", audio_channels);
        return 1;
    }

    /* Initialize NDI */
    if (!NDIlib_initialize()) {
        fprintf(stderr, "Failed to initialize NDI SDK\n");
        return 1;
    }

    const char *sdk_version = NDIlib_version();
    fprintf(stderr, "ndi-testgen: NDI SDK %s\n", sdk_version);
    fprintf(stderr, "ndi-testgen: Source name=\"%s\", pattern=%s, resolution=%dx%d, fps=%d/%d\n",
            source_name, pattern_names[pattern], width, height, fps_n, fps_d);
    if (enable_audio) {
        fprintf(stderr, "ndi-testgen: Audio: %dHz %dCH, %.0fHz tone at %.1fdBFS\n",
                audio_sample_rate, audio_channels, audio_freq, audio_level_db);
    }

    /* Create sender */
    const NDIlib_send_create_t send_create = {
        .p_ndi_name = source_name,
        .p_groups = groups,
        .clock_video = clock_video,
        .clock_audio = clock_audio
    };

    g_sender = NDIlib_send_create(&send_create);
    if (!g_sender) {
        fprintf(stderr, "Failed to create NDI sender\n");
        NDIlib_destroy();
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Add connection metadata if provided */
    if (metadata_xml) {
        NDIlib_metadata_frame_t meta = {
            .length = 0,
            .timecode = NDIlib_send_timecode_synthesize,
            .p_data = (char *)metadata_xml
        };
        NDIlib_send_add_connection_metadata(g_sender, &meta);
    }

    /* Allocate video frame buffer */
    uint8_t *video_buf = NULL;
    if (!no_video) {
        video_buf = (uint8_t *)malloc(width * height * 4);  /* BGRA */
        if (!video_buf) {
            fprintf(stderr, "Failed to allocate video buffer (%dx%d)\n", width, height);
            NDIlib_send_destroy(g_sender);
            NDIlib_destroy();
            return 1;
        }
    }

    /* Audio buffer — for 48kHz stereo we send ~1600 samples per video frame at 30fps */
    float *audio_buf = NULL;
    int audio_samples_per_frame = 0;
    int64_t audio_sample_offset = 0;
    if (enable_audio) {
        /* Calculate samples per frame: sample_rate / (fps_n / fps_d) */
        audio_samples_per_frame = (int)((double)audio_sample_rate * (double)fps_d / (double)fps_n);
        if (audio_samples_per_frame < 1) audio_samples_per_frame = 1600;
        audio_buf = (float *)malloc(audio_samples_per_frame * audio_channels * sizeof(float));
        if (!audio_buf) {
            fprintf(stderr, "Failed to allocate audio buffer\n");
            free(video_buf);
            NDIlib_send_destroy(g_sender);
            NDIlib_destroy();
            return 1;
        }
    }

    int frame_count = 0;
    time_t start_time = time(NULL);
    time_t last_conn_time = 0;

    fprintf(stderr, "ndi-testgen: Running (Ctrl+C to stop)\n");

    while (g_running) {
        /* Check duration limit */
        if (duration_secs > 0) {
            time_t elapsed = time(NULL) - start_time;
            if (elapsed >= duration_secs) {
                fprintf(stderr, "ndi-testgen: Duration %ds reached\n", duration_secs);
                break;
            }
        }

        /* Send video frame */
        if (!no_video && video_buf) {
            draw_pattern(video_buf, width, height, pattern, frame_count);

            NDIlib_video_frame_v2_t video_frame = {
                .xres = width,
                .yres = height,
                .FourCC = NDIlib_FourCC_video_type_BGRX,
                .frame_rate_N = fps_n,
                .frame_rate_D = fps_d,
                .picture_aspect_ratio = 0.0f,  /* square pixels */
                .frame_format_type = NDIlib_frame_format_type_progressive,
                .timecode = NDIlib_send_timecode_synthesize,
                .p_data = video_buf,
                .line_stride_in_bytes = width * 4,
                .p_metadata = NULL,
                .timestamp = 0
            };

            NDIlib_send_send_video_v2(g_sender, &video_frame);
        }

        /* Send audio frame */
        if (enable_audio && audio_buf) {
            generate_audio_tone(audio_buf, audio_samples_per_frame, audio_channels,
                              audio_sample_rate, audio_freq, audio_level_db,
                              audio_sample_offset);

            /* Fill as planar FLTP:
               channel 0: buf[0..no_samples-1]
               channel 1: buf[no_samples..2*no_samples-1]
               channel_stride_in_bytes = no_samples * sizeof(float) */
            float amp = db_to_linear(audio_level_db);
            double phase_per_sample = 2.0 * M_PI * audio_freq / audio_sample_rate;
            for (int ch = 0; ch < audio_channels; ch++) {
                for (int s = 0; s < audio_samples_per_frame; s++) {
                    double t = phase_per_sample * (double)(audio_sample_offset + s);
                    float sample = (float)(amp * sin(t));
                    audio_buf[ch * audio_samples_per_frame + s] = sample;
                }
            }

            NDIlib_audio_frame_v2_t audio_frame = {
                .sample_rate = audio_sample_rate,
                .no_channels = audio_channels,
                .no_samples = audio_samples_per_frame,
                .timecode = NDIlib_send_timecode_synthesize,
                .p_data = audio_buf,
                .channel_stride_in_bytes = audio_samples_per_frame * sizeof(float),
                .p_metadata = NULL,
                .timestamp = 0
            };

            NDIlib_send_send_audio_v2(g_sender, &audio_frame);
            audio_sample_offset += audio_samples_per_frame;
        }

        /* Connection count reporting */
        if (show_connections) {
            time_t now = time(NULL);
            if (now != last_conn_time) {
                last_conn_time = now;
                int conns = NDIlib_send_get_no_connections(g_sender, 0);
                fprintf(stderr, "ndi-testgen: Connections: %d\n", conns);
            }
        }

        /* JSON status output */
        if (json_output) {
            json_object *root = json_object_new_object();
            json_object_object_add(root, "frame", json_object_new_int(frame_count));
            json_object_object_add(root, "pattern", json_object_new_string(pattern_names[pattern]));
            json_object_object_add(root, "width", json_object_new_int(width));
            json_object_object_add(root, "height", json_object_new_int(height));
            json_object_object_add(root, "fps_n", json_object_new_int(fps_n));
            json_object_object_add(root, "fps_d", json_object_new_int(fps_d));
            json_object_object_add(root, "source_name", json_object_new_string(source_name));
            json_object_object_add(root, "audio_enabled", json_object_new_boolean(enable_audio));
            json_object_object_add(root, "connections",
                json_object_new_int(NDIlib_send_get_no_connections(g_sender, 0)));
            json_object_object_add(root, "timestamp", json_object_new_int64((int64_t)time(NULL)));

            printf("%s\n", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
            fflush(stdout);
            json_object_put(root);
        }

        frame_count++;

        if (once) break;

        /* If not clocked, we need to sleep to maintain frame rate */
        if (!clock_video && !no_video) {
            double frame_duration_us = 1000000.0 * (double)fps_d / (double)fps_n;
            usleep((useconds_t)frame_duration_us);
        } else if (no_video && !clock_audio) {
            /* Audio-only, no clocking — sleep to avoid spinning */
            usleep(1000);  /* 1ms */
        }
    }

    /* Cleanup */
    fprintf(stderr, "ndi-testgen: Sent %d frames, shutting down\n", frame_count);

    if (video_buf) free(video_buf);
    if (audio_buf) free(audio_buf);

    NDIlib_send_destroy(g_sender);
    NDIlib_destroy();

    return 0;
}