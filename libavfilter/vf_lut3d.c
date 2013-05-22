/*
 * Copyright (c) 2013 Clément Bœsch
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * 3D Lookup table filter
 */

#include "libavutil/opt.h"
#include "libavutil/file.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define R 0
#define G 1
#define B 2
#define A 3

enum interp_mode {
    INTERPOLATE_NEAREST,
    INTERPOLATE_TRILINEAR,
    INTERPOLATE_TETRAHEDRAL,
    NB_INTERP_MODE
};

struct rgbvec {
    float r, g, b;
};

#define MAX_LEVEL 36

typedef struct LUT3DContext {
    const AVClass *class;
    enum interp_mode interpolation;
    char *file;
    uint8_t rgba_map[4];
    int step;
    int is16bit;
    struct rgbvec (*interp_8) (const struct LUT3DContext*, uint8_t,  uint8_t,  uint8_t);
    struct rgbvec (*interp_16)(const struct LUT3DContext*, uint16_t, uint16_t, uint16_t);
    struct rgbvec lut[MAX_LEVEL][MAX_LEVEL][MAX_LEVEL];
    int lutsize;
} LUT3DContext;

#define OFFSET(x) offsetof(LUT3DContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption lut3d_options[] = {
    { "file",   "set 3D LUT file name", OFFSET(file), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { "interp", "select interpolation mode", OFFSET(interpolation), AV_OPT_TYPE_INT, {.i64=INTERPOLATE_TETRAHEDRAL}, 0, NB_INTERP_MODE-1, FLAGS, "interp_mode" },
        { "nearest",     "use values from the nearest defined points",            0, AV_OPT_TYPE_CONST, {.i64=INTERPOLATE_NEAREST},     INT_MIN, INT_MAX, FLAGS, "interp_mode" },
        { "trilinear",   "interpolate values using the 8 points defining a cube", 0, AV_OPT_TYPE_CONST, {.i64=INTERPOLATE_TRILINEAR},   INT_MIN, INT_MAX, FLAGS, "interp_mode" },
        { "tetrahedral", "interpolate values using a tetrahedron",                0, AV_OPT_TYPE_CONST, {.i64=INTERPOLATE_TETRAHEDRAL}, INT_MIN, INT_MAX, FLAGS, "interp_mode" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(lut3d);

static inline float lerpf(float v0, float v1, float f)
{
    return v0 + (v1 - v0) * f;
}

static inline struct rgbvec lerp(const struct rgbvec *v0, const struct rgbvec *v1, float f)
{
    struct rgbvec v = {
        lerpf(v0->r, v1->r, f), lerpf(v0->g, v1->g, f), lerpf(v0->b, v1->b, f)
    };
    return v;
}

#define NEAR(x) ((int)((x) + .5))
#define PREV(x) ((int)(x))
#define NEXT(x) ((int)(x) + 1)

/**
 * Get the nearest defined point
 */
static inline struct rgbvec interp_nearest(const LUT3DContext *lut3d,
                                           const struct rgbvec *s)
{
    return lut3d->lut[NEAR(s->r)][NEAR(s->g)][NEAR(s->b)];
}

/**
 * Interpolate using the 8 vertices of a cube
 * @see https://en.wikipedia.org/wiki/Trilinear_interpolation
 */
static inline struct rgbvec interp_trilinear(const LUT3DContext *lut3d,
                                             const struct rgbvec *s)
{
    const struct rgbvec d = {s->r - PREV(s->r), s->g - PREV(s->g), s->b - PREV(s->b)};
    const struct rgbvec c000 = lut3d->lut[PREV(s->r)][PREV(s->g)][PREV(s->b)];
    const struct rgbvec c001 = lut3d->lut[PREV(s->r)][PREV(s->g)][NEXT(s->b)];
    const struct rgbvec c010 = lut3d->lut[PREV(s->r)][NEXT(s->g)][PREV(s->b)];
    const struct rgbvec c011 = lut3d->lut[PREV(s->r)][NEXT(s->g)][NEXT(s->b)];
    const struct rgbvec c100 = lut3d->lut[NEXT(s->r)][PREV(s->g)][PREV(s->b)];
    const struct rgbvec c101 = lut3d->lut[NEXT(s->r)][PREV(s->g)][NEXT(s->b)];
    const struct rgbvec c110 = lut3d->lut[NEXT(s->r)][NEXT(s->g)][PREV(s->b)];
    const struct rgbvec c111 = lut3d->lut[NEXT(s->r)][NEXT(s->g)][NEXT(s->b)];
    const struct rgbvec c00  = lerp(&c000, &c100, d.r);
    const struct rgbvec c10  = lerp(&c010, &c110, d.r);
    const struct rgbvec c01  = lerp(&c001, &c101, d.r);
    const struct rgbvec c11  = lerp(&c011, &c111, d.r);
    const struct rgbvec c0   = lerp(&c00,  &c10,  d.g);
    const struct rgbvec c1   = lerp(&c01,  &c11,  d.g);
    const struct rgbvec c    = lerp(&c0,   &c1,   d.b);
    return c;
}

/**
 * Tetrahedral interpolation. Based on code found in Truelight Software Library paper.
 * @see http://www.filmlight.ltd.uk/pdf/whitepapers/FL-TL-TN-0057-SoftwareLib.pdf
 */
static inline struct rgbvec interp_tetrahedral(const LUT3DContext *lut3d,
                                               const struct rgbvec *s)
{
    const struct rgbvec d = {s->r - PREV(s->r), s->g - PREV(s->g), s->b - PREV(s->b)};
    const struct rgbvec c000 = lut3d->lut[PREV(s->r)][PREV(s->g)][PREV(s->b)];
    const struct rgbvec c001 = lut3d->lut[PREV(s->r)][PREV(s->g)][NEXT(s->b)];
    const struct rgbvec c010 = lut3d->lut[PREV(s->r)][NEXT(s->g)][PREV(s->b)];
    const struct rgbvec c011 = lut3d->lut[PREV(s->r)][NEXT(s->g)][NEXT(s->b)];
    const struct rgbvec c100 = lut3d->lut[NEXT(s->r)][PREV(s->g)][PREV(s->b)];
    const struct rgbvec c101 = lut3d->lut[NEXT(s->r)][PREV(s->g)][NEXT(s->b)];
    const struct rgbvec c110 = lut3d->lut[NEXT(s->r)][NEXT(s->g)][PREV(s->b)];
    const struct rgbvec c111 = lut3d->lut[NEXT(s->r)][NEXT(s->g)][NEXT(s->b)];
    struct rgbvec c;
    if (d.r > d.g) {
        if (d.g > d.b) {
            c.r = (1-d.r) * c000.r + (d.r-d.g) * c100.r + (d.g-d.b) * c110.r + (d.b) * c111.r;
            c.g = (1-d.r) * c000.g + (d.r-d.g) * c100.g + (d.g-d.b) * c110.g + (d.b) * c111.g;
            c.b = (1-d.r) * c000.b + (d.r-d.g) * c100.b + (d.g-d.b) * c110.b + (d.b) * c111.b;
        } else if (d.r > d.b) {
            c.r = (1-d.r) * c000.r + (d.r-d.b) * c100.r + (d.b-d.g) * c101.r + (d.g) * c111.r;
            c.g = (1-d.r) * c000.g + (d.r-d.b) * c100.g + (d.b-d.g) * c101.g + (d.g) * c111.g;
            c.b = (1-d.r) * c000.b + (d.r-d.b) * c100.b + (d.b-d.g) * c101.b + (d.g) * c111.b;
        } else {
            c.r = (1-d.b) * c000.r + (d.b-d.r) * c001.r + (d.r-d.g) * c101.r + (d.g) * c111.r;
            c.g = (1-d.b) * c000.g + (d.b-d.r) * c001.g + (d.r-d.g) * c101.g + (d.g) * c111.g;
            c.b = (1-d.b) * c000.b + (d.b-d.r) * c001.b + (d.r-d.g) * c101.b + (d.g) * c111.b;
        }
    } else {
        if (d.b > d.g) {
            c.r = (1-d.b) * c000.r + (d.b-d.g) * c001.r + (d.g-d.r) * c011.r + (d.r) * c111.r;
            c.g = (1-d.b) * c000.g + (d.b-d.g) * c001.g + (d.g-d.r) * c011.g + (d.r) * c111.g;
            c.b = (1-d.b) * c000.b + (d.b-d.g) * c001.b + (d.g-d.r) * c011.b + (d.r) * c111.b;
        } else if (d.b > d.r) {
            c.r = (1-d.g) * c000.r + (d.g-d.b) * c010.r + (d.b-d.r) * c011.r + (d.r) * c111.r;
            c.g = (1-d.g) * c000.g + (d.g-d.b) * c010.g + (d.b-d.r) * c011.g + (d.r) * c111.g;
            c.b = (1-d.g) * c000.b + (d.g-d.b) * c010.b + (d.b-d.r) * c011.b + (d.r) * c111.b;
        } else {
            c.r = (1-d.g) * c000.r + (d.g-d.r) * c010.r + (d.r-d.b) * c110.r + (d.b) * c111.r;
            c.g = (1-d.g) * c000.g + (d.g-d.r) * c010.g + (d.r-d.b) * c110.g + (d.b) * c111.g;
            c.b = (1-d.g) * c000.b + (d.g-d.r) * c010.b + (d.r-d.b) * c110.b + (d.b) * c111.b;
        }
    }
    return c;
}

#define DEFINE_INTERP_FUNC(name, nbits)                                     \
static struct rgbvec interp_##nbits##_##name(const LUT3DContext *lut3d,     \
                                             uint##nbits##_t r,             \
                                             uint##nbits##_t g,             \
                                             uint##nbits##_t b)             \
{                                                                           \
    const float scale = (1. / ((1<<nbits) - 1)) * (lut3d->lutsize - 1);     \
    const struct rgbvec scaled_rgb = {r * scale, g * scale, b * scale};     \
    return interp_##name(lut3d, &scaled_rgb);                               \
}

DEFINE_INTERP_FUNC(nearest,     8)
DEFINE_INTERP_FUNC(trilinear,   8)
DEFINE_INTERP_FUNC(tetrahedral, 8)

DEFINE_INTERP_FUNC(nearest,     16)
DEFINE_INTERP_FUNC(trilinear,   16)
DEFINE_INTERP_FUNC(tetrahedral, 16)

#define MAX_LINE_SIZE 512

static int skip_line(const char *p)
{
    while (*p && av_isspace(*p))
        p++;
    return !*p || *p == '#';
}

#define NEXT_LINE(loop_cond) do {                           \
    if (!fgets(line, sizeof(line), f)) {                    \
        av_log(ctx, AV_LOG_ERROR, "Unexpected EOF\n");      \
        return AVERROR_INVALIDDATA;                         \
    }                                                       \
} while (loop_cond)

/* Basically r g and b float values on each line; seems to be generated by
 * Davinci */
static int parse_dat(AVFilterContext *ctx, FILE *f)
{
    LUT3DContext *lut3d = ctx->priv;
    const int size = lut3d->lutsize;
    int i, j, k;

    for (k = 0; k < size; k++) {
        for (j = 0; j < size; j++) {
            for (i = 0; i < size; i++) {
                char line[MAX_LINE_SIZE];
                struct rgbvec *vec = &lut3d->lut[k][j][i];
                NEXT_LINE(skip_line(line));
                sscanf(line, "%f %f %f", &vec->r, &vec->g, &vec->b);
            }
        }
    }
    return 0;
}

/* Iridas format */
static int parse_cube(AVFilterContext *ctx, FILE *f)
{
    LUT3DContext *lut3d = ctx->priv;
    char line[MAX_LINE_SIZE];
    float min[3] = {0.0, 0.0, 0.0};
    float max[3] = {1.0, 1.0, 1.0};

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "LUT_3D_SIZE ", 12)) {
            int i, j, k;
            const int size = strtol(line + 12, NULL, 0);

            if (size > MAX_LEVEL) {
                av_log(ctx, AV_LOG_ERROR, "Too large 3D LUT\n");
                return AVERROR(EINVAL);
            }
            lut3d->lutsize = size;
            for (k = 0; k < size; k++) {
                for (j = 0; j < size; j++) {
                    for (i = 0; i < size; i++) {
                        struct rgbvec *vec = &lut3d->lut[k][j][i];

                        do {
                            NEXT_LINE(0);
                            if (!strncmp(line, "DOMAIN_", 7)) {
                                float *vals = NULL;
                                if      (!strncmp(line + 7, "MIN ", 4)) vals = min;
                                else if (!strncmp(line + 7, "MAX ", 4)) vals = max;
                                if (!vals)
                                    return AVERROR_INVALIDDATA;
                                sscanf(line + 11, "%f %f %f", vals, vals + 1, vals + 2);
                                av_log(ctx, AV_LOG_DEBUG, "min: %f %f %f | max: %f %f %f\n",
                                       min[0], min[1], min[2], max[0], max[1], max[2]);
                                continue;
                            }
                        } while (skip_line(line));
                        if (sscanf(line, "%f %f %f", &vec->r, &vec->g, &vec->b) != 3)
                            return AVERROR_INVALIDDATA;
                        vec->r *= max[0] - min[0];
                        vec->g *= max[1] - min[1];
                        vec->b *= max[2] - min[2];
                    }
                }
            }
            break;
        }
    }
    return 0;
}

