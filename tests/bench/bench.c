/*
 * This file is part of AtomGL.
 *
 * Copyright 2026 Tom Hoenderdos <tomhoenderdos@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dcs_lcd_draw.h"
#include "dcs_lcd_screen.h"
#include "display_items.h"
#include "shape.h"

#define SCREEN_SIZE 240
#define MAX_SCREEN_WIDTH 320
#define WARMUP_FRAMES 20
#define MIN_ELAPSED_US 500000.0
#define MAX_ITEMS 128
#define MAX_RATIOS 32

// 240 px * 16 bit at 40 MHz
#define SPI_US_PER_LINE 96.0
// 320 px * 16 bit at 80 MHz
#define SPI_US_PER_LINE_320 64.0

static uint16_t line_buf[MAX_SCREEN_WIDTH];
static struct DCSLCDScreen screen;
static uint64_t g_checksum = 0;

struct Ratio
{
    const char *name;
    double shape_us;
    double rect_us;
};

static struct Ratio ratios[MAX_RATIOS];
static int ratios_len = 0;

struct Scene
{
    BaseDisplayItem items[MAX_ITEMS];
    struct ShapeData *shapes[MAX_ITEMS];
    int len;
    int shapes_len;
};

static void render_frame(BaseDisplayItem items[], size_t len)
{
    static BaseDisplayItem *row[MAX_ITEMS];
    for (int ypos = 0; ypos < screen.h; ypos++) {
        size_t row_len = display_items_row(items, len, ypos, row);
        int xpos = 0;
        while (xpos < screen.w) {
            xpos += dcs_lcd_draw_x(&screen, xpos, ypos, row, row_len);
        }
        for (int i = 0; i < screen.w; i++) {
            g_checksum += line_buf[i];
        }
    }
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e6 + (double) ts.tv_nsec / 1e3;
}

static double run_bench(BaseDisplayItem items[], size_t len)
{
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        render_frame(items, len);
    }

    double start = now_us();
    long frames = 0;
    double elapsed;
    do {
        render_frame(items, len);
        frames++;
        elapsed = now_us() - start;
    } while (elapsed < MIN_ELAPSED_US);

    return elapsed / (double) frames;
}

static uint32_t rgba(uint32_t rgb)
{
    return (rgb << 8) | 0xFF;
}

static BaseDisplayItem *scene_item(struct Scene *scene)
{
    if (scene->len == MAX_ITEMS) {
        fprintf(stderr, "too many items in scene\n");
        exit(1);
    }
    BaseDisplayItem *item = &scene->items[scene->len++];
    memset(item, 0, sizeof(*item));
    return item;
}

static void add_rect(struct Scene *scene, int x, int y, int w, int h, uint32_t rgb)
{
    BaseDisplayItem *item = scene_item(scene);
    item->primitive = PrimitiveRect;
    item->x = x;
    item->y = y;
    item->width = w;
    item->height = h;
    item->brcolor = rgba(rgb);
}

static void add_background(struct Scene *scene)
{
    add_rect(scene, 0, 0, screen.w, screen.h, 0x202020);
}

static void add_shape(struct Scene *scene, struct ShapeData *shape, uint32_t rgb)
{
    if (shape == NULL) {
        fprintf(stderr, "invalid shape in scene\n");
        exit(1);
    }
    scene->shapes[scene->shapes_len++] = shape;
    BaseDisplayItem *item = scene_item(scene);
    item->primitive = PrimitiveShape;
    item->brcolor = rgba(rgb);
    item->data.shape_data.shape = shape;
    shape_bounds(shape, &item->x, &item->y, &item->width, &item->height);
}

static void add_text(struct Scene *scene, int x, int y, const char *text, uint32_t fg, bool bg)
{
    BaseDisplayItem *item = scene_item(scene);
    item->primitive = PrimitiveText;
    item->x = x;
    item->y = y;
    item->width = (int) strlen(text) * 8;
    item->height = 16;
    item->brcolor = bg ? rgba(0x303030) : 0;
    item->data.text_data.fgcolor = rgba(fg);
    item->data.text_data.text = text;
}

static void free_scene(struct Scene *scene)
{
    for (int i = 0; i < scene->shapes_len; i++) {
        shape_destroy(scene->shapes[i]);
    }
}

static void print_scene(const char *name, double us)
{
    double us_line = us / (double) screen.h;
    printf("%-26s %10.1f %9.2f\n", name, us, us_line);
}

static double bench_scene(const char *name, struct Scene *scene)
{
    double us = run_bench(scene->items, scene->len);
    print_scene(name, us);

    if (scene->shapes_len > 0) {
        BaseDisplayItem rects[MAX_ITEMS];
        memcpy(rects, scene->items, sizeof(rects[0]) * scene->len);
        for (int i = 0; i < scene->len; i++) {
            if (rects[i].primitive == PrimitiveShape) {
                rects[i].primitive = PrimitiveRect;
                rects[i].data.shape_data.shape = NULL;
            }
        }
        if (ratios_len < MAX_RATIOS) {
            ratios[ratios_len++] = (struct Ratio){ name, us, run_bench(rects, scene->len) };
        }
    }

    free_scene(scene);
    return us;
}

static void bench_sprite(const char *name, int scale, bool flip_x, bool flip_y)
{
    int side = SCREEN_SIZE / scale;
    uint8_t *pix = malloc((size_t) side * side * 4);
    for (int i = 0; i < side * side; i++) {
        pix[4 * i + 0] = (uint8_t) (i * 7);
        pix[4 * i + 1] = (uint8_t) (i * 13);
        pix[4 * i + 2] = (uint8_t) (i * 29);
        pix[4 * i + 3] = 0xFF;
    }

    struct Scene scene = { .len = 0, .shapes_len = 0 };
    BaseDisplayItem *item = scene_item(&scene);
    item->primitive = PrimitiveScaledCroppedImage;
    item->width = SCREEN_SIZE;
    item->height = SCREEN_SIZE;
    item->x_scale = scale;
    item->y_scale = scale;
    item->data.image_data_with_size.width = side;
    item->data.image_data_with_size.height = side;
    item->data.image_data_with_size.pix = (const char *) pix;
    item->flip_x = flip_x;
    item->flip_y = flip_y;
    add_background(&scene);

    bench_scene(name, &scene);
    free(pix);
}

static double bench_shape(const char *name, uint32_t rgb, struct ShapeData *shape)
{
    struct Scene scene = { 0 };
    add_shape(&scene, shape, rgb);
    add_background(&scene);
    return bench_scene(name, &scene);
}

#define RACER_W 320
#define RACER_H 240
#define RACER_HORIZON 96
#define RACER_BANDS 20

static int racer_half_width(int y)
{
    return 8 + (y - RACER_HORIZON) * 150 / (RACER_H - RACER_HORIZON);
}

static int racer_centre(int y)
{
    int d = RACER_H - y;
    return RACER_W / 2 + d * d / 400;
}

static void add_racer_band(struct Scene *scene, int y0, int y1, int from_frac, int to_frac,
    uint32_t rgb)
{
    int c0 = racer_centre(y0);
    int c1 = racer_centre(y1);
    int w0 = racer_half_width(y0);
    int w1 = racer_half_width(y1);
    struct ShapePoint pts[] = {
        { c0 + w0 * from_frac / 100, y0 },
        { c0 + w0 * to_frac / 100 + 1, y0 },
        { c1 + w1 * to_frac / 100 + 1, y1 },
        { c1 + w1 * from_frac / 100, y1 }
    };
    add_shape(scene, shape_new_polygon(pts, 4), rgb);
}

static void bench_racer(void)
{
    screen.w = RACER_W;
    screen.h = RACER_H;

    static uint8_t skyline[160 * 24 * 4];
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 160; x++) {
            uint8_t *p = &skyline[4 * (y * 160 + x)];
            int top = 4 + (x * 7 % 13) + (x / 20 % 2) * 6;
            p[0] = (uint8_t) (40 + x);
            p[1] = (uint8_t) (60 + y * 3);
            p[2] = 90;
            p[3] = y >= top ? 0xFF : 0;
        }
    }

    struct Scene scene = { 0 };

    add_text(&scene, 4, 2, "LAP 2/3", 0xFFFFFF, false);
    add_text(&scene, 124, 2, "00:41.27", 0xFFFF00, false);
    add_text(&scene, 252, 2, "POS 3", 0xFFFFFF, false);
    add_text(&scene, 4, 20, "BEST 00:40.10", 0xC0C0C0, false);
    add_text(&scene, 244, 20, "123 km/h", 0xC0C0C0, false);

    add_rect(&scene, 136, 200, 48, 18, 0xD02020);
    add_rect(&scene, 148, 190, 24, 10, 0x802020);
    add_rect(&scene, 130, 212, 10, 12, 0x101010);
    add_rect(&scene, 180, 212, 10, 12, 0x101010);
    add_rect(&scene, 196, 150, 24, 9, 0x2040D0);
    add_rect(&scene, 202, 145, 12, 5, 0x102080);
    add_rect(&scene, 193, 156, 5, 6, 0x101010);
    add_rect(&scene, 218, 156, 5, 6, 0x101010);

    for (int b = 0; b < RACER_BANDS; b++) {
        int y0 = RACER_HORIZON + b * (RACER_H - RACER_HORIZON) / RACER_BANDS;
        int y1 = RACER_HORIZON + (b + 1) * (RACER_H - RACER_HORIZON) / RACER_BANDS;
        add_racer_band(&scene, y0, y1, -3, 3, b % 2 ? 0xFFFFFF : 0x606060);
        add_racer_band(&scene, y0, y1, -100, 100, 0x606060);
        add_racer_band(&scene, y0, y1, -115, 115, b % 2 ? 0xFFFFFF : 0xD02020);
    }
    for (int b = 0; b < RACER_BANDS; b++) {
        int y0 = RACER_HORIZON + b * (RACER_H - RACER_HORIZON) / RACER_BANDS;
        int y1 = RACER_HORIZON + (b + 1) * (RACER_H - RACER_HORIZON) / RACER_BANDS;
        add_rect(&scene, 0, y0, RACER_W, y1 - y0, b % 2 ? 0x208020 : 0x30A030);
    }

    BaseDisplayItem *item = scene_item(&scene);
    item->primitive = PrimitiveScaledCroppedImage;
    item->x = 0;
    item->y = RACER_HORIZON - 24;
    item->width = RACER_W;
    item->height = 24;
    item->x_scale = 2;
    item->y_scale = 1;
    item->data.image_data_with_size.width = 160;
    item->data.image_data_with_size.height = 24;
    item->data.image_data_with_size.pix = (const char *) skyline;

    add_rect(&scene, 0, 0, RACER_W, RACER_HORIZON, 0x60A0E0);

    bench_scene("racer (95 items, 320x240)", &scene);

    screen.w = SCREEN_SIZE;
    screen.h = SCREEN_SIZE;
}

static const char *const labels[] = {
    "Temperature", "Humidity", "Pressure", "Wind", "Rain", "UV index",
    "Sunrise", "Sunset", "Battery", "WiFi", "Uptime", "Free heap"
};
static const char *const values[] = {
    "21.5 C", "48 %", "1013 hPa", "12 km/h", "0.2 mm", "3",
    "07:31", "19:02", "87 %", "-61 dBm", "3d 04h", "112 KiB"
};

int main(void)
{
    memset(&screen, 0, sizeof(screen));
    screen.w = SCREEN_SIZE;
    screen.h = SCREEN_SIZE;
    screen.pixels = line_buf;

    printf("%-26s %10s %9s\n", "scene", "us/frame", "us/line");

    {
        struct Scene scene = { 0 };
        add_background(&scene);
        bench_scene("background only", &scene);
    }

    bench_shape("rounded_rect", 0x3060C0, shape_new_rounded_rect(70, 70, 100, 100, 12));
    bench_shape("circle", 0x20C040, shape_new_ellipse(120, 120, 50, 50));
    bench_shape("ellipse", 0xC04080, shape_new_ellipse(120, 120, 80, 40));
    bench_shape("thin line", 0xFFCC00, shape_new_line(10, 115, 230, 125, 1));
    bench_shape("diagonal thick line", 0xFFCC00, shape_new_line(20, 20, 220, 220, 3));
    bench_shape("arc gauge", 0x00C0C0, shape_new_arc(120, 120, 60, 10, 0, 270));
    bench_shape("ring", 0x00C0C0, shape_new_arc(120, 120, 60, 10, 0, 360));
    bench_shape("full-screen circle", 0x20C040, shape_new_ellipse(120, 120, 119, 119));

    {
        static const struct ShapePoint star[] = {
            { 120, 20 }, { 150, 95 }, { 230, 95 }, { 165, 140 }, { 190, 220 },
            { 120, 170 }, { 50, 220 }, { 75, 140 }, { 10, 95 }, { 90, 95 }
        };
        bench_shape("star polygon", 0xE06020, shape_new_polygon(star, sizeof(star) / sizeof(star[0])));
    }

    {
        struct Scene scene = { 0 };
        for (int j = 0; j < 4; j++) {
            for (int i = 0; i < 5; i++) {
                add_shape(&scene, shape_new_ellipse(30 + 45 * i, 40 + 50 * j, 8, 8), 0x4080FF);
            }
        }
        add_background(&scene);
        bench_scene("20 bullets", &scene);
    }

    {
        struct Scene scene = { 0 };
        for (int j = 0; j < 5; j++) {
            for (int i = 0; i < 10; i++) {
                add_shape(&scene, shape_new_ellipse(20 + 22 * i, 30 + 45 * j, 16, 16), 0x4080FF + 0x10 * i);
            }
        }
        add_background(&scene);
        bench_scene("50 overlapping bullets", &scene);
    }

    {
        struct Scene scene = { 0 };
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 2; c++) {
                add_shape(&scene, shape_new_rounded_rect(20 + 110 * c, 20 + 70 * r, 90, 50, 10), 0x3060C0);
            }
        }
        add_background(&scene);
        bench_scene("6 buttons", &scene);
    }

    {
        struct Scene scene = { 0 };
        for (int i = 0; i < 12; i++) {
            add_shape(&scene, shape_new_line(0, 10 + 20 * i, 239, 230 - 20 * i, 1 + i % 4), 0xFFCC00);
        }
        add_background(&scene);
        bench_scene("12 crossing lines", &scene);
    }

    {
        struct Scene scene = { 0 };
        for (int i = 0; i < 5; i++) {
            add_shape(&scene, shape_new_arc(120, 120, 30 + 20 * i, 8, 30 * i, 30 * i + 270), 0x00C0C0);
        }
        add_background(&scene);
        bench_scene("5 concentric arcs", &scene);
    }

    double comb_us;
    double box_us;
    {
        struct ShapePoint comb[SHAPE_POLYGON_MAX_POINTS];
        int teeth = SHAPE_POLYGON_MAX_POINTS / 4;
        for (int t = 0; t < teeth; t++) {
            int x0 = 10 + (2 * t) * 220 / (2 * teeth);
            int x1 = 10 + (2 * t + 1) * 220 / (2 * teeth);
            comb[4 * t + 0] = (struct ShapePoint){ x0, 230 };
            comb[4 * t + 1] = (struct ShapePoint){ x0, 10 };
            comb[4 * t + 2] = (struct ShapePoint){ x1 + 1, 10 };
            comb[4 * t + 3] = (struct ShapePoint){ x1 + 1, 230 };
        }
        static char comb_name[32];
        snprintf(comb_name, sizeof(comb_name), "%d-point comb polygon", SHAPE_POLYGON_MAX_POINTS);
        struct ShapeData *s = shape_new_polygon(comb, SHAPE_POLYGON_MAX_POINTS);
        int bx, by, bw, bh;
        shape_bounds(s, &bx, &by, &bw, &bh);
        comb_us = bench_shape(comb_name, 0xE06020, s);
        struct ShapePoint box[] = { { bx, by }, { bx + bw, by }, { bx + bw, by + bh }, { bx, by + bh } };
        box_us = bench_shape("4-point polygon, comb bbox", 0xE06020, shape_new_polygon(box, 4));
    }

    {
        struct Scene scene = { 0 };
        add_text(&scene, 8, 0, "Weather station", 0xFFFFFF, false);
        for (int i = 0; i < 12; i++) {
            add_text(&scene, 8, 20 + 18 * i, labels[i], 0xC0C0C0, false);
            add_text(&scene, 136, 20 + 18 * i, values[i], 0xFFCC00, i % 2 == 0);
        }
        add_rect(&scene, 0, 0, SCREEN_SIZE, 17, 0x3060C0);
        add_background(&scene);
        bench_scene("text UI (25 text items)", &scene);
    }

    bench_sprite("sprite x1", 1, false, false);
    bench_sprite("sprite x3", 3, false, false);
    bench_sprite("sprite x1 flip x", 1, true, false);
    bench_sprite("sprite x1 flip xy", 1, true, true);
    bench_sprite("sprite x3 flip x", 3, true, false);
    bench_sprite("sprite x3 flip xy", 3, true, true);

    bench_racer();

    printf("\nshapes vs. rects of their bounding boxes\n");
    printf("%-26s %10s %10s %7s\n", "scene", "shape us", "rect us", "ratio");
    for (int i = 0; i < ratios_len; i++) {
        printf("%-26s %10.1f %10.1f %7.2f\n", ratios[i].name, ratios[i].shape_us, ratios[i].rect_us,
            ratios[i].shape_us / ratios[i].rect_us);
    }

    printf("\n%d-point comb over a 4-point polygon of its bbox: %.2fx\n", SHAPE_POLYGON_MAX_POINTS,
        comb_us / box_us);

    printf("\nchecksum: %llu\n", (unsigned long long) g_checksum);
    printf("\nHost numbers are only meaningful relative to each other: the host CPU\n"
           "is not an ESP32-S3. On the device a line takes width * 16 bit / SPI\n"
           "clock to send, %.0f us for 240 px at 40 MHz and %.0f us for 320 px at\n"
           "80 MHz, and drawing the next line is hidden behind that transfer as\n"
           "long as it is faster. Measure on the device with ATOMGL_PROFILE, see\n"
           "tests/bench/README.md.\n",
        SPI_US_PER_LINE, SPI_US_PER_LINE_320);

    return 0;
}
