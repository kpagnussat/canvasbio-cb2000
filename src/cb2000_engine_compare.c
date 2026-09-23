/*
 * CanvasBio CB2000: the engine's extract and compare calls
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The Windows engine never matches the extractor's image object directly.
 * An extracted image is serialized into a node record (the form templates
 * store), and the compare decodes the records again: the decoded image has
 * no pixel buffer and fresh block records. Every template node is then
 * paired, scored and decided against the probe, largest record first, and
 * the result only says whether any node matched.
 */

#include "cb2000_engine.h"
#include "cb2000_engine_bytes.h"

#include <string.h>

#define BLOCK   CB2000_ENGINE_BLOCK_SIZE

/* Probes with fewer foreground pixels are not extracted. */
#define EXTRACT_MIN_FEATURE_SCORE   640
/* Pairs a node needs before it is scored. */
#define COMPARE_MIN_PAIRS           5
/* Probe coverage (percent of the frame) that makes a failed compare a
 * "no match" rather than a "poor sample". */
#define NO_MATCH_MIN_COVERAGE       40
/* The only scratch mark that survives between pairing calls. */
#define KEYPOINT_KEPT_FLAGS         CB2000_ENGINE_KP_EDGE

#define MINUTIA_RECORD_SIZE     10
#define KEYPOINT_RECORD_SIZE    34

/*
 * Largest side a node record may claim. A node is not bound to the frame:
 * a merge rotates the touch onto the node and stores their union, which
 * grows past 80x64. This is a sanity bound on untrusted input, not a vendor
 * rule; it is far above any union the enrollment produces, and it keeps the
 * squared keypoint distances of the pairing inside 32 bits.
 */
#define NODE_MAX_SIDE           1024

/*
 * Node record layout, little-endian, packed:
 *   u16 width, u16 height
 *   u32 ridge plane, u32 masked plane   (words per row * rows each)
 *   i32 featureScore
 *   i32 minutia count,  10 bytes each: i16 x, i16 y, i32 angle, i16 pattern
 *   i32 dark-centre keypoint count,   34 bytes each (below)
 *   i32 bright-centre keypoint count, 34 bytes each
 * Keypoint: i16 reserved (always 0 from the extractor), i16 x10, i16 y10,
 *   u8 scale, i8 polarity, i16 orientation, u8 descriptor[24].
 */

static void
put_keypoints(GByteArray *b, const GArray *list)
{
    cb2000_put_u32(b, list->len);
    for (guint i = 0; i < list->len; i++) {
        const Cb2000EngineKeypoint *kp = &g_array_index(list, Cb2000EngineKeypoint, i);
        const guint8 scale = kp->scale;
        const guint8 polarity = (guint8) kp->polarity;

        cb2000_put_u16(b, 0);
        cb2000_put_u16(b, (guint16) kp->x10);
        cb2000_put_u16(b, (guint16) kp->y10);
        g_byte_array_append(b, &scale, 1);
        g_byte_array_append(b, &polarity, 1);
        cb2000_put_u16(b, (guint16) kp->orientation);
        g_byte_array_append(b, kp->descriptor, CB2000_ENGINE_DESCRIPTOR_SIZE);
    }
}

GBytes *
cb2000_engine_node_encode(const Cb2000EngineImage *img)
{
    const gint n_words = img->plane_words * img->height;
    GByteArray *b = g_byte_array_new();

    cb2000_put_u16(b, (guint16) img->width);
    cb2000_put_u16(b, (guint16) img->height);
    for (gint i = 0; i < n_words; i++)
        cb2000_put_u32(b, img->plane_ridge[i]);
    for (gint i = 0; i < n_words; i++)
        cb2000_put_u32(b, img->plane_masked[i]);
    cb2000_put_u32(b, (guint32) img->feature_score);

    cb2000_put_u32(b, img->minutiae->len);
    for (guint i = 0; i < img->minutiae->len; i++) {
        const Cb2000EngineMinutia *m = &g_array_index(img->minutiae, Cb2000EngineMinutia, i);

        cb2000_put_u16(b, (guint16) m->x);
        cb2000_put_u16(b, (guint16) m->y);
        cb2000_put_u32(b, (guint32) m->angle);
        cb2000_put_u16(b, (guint16) m->pattern);
    }
    put_keypoints(b, img->keypoints[0]);
    put_keypoints(b, img->keypoints[1]);
    return g_byte_array_free_to_bytes(b);
}

/*
 * Reads a keypoint list; FALSE when the record is too short or claims more
 * keypoints than `max`, which the caller sets to the node's pixel count: the
 * detector finds at most one keypoint per pixel, so a larger count is a
 * malformed record rather than a node this engine wrote.
 */