/* Assume 17x17x17 LUT with a 16-bit depth
 * FIXME: it seems there are various 3dl formats */
static int parse_3dl(AVFilterContext *ctx, FILE *f)
{
    char line[MAX_LINE_SIZE];
    LUT3DContext *lut3d = ctx->priv;
    int i, j, k;
    const int size = 17;
    const float scale = 16*16*16;

    lut3d->lutsize = size;
    if (!fgets(line, sizeof(line), f))
        return AVERROR_INVALIDDATA;
    for (k = 0; k < size; k++) {
        for (j = 0; j < size; j++) {
            for (i = 0; i < size; i++) {
                int r, g, b;
                struct rgbvec *vec = &lut3d->lut[k][j][i];

                NEXT_LINE(skip_line(line));
                if (sscanf(line, "%d %d %d", &r, &g, &b) != 3)
                    return AVERROR_INVALIDDATA;
                vec->r = r / scale;
                vec->g = g / scale;
                vec->b = b / scale;
            }
        }
    }
    return 0;
}

/* Pandora format */
static int parse_m3d(AVFilterContext *ctx, FILE *f)
{
    LUT3DContext *lut3d = ctx->priv;
    float scale;
    int i, j, k, size, in = -1, out = -1;
    char line[MAX_LINE_SIZE];
    uint8_t rgb_map[3] = {0, 1, 2};

    while (fgets(line, sizeof(line), f)) {
        if      (!strncmp(line, "in",  2)) in  = strtol(line + 2, NULL, 0);
        else if (!strncmp(line, "out", 3)) out = strtol(line + 3, NULL, 0);
        else if (!strncmp(line, "values", 6)) {
            const char *p = line + 6;
#define SET_COLOR(id) do {                  \
    while (av_isspace(*p))                  \
        p++;                                \
    switch (*p) {                           \
    case 'r': rgb_map[id] = 0; break;       \
    case 'g': rgb_map[id] = 1; break;       \
    case 'b': rgb_map[id] = 2; break;       \
    }                                       \
    while (*p && !av_isspace(*p))           \
        p++;                                \
} while (0)
            SET_COLOR(0);
            SET_COLOR(1);
            SET_COLOR(2);
            break;
        }
    }

    if (in == -1 || out == -1) {
        av_log(ctx, AV_LOG_ERROR, "in and out must be defined\n");
        return AVERROR_INVALIDDATA;
    }
    for (size = 1; size*size*size < in; size++);
    lut3d->lutsize = size;
    scale = 1. / (out - 1);

    for (k = 0; k < size; k++) {
        for (j = 0; j < size; j++) {
            for (i = 0; i < size; i++) {
                struct rgbvec *vec = &lut3d->lut[k][j][i];
                float val[3];

                NEXT_LINE(0);
                if (sscanf(line, "%f %f %f", val, val + 1, val + 2) != 3)
                    return AVERROR_INVALIDDATA;
                vec->r = val[rgb_map[0]] * scale;
                vec->g = val[rgb_map[1]] * scale;
                vec->b = val[rgb_map[2]] * scale;
            }
        }
    }
    return 0;
}