static gboolean
get_keypoints(const guint8 *data, gsize len, gsize *pos, GArray *list, guint32 max)
{
    guint32 n;

    if (*pos + 4 > len)
        return FALSE;
    n = cb2000_get_u32(data + *pos);
    *pos += 4;
    if (n > max || n > (len - *pos) / KEYPOINT_RECORD_SIZE)
        return FALSE;
    for (guint32 i = 0; i < n; i++) {
        const guint8 *p = data + *pos;
        Cb2000EngineKeypoint kp = { 0 };

        kp.x10 = (gint16) cb2000_get_u16(p + 2);
        kp.y10 = (gint16) cb2000_get_u16(p + 4);
        kp.scale = p[6];
        kp.polarity = (gint8) p[7];
        kp.orientation = (gint16) cb2000_get_u16(p + 8);
        memcpy(kp.descriptor, p + 10, CB2000_ENGINE_DESCRIPTOR_SIZE);
        g_array_append_val(list, kp);
        *pos += KEYPOINT_RECORD_SIZE;
    }
    return TRUE;
}

Cb2000EngineImage *
cb2000_engine_node_decode(GBytes *node)
{
    gsize len;
    const guint8 *data = g_bytes_get_data(node, &len);
    Cb2000EngineImage *img;
    gint width, height, words, n_words, n_blocks;
    gsize pos;
    guint32 n_min;

    if (len < 4)
        return NULL;

    /*
     * The record comes from the print file, so its header is untrusted:
     * every size is checked here, before the first allocation. Reading the
     * dimensions into locals keeps a rejected record from ever reaching an
     * allocation sized by them.
     */
    width = cb2000_get_u16(data);
    height = cb2000_get_u16(data + 2);
    if (width == 0 || height == 0 ||
        width > NODE_MAX_SIDE || height > NODE_MAX_SIDE)
        return NULL;

    words = (width + 31) >> 5;
    n_words = words * height;
    pos = 4;
    if ((gsize) n_words * 8 + 8 > len - pos)
        return NULL;

    img = g_new0(Cb2000EngineImage, 1);
    img->width = width;
    img->height = height;
    for (gint i = 0; i < 2; i++)
        img->keypoints[i] = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineKeypoint));
    img->minutiae = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineMinutia));

    /* No pixel buffer: the engine keeps only the planes. Fresh block records
     * on the cropped size; nothing of the extractor's field is carried. */
    img->blocks_w = (img->width + BLOCK - 1) / BLOCK;
    img->blocks_h = (img->height + BLOCK - 1) / BLOCK;
    n_blocks = img->blocks_w * img->blocks_h;
    img->blocks = g_new0(Cb2000EngineBlock, n_blocks);
    for (gint i = 0; i < n_blocks; i++)
        img->blocks[i].orientation = CB2000_ENGINE_UNCLASSIFIED;
    img->integral = g_new0(gint32, (gsize) img->width * img->height);

    img->plane_words = words;
    img->plane_ridge = g_new(guint32, n_words);
    img->plane_masked = g_new(guint32, n_words);
    for (gint i = 0; i < n_words; i++, pos += 4)
        img->plane_ridge[i] = cb2000_get_u32(data + pos);
    for (gint i = 0; i < n_words; i++, pos += 4)
        img->plane_masked[i] = cb2000_get_u32(data + pos);
    img->feature_score = (gint32) cb2000_get_u32(data + pos);
    pos += 4;

    n_min = cb2000_get_u32(data + pos);
    pos += 4;
    if (n_min > (guint32) (width * height) ||
        n_min > (len - pos) / MINUTIA_RECORD_SIZE)
        goto fail;
    for (guint32 i = 0; i < n_min; i++, pos += MINUTIA_RECORD_SIZE) {
        const guint8 *p = data + pos;
        Cb2000EngineMinutia m = { 0 };

        /* Only these fields are stored; the rest decode as 0. */
        m.x = (gint16) cb2000_get_u16(p);
        m.y = (gint16) cb2000_get_u16(p + 2);
        m.angle = (gint32) cb2000_get_u32(p + 4);
        m.pattern = (gint16) cb2000_get_u16(p + 8);
        g_array_append_val(img->minutiae, m);
    }
    if (!get_keypoints(data, len, &pos, img->keypoints[0], (guint32) (width * height)) ||
        !get_keypoints(data, len, &pos, img->keypoints[1], (guint32) (width * height)))
        goto fail;
    return img;

fail:
    cb2000_engine_image_free(img);
    return NULL;
}

void
cb2000_engine_unpack_pixels(Cb2000EngineImage *img)
{
    const gint w = img->width, h = img->height, words = img->plane_words;

    g_free(img->pixels);
    img->pixels = g_malloc((gsize) w * h);
    for (gint y = 0; y < h; y++) {
        for (gint x = 0; x < w; x++) {
            const guint32 bit = 0x80000000u >> (x % 32);
            const gint k = y * words + x / 32;
            guint8 v;

            if (img->plane_masked[k] & bit)
                v = CB2000_ENGINE_PIXEL_MASKED;
            else if (img->plane_ridge[k] & bit)
                v = CB2000_ENGINE_PIXEL_RIDGE;
            else
                v = 0x00;
            img->pixels[y * w + x] = v;
        }
    }
}