static void set_identity_matrix(LUT3DContext *lut3d, int size)
{
    int i, j, k;
    const float c = 1. / (size - 1);

    lut3d->lutsize = size;
    for (k = 0; k < size; k++) {
        for (j = 0; j < size; j++) {
            for (i = 0; i < size; i++) {
                struct rgbvec *vec = &lut3d->lut[k][j][i];
                vec->r = k * c;
                vec->g = j * c;
                vec->b = i * c;
            }
        }
    }
}

static av_cold int init(AVFilterContext *ctx)
{
    int ret;
    FILE *f;
    const char *ext;
    LUT3DContext *lut3d = ctx->priv;

    if (!lut3d->file) {
        set_identity_matrix(lut3d, 32);
        return 0;
    }

    f = fopen(lut3d->file, "r");
    if (!f) {
        ret = AVERROR(errno);
        av_log(ctx, AV_LOG_ERROR, "%s: %s\n", lut3d->file, av_err2str(ret));
        return ret;
    }

    ext = strrchr(lut3d->file, '.');
    if (!ext) {
        av_log(ctx, AV_LOG_ERROR, "Unable to guess the format from the extension\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    ext++;

    if (!av_strcasecmp(ext, "dat")) {
        lut3d->lutsize = 33;
        ret = parse_dat(ctx, f);
    } else if (!av_strcasecmp(ext, "3dl")) {
        ret = parse_3dl(ctx, f);
    } else if (!av_strcasecmp(ext, "cube")) {
        ret = parse_cube(ctx, f);
    } else if (!av_strcasecmp(ext, "m3d")) {
        ret = parse_m3d(ctx, f);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unrecognized '.%s' file type\n", ext);
        ret = AVERROR(EINVAL);
    }

    if (!ret && !lut3d->lutsize) {
        av_log(ctx, AV_LOG_ERROR, "3D LUT is empty\n");
        ret = AVERROR_INVALIDDATA;
    }

end:
    fclose(f);
    return ret;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24,  AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGBA,   AV_PIX_FMT_BGRA,
        AV_PIX_FMT_ARGB,   AV_PIX_FMT_ABGR,
        AV_PIX_FMT_0RGB,   AV_PIX_FMT_0BGR,
        AV_PIX_FMT_RGB0,   AV_PIX_FMT_BGR0,
        AV_PIX_FMT_RGB48,  AV_PIX_FMT_BGR48,
        AV_PIX_FMT_RGBA64, AV_PIX_FMT_BGRA64,
        AV_PIX_FMT_NONE
    };
    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    LUT3DContext *lut3d = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    switch (inlink->format) {
    case AV_PIX_FMT_RGB48:
    case AV_PIX_FMT_BGR48:
    case AV_PIX_FMT_RGBA64:
    case AV_PIX_FMT_BGRA64:
        lut3d->is16bit = 1;
    }

    ff_fill_rgba_map(lut3d->rgba_map, inlink->format);
    lut3d->step = av_get_padded_bits_per_pixel(desc) >> (3 + lut3d->is16bit);

#define SET_FUNC(name) do {                                     \
    if (lut3d->is16bit) lut3d->interp_16 = interp_16_##name;    \
    else                lut3d->interp_8  = interp_8_##name;     \
} while (0)

    switch (lut3d->interpolation) {
    case INTERPOLATE_NEAREST:     SET_FUNC(nearest);        break;
    case INTERPOLATE_TRILINEAR:   SET_FUNC(trilinear);      break;
    case INTERPOLATE_TETRAHEDRAL: SET_FUNC(tetrahedral);    break;
    default:
        av_assert0(0);
    }

    return 0;
}