/* Extract call. */

gint
cb2000_engine_extract_node(const guint8 *frame, gint width, gint height, GBytes **node)
{
    Cb2000EngineImage *img = cb2000_engine_extract(frame, width, height);
    gint ret;

    *node = NULL;
    if (img == NULL)
        return CB2000_ENGINE_ERR_POOR;
    if (img->feature_score < EXTRACT_MIN_FEATURE_SCORE) {
        cb2000_engine_image_free(img);
        return CB2000_ENGINE_ERR_POOR;
    }
    ret = img->feature_score * 100 / (width * height);
    *node = cb2000_engine_node_encode(img);
    cb2000_engine_image_free(img);
    return ret;
}

/* Compare call. */

/* The engine's list quicksort, see the header. The comparator "a is smaller
 * than b" is never negative. */
static gint
cmp_node_size(const gsize *size, gint a, gint b)
{
    return size[a] < size[b] ? 1 : 0;
}

static void
swap_index(gint *v, gint i, gint j)
{
    const gint t = v[i];

    v[i] = v[j];
    v[j] = t;
}

void
cb2000_engine_sort_by_size(gint *v, const gsize *size, gint n, gint lo, gint hi)
{
    while (lo >= 0 && hi >= 0 && lo < n && hi < n && hi > lo) {
        gint i = lo, j = hi;

        for (;;) {
            do
                i++;
            while (i < n && cmp_node_size(size, v[i], v[lo]) < 0);
            while (cmp_node_size(size, v[j], v[lo]) > 0)
                j--;
            if (i < n && i < j) {
                swap_index(v, i, j);
                continue;
            }
            break;
        }
        swap_index(v, lo, j);
        cb2000_engine_sort_by_size(v, size, n, lo, j - 1);
        lo = j + 1;
    }
}

void
cb2000_engine_clear_match_flags(Cb2000EngineImage *img)
{
    for (gint l = 0; l < 2; l++)
        for (guint i = 0; i < img->keypoints[l]->len; i++)
            g_array_index(img->keypoints[l], Cb2000EngineKeypoint, i).match_flags &= KEYPOINT_KEPT_FLAGS;
}

gint
cb2000_engine_compare(GBytes *const *nodes, guint n_nodes, const guint8 *probe_frame,
                      gint width, gint height, gint *matched_node)
{
    g_autoptr(GBytes) probe_node = NULL;
    g_autofree gint *order = NULL;
    g_autofree gsize *size = NULL;
    Cb2000EngineImage *probe;
    gint rc;
    gint probe_score;

    *matched_node = -1;
    rc = cb2000_engine_extract_node(probe_frame, width, height, &probe_node);
    if (rc < 0)
        return rc;
    probe = cb2000_engine_node_decode(probe_node);
    if (probe == NULL)
        return CB2000_ENGINE_ERR_POOR;
    if (probe->feature_score < EXTRACT_MIN_FEATURE_SCORE) {
        cb2000_engine_image_free(probe);
        return CB2000_ENGINE_ERR_POOR;
    }
    probe_score = probe->feature_score;

    /* Largest node record first. */
    order = g_new(gint, MAX(n_nodes, 1));
    size = g_new(gsize, MAX(n_nodes, 1));
    for (guint i = 0; i < n_nodes; i++) {
        order[i] = (gint) i;
        size[i] = g_bytes_get_size(nodes[i]);
    }
    cb2000_engine_sort_by_size(order, size, (gint) n_nodes, 0, (gint) n_nodes - 1);

    /* Every node is compared, even after a match: only the first match
     * counts, later decisions change nothing. */
    for (guint k = 0; k < n_nodes; k++) {
        Cb2000EngineImage *node = cb2000_engine_node_decode(nodes[order[k]]);
        Cb2000EnginePetResult pet = { 0 };
        gint pairs;

        if (node == NULL)
            continue;
        pairs = cb2000_engine_pet_match(node, probe, &pet);
        cb2000_engine_clear_match_flags(node);
        cb2000_engine_clear_match_flags(probe);
        if (pairs >= COMPARE_MIN_PAIRS) {
            Cb2000EngineScore score;
            gboolean accepted = FALSE;

            if (probe->pixels == NULL)
                cb2000_engine_unpack_pixels(probe);
            cb2000_engine_node_score(node, probe, &pet, &score);
            cb2000_engine_decide(pairs, score.score, &accepted);
            if (accepted && *matched_node < 0)
                *matched_node = order[k];
        }
        cb2000_engine_image_free(node);
    }
    cb2000_engine_image_free(probe);

    if (*matched_node >= 0)
        return 0;
    return (width * height * NO_MATCH_MIN_COVERAGE) / 100 <= probe_score
           ? CB2000_ENGINE_ERR_NO_MATCH : CB2000_ENGINE_ERR_POOR;
}