#define FILTER(nbits) do {                                                                          \
    uint8_t       *dstrow = out->data[0];                                                           \
    const uint8_t *srcrow = in ->data[0];                                                           \
                                                                                                    \
    for (y = 0; y < inlink->h; y++) {                                                               \
        uint##nbits##_t *dst = (uint##nbits##_t *)dstrow;                                           \
        const uint##nbits##_t *src = (const uint##nbits##_t *)srcrow;                               \
        for (x = 0; x < inlink->w * step; x += step) {                                              \
            struct rgbvec vec = lut3d->interp_##nbits(lut3d, src[x + r], src[x + g], src[x + b]);   \
            dst[x + r] = av_clip_uint##nbits(vec.r * (float)((1<<nbits) - 1));                      \
            dst[x + g] = av_clip_uint##nbits(vec.g * (float)((1<<nbits) - 1));                      \
            dst[x + b] = av_clip_uint##nbits(vec.b * (float)((1<<nbits) - 1));                      \
            if (!direct && step == 4)                                                               \
                dst[x + a] = src[x + a];                                                            \
        }                                                                                           \
        dstrow += out->linesize[0];                                                                 \
        srcrow += in ->linesize[0];                                                                 \
    }                                                                                               \
} while (0)

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    int x, y, direct = 0;
    AVFilterContext *ctx = inlink->dst;
    LUT3DContext *lut3d = ctx->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFrame *out;
    const int step = lut3d->step;
    const uint8_t r = lut3d->rgba_map[R];
    const uint8_t g = lut3d->rgba_map[G];
    const uint8_t b = lut3d->rgba_map[B];
    const uint8_t a = lut3d->rgba_map[A];

    if (av_frame_is_writable(in)) {
        direct = 1;
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    if (lut3d->is16bit) FILTER(16);
    else                FILTER(8);

    if (!direct)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static const AVFilterPad lut3d_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad lut3d_outputs[] = {
     {
         .name = "default",
         .type = AVMEDIA_TYPE_VIDEO,
     },
     { NULL }
};

AVFilter avfilter_vf_lut3d = {
    .name          = "lut3d",
    .description   = NULL_IF_CONFIG_SMALL("Adjust colors using a 3D LUT."),
    .priv_size     = sizeof(LUT3DContext),
    .init          = init,
    .query_formats = query_formats,
    .inputs        = lut3d_inputs,
    .outputs       = lut3d_outputs,
    .priv_class    = &lut3d_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};