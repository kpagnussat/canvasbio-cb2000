/*
 * CanvasBio CB2000: the engine's enrollment and template buffer
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The Windows engine enrolls one touch at a time into a template buffer
 * that holds a small directory and the node records. A touch is paired and
 * scored against the stored nodes, largest record first. The first node it
 * matches closely enough absorbs it: the touch is rotated onto the node and
 * the two are fused into one larger node, which may then absorb one more
 * stored node that it matches very closely. A touch that fuses nowhere is
 * stored as a new node. The buffer also keeps the observed counters (loads,
 * matches, decodes per node), preserving the documented buffer layout and
 * values.
 */

#include "cb2000_engine.h"
#include "cb2000_engine_bytes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PET_MODE            0
#define FINGER_ID           0

/* A touch this small (percent of the frame) is not enrolled. */
#define ENROLL_MIN_COVERAGE     40
/* Pairs a node needs before it is scored against the touch. */
#define ENROLL_MIN_PAIRS        9
/* Merge bars: below MERGE_BAR, or below MERGE_BAR_MANY with at least
 * MERGE_MANY_PAIRS pairs. */
#define MERGE_BAR               1000
#define MERGE_BAR_MANY          1600
#define MERGE_MANY_PAIRS        11
/* A merge that brings fewer new foreground pixels reports "little new". */
#define MERGE_LITTLE_NEW        640
/* Nodes a finger may hold before new touches are refused. */
#define MAX_NODES               15
/*
 * Nodes a stored template may claim when it is read back. The count comes
 * from the print file, which is untrusted, and every node is decoded,
 * paired and scored against each touch, so an inflated count costs CPU per
 * touch inside fprintd. The bound sits above MAX_NODES rather than on it,
 * so a template written by another build of this engine still loads.
 */
#define MAX_NODES_STORED        25
/* The fused node absorbs a stored node from these values on. */
#define ASSEMBLE_MIN_PAIRS      30
#define ASSEMBLE_BAR            1100

/* Rotation border of the union: the scorer's, or a wider one when the touch
 * brings fewer than UNION_FEW_NEW new pixels. */
#define UNION_BORDER            CB2000_ENGINE_BORDER
#define UNION_BORDER_FEW_NEW    5
#define UNION_FEW_NEW           200
/* Minutiae closer than these squared distances (same scan pattern, or not)
 * and closer than MINUTIA_MAX_TURN degrees are duplicates. */
#define MINUTIA_DUP_SAME        49
#define MINUTIA_DUP_OTHER       99
#define MINUTIA_MAX_TURN        60
/* Keypoints of the same polarity closer than 2 px (squared, tenths) are
 * duplicates. */
#define KEYPOINT_DUP_D2         400
/* Probe keypoints this close to the probe's edge (tenths) lose against a
 * stored duplicate. */
#define KEYPOINT_EDGE_MARGIN    130
/* Scratch mark for keypoints that a probe keypoint replaces. */
#define KP_REPLACED             0x40

#define HEADER_SIZE         0x7c
#define FINGER_SIZE         0x32
#define NODE_INFO_SIZE      0x1c
#define TAG_SIZE            0x24
#define ID_SIZE             0x20

static const char TAG[] = "CR";
static const char VERSION[] = "8.8.2.0U";

/* Template buffer. */

typedef struct {
    guint32  index;
    guint32  decodes;       /* successful decodes, all opens */
    guint32  matches;       /* commits that credited this node */
    guint32  pet_mode;
    GBytes  *body;
} Node;

typedef struct {
    gchar      id[ID_SIZE];
    guint32    loads;
    guint32    matches;
    gint16     last_finger;
    gboolean   has_finger;
    gint32     last_node;       /* the finger's last credited node, -1 none */
    GPtrArray *nodes;           /* of Node, index order */
    GPtrArray *owned;           /* every Node, removed ones included */

    /* State of one open, as the engine's template object keeps it. */
    Node      **queue;
    guint       queue_len;
    gint        cursor;         /* queue position, -1 parked */
    Node       *in_use;
    gboolean    matched;
    gboolean    merged;
    gboolean    assembled;
    gboolean    dirty;
    Node       *matched_node;
    Node       *merging_node;
    Node       *assembled_node;
    Cb2000EngineImage *merging_data;
} Template;

static void
node_free(gpointer data)
{
    Node *n = data;

    g_bytes_unref(n->body);
    g_free(n);
}

static void
put_zero(GByteArray *b, gsize n)
{
    static const guint8 zero[TAG_SIZE] = { 0 };

    while (n > 0) {
        const gsize k = MIN(n, sizeof(zero));

        g_byte_array_append(b, zero, k);
        n -= k;
    }
}

static void
put_text(GByteArray *b, const char *s, gsize field)
{
    const gsize n = strlen(s);

    g_byte_array_append(b, (const guint8 *) s, n);
    put_zero(b, field - n);
}

/* A random version 4 UUID as 32 lowercase hex digits. */
static void
draw_id(gchar *id)
{
    g_autofree gchar *uuid = g_uuid_string_random();
    gint k = 0;

    for (const gchar *p = uuid; *p != '\0' && k < ID_SIZE; p++)
        if (*p != '-')
            id[k++] = g_ascii_tolower(*p);
}

static Template *
template_new(void)
{
    Template *t = g_new0(Template, 1);

    draw_id(t->id);
    t->last_finger = -1;
    t->last_node = -1;
    t->nodes = g_ptr_array_new();
    t->owned = g_ptr_array_new_with_free_func(node_free);
    t->cursor = -1;
    return t;
}

static void
template_free(Template *t)
{
    cb2000_engine_image_free(t->merging_data);
    g_free(t->queue);
    g_ptr_array_unref(t->nodes);
    g_ptr_array_unref(t->owned);
    g_free(t);
}

static Node *
node_new(Template *t, GBytes *body)
{
    Node *n = g_new0(Node, 1);

    n->index = t->nodes->len;
    n->pet_mode = PET_MODE;
    n->body = body;
    g_ptr_array_add(t->owned, n);
    g_ptr_array_add(t->nodes, n);
    return n;
}

static guint32
directory_end(const Template *t)
{
    return HEADER_SIZE + (t->has_finger ? FINGER_SIZE + NODE_INFO_SIZE * t->nodes->len : 0);
}

static guint32
body_size(const Template *t)
{
    guint32 size = 0;

    for (guint i = 0; i < t->nodes->len; i++)
        size += g_bytes_get_size(((Node *) g_ptr_array_index(t->nodes, i))->body);
    return size;
}

/* Writes header, directory and bodies: the engine rewrites the first two
 * and moves the bodies at the same points, so the whole buffer follows. */
static void
template_write(const Template *t, GByteArray *buf)
{
    const guint32 dir_end = directory_end(t);
    guint32 offset = dir_end;

    g_byte_array_set_size(buf, 0);
    put_text(buf, TAG, TAG_SIZE);
    put_text(buf, VERSION, TAG_SIZE);
    g_byte_array_append(buf, (const guint8 *) t->id, ID_SIZE);
    cb2000_put_u32(buf, dir_end);
    cb2000_put_u32(buf, t->loads);
    cb2000_put_u32(buf, t->matches);
    cb2000_put_u16(buf, (guint16) t->last_finger);
    cb2000_put_u32(buf, body_size(t));
    cb2000_put_u16(buf, t->has_finger ? 1 : 0);
    if (!t->has_finger)
        return;

    cb2000_put_u32(buf, FINGER_ID);
    cb2000_put_u32(buf, t->nodes->len > 0 ? dir_end : G_MAXUINT32);
    cb2000_put_u32(buf, body_size(t));
    put_zero(buf, ID_SIZE);
    cb2000_put_u32(buf, (guint32) t->last_node);
    cb2000_put_u16(buf, (guint16) t->nodes->len);
    for (guint i = 0; i < t->nodes->len; i++) {
        const Node *n = g_ptr_array_index(t->nodes, i);
        const guint32 size = g_bytes_get_size(n->body);

        cb2000_put_u32(buf, n->index);
        cb2000_put_u32(buf, offset);
        cb2000_put_u32(buf, size);
        cb2000_put_u32(buf, n->decodes);
        cb2000_put_u32(buf, n->matches);
        cb2000_put_u32(buf, n->pet_mode);
        cb2000_put_u32(buf, 0);
        offset += size;
    }
    for (guint i = 0; i < t->nodes->len; i++) {
        gsize size;
        const guint8 *body = g_bytes_get_data(((Node *) g_ptr_array_index(t->nodes, i))->body, &size);

        g_byte_array_append(buf, body, size);
    }
}

/* Accepts "8.<m>.<p>.<q>" with m > 3. */
static gboolean
version_ok(const guint8 *field)
{
    gchar text[TAG_SIZE + 1];
    gint major, minor, patch, build;

    memcpy(text, field, TAG_SIZE);
    text[TAG_SIZE] = '\0';
    if (sscanf(text, "%d.%d.%d.%d", &major, &minor, &patch, &build) != 4)
        return FALSE;
    return major == 8 && minor > 3 && patch >= 0 && build >= 0;
}

/* Parses the buffer. Returns 0, TEMPLATE_EMPTY for an empty buffer, or a
 * format error. */
static gint
template_parse(Template *t, const guint8 *data, gsize len)
{
    guint32 dir_end;
    gint16 n_fingers;
    gsize pos;

    if (len == 0 || data[0] == 0)
        return CB2000_ENGINE_ERR_TEMPLATE_EMPTY;
    if (len < HEADER_SIZE || memcmp(data, TAG, sizeof(TAG)) != 0)
        return CB2000_ENGINE_ERR_TEMPLATE_TAG;
    if (!version_ok(data + TAG_SIZE))
        return CB2000_ENGINE_ERR_TEMPLATE_VERSION;
    memcpy(t->id, data + 0x48, ID_SIZE);
    dir_end = cb2000_get_u32(data + 0x68);
    t->loads = cb2000_get_u32(data + 0x6c);
    t->matches = cb2000_get_u32(data + 0x70);
    t->last_finger = (gint16) cb2000_get_u16(data + 0x74);
    n_fingers = (gint16) cb2000_get_u16(data + 0x7a);
    if (n_fingers == 0)
        return 0;
    /* One finger on this sensor. */
    if (n_fingers != 1 || len < HEADER_SIZE + FINGER_SIZE ||
        cb2000_get_u32(data + HEADER_SIZE) != FINGER_ID)
        return CB2000_ENGINE_ERR_TEMPLATE_BAD;

    pos = HEADER_SIZE;
    t->has_finger = TRUE;
    t->last_node = (gint32) cb2000_get_u32(data + pos + 0x2c);
    {
        const gint n_nodes = (gint16) cb2000_get_u16(data + pos + 0x30);

        pos += FINGER_SIZE;
        if (n_nodes < 0 || n_nodes > MAX_NODES_STORED ||
            pos + (gsize) n_nodes * NODE_INFO_SIZE > len ||
            pos + (gsize) n_nodes * NODE_INFO_SIZE != dir_end)
            return CB2000_ENGINE_ERR_TEMPLATE_BAD;
        for (gint i = 0; i < n_nodes; i++, pos += NODE_INFO_SIZE) {
            const guint32 offset = cb2000_get_u32(data + pos + 0x04);
            const guint32 size = cb2000_get_u32(data + pos + 0x08);
            Node *n;

            if (offset > len || size > len - offset)
                return CB2000_ENGINE_ERR_TEMPLATE_BAD;
            n = node_new(t, g_bytes_new(data + offset, size));
            n->index = cb2000_get_u32(data + pos);
            n->decodes = cb2000_get_u32(data + pos + 0x0c);
            n->matches = cb2000_get_u32(data + pos + 0x10);
            n->pet_mode = cb2000_get_u32(data + pos + 0x14);
        }
    }
    return 0;
}

/* Opens and loads the buffer: every load of a stored template counts. */
static gint
template_load(GByteArray *buf, Template **out)
{
    Template *t = template_new();
    gint rc = template_parse(t, buf->data, buf->len);

    if (rc == 0) {
        t->loads++;
        template_write(t, buf);
    } else if (rc != CB2000_ENGINE_ERR_TEMPLATE_EMPTY) {
        template_free(t);
        t = NULL;
    }
    *out = t;
    return rc;
}

static void
template_add_node(Template *t, const Cb2000EngineImage *img, GByteArray *buf)
{
    t->has_finger = TRUE;
    node_new(t, cb2000_engine_node_encode(img));
    template_write(t, buf);
}

static void
template_remove_node(Template *t, Node *n)
{
    if (!g_ptr_array_remove(t->nodes, n))
        return;
    for (guint i = 0; i < t->nodes->len; i++)
        ((Node *) g_ptr_array_index(t->nodes, i))->index = i;
}

/* Stores what this open changed: the absorbed node goes, the fused node
 * gets its new record, and the matched node is credited. */
static void
template_commit(Template *t, GByteArray *buf)
{
    if (!t->dirty)
        return;
    if (t->assembled)
        template_remove_node(t, t->assembled_node);
    if (t->merged) {
        g_bytes_unref(t->merging_node->body);
        t->merging_node->body = cb2000_engine_node_encode(t->merging_data);
    }
    if (t->matched) {
        Node *k = t->merging_node != NULL ? t->merging_node : t->matched_node;

        t->last_node = (gint32) k->index;
        t->last_finger = FINGER_ID;
        t->matches++;
        k->matches++;
        if (t->matched_node != NULL && t->matched_node != k)
            t->matched_node->matches++;
    }
    t->assembled = FALSE;
    t->dirty = FALSE;
    template_write(t, buf);
}

/* Node iteration. */

static void
template_build_queue(Template *t)
{
    const guint n = t->nodes->len;
    g_autofree gint *order = g_new(gint, MAX(n, 1));
    g_autofree gsize *size = g_new(gsize, MAX(n, 1));

    for (guint i = 0; i < n; i++) {
        order[i] = (gint) i;
        size[i] = g_bytes_get_size(((Node *) g_ptr_array_index(t->nodes, i))->body);
    }
    cb2000_engine_sort_by_size(order, size, (gint) n, 0, (gint) n - 1);
    g_free(t->queue);
    t->queue = g_new(Node *, MAX(n, 1));
    for (guint i = 0; i < n; i++)
        t->queue[i] = g_ptr_array_index(t->nodes, order[i]);
    t->queue_len = n;
    t->cursor = 0;
}

/* The next queued node, decoded, or NULL. While a fusion is pending the
 * fused node is skipped. */
static Cb2000EngineImage *
template_next(Template *t)
{
    Node *item;
    Cb2000EngineImage *img;

    t->in_use = NULL;
    if (t->cursor < 0)
        return NULL;
    while ((guint) t->cursor < t->queue_len && t->merged &&
           t->queue[t->cursor] == t->merging_node)
        t->cursor++;
    if ((guint) t->cursor >= t->queue_len)
        return NULL;
    item = t->queue[t->cursor++];
    img = cb2000_engine_node_decode(item->body);
    if (img == NULL)
        return NULL;
    item->decodes++;
    t->in_use = item;
    return img;
}

static void
template_matched(Template *t)
{
    if (t->matched)
        return;
    t->matched = TRUE;
    t->matched_node = t->in_use;
    t->dirty = TRUE;
}

/* Keeps u as the fused data of the node in use; the first fusion of an open
 * wins and ends the iteration. */
static void
template_merge(Template *t, Cb2000EngineImage *u)
{
    if (t->merged) {
        cb2000_engine_image_free(u);
        return;
    }
    if (!t->matched) {
        t->matched = TRUE;
        t->matched_node = t->in_use;
    }
    t->merged = TRUE;
    t->merging_node = t->in_use;
    t->merging_data = u;
    t->cursor = -1;
    t->dirty = TRUE;
}

/* The fused data absorbed the node in use: that node is removed at the
 * commit. Ends the iteration. */
static void
template_assemble(Template *t, Cb2000EngineImage *u)
{
    if (t->assembled) {
        cb2000_engine_image_free(u);
        return;
    }
    t->assembled = TRUE;
    t->assembled_node = t->in_use;
    cb2000_engine_image_free(t->merging_data);
    t->merging_data = u;
    t->cursor = -1;
    t->dirty = TRUE;
}

/* Union of two images. */

static inline gboolean
bit_get(const guint32 *plane, gint words, gint x, gint y)
{
    return (plane[y * words + (x >> 5)] >> (31 - (x & 31))) & 1;
}

static inline void
bit_set(guint32 *plane, gint words, gint x, gint y, gboolean v)
{
    const guint32 mask = 0x80000000u >> (x & 31);

    if (v)
        plane[y * words + (x >> 5)] |= mask;
    else
        plane[y * words + (x >> 5)] &= ~mask;
}

static Cb2000EngineImage *
image_empty(void)
{
    Cb2000EngineImage *img = g_new0(Cb2000EngineImage, 1);

    for (gint i = 0; i < 2; i++)
        img->keypoints[i] = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineKeypoint));
    img->minutiae = g_array_new(FALSE, FALSE, sizeof(Cb2000EngineMinutia));
    return img;
}

static inline gboolean
near_probe_edge(const Cb2000EngineKeypoint *kp, const Cb2000EngineImage *p)
{
    return kp->x10 < KEYPOINT_EDGE_MARGIN || kp->x10 > 10 * p->width - KEYPOINT_EDGE_MARGIN ||
           kp->y10 < KEYPOINT_EDGE_MARGIN || kp->y10 > 10 * p->height - KEYPOINT_EDGE_MARGIN;
}

/* Moves p's minutiae and keypoints into v, the image of p rotated by a.
 * Compatibility detail: angles in degrees get the angle in radians added. */
static void
rotate_features(const Cb2000EngineImage *p, Cb2000EngineImage *v, gdouble a)
{
    const gdouble c = cos(a), s = sin(a);
    gint xmin, ymin;

    cb2000_engine_rotated_box(p->width, p->height, c, s, &xmin, &ymin);
    for (guint i = 0; i < p->minutiae->len; i++) {
        Cb2000EngineMinutia m = g_array_index(p->minutiae, Cb2000EngineMinutia, i);
        const gdouble x = m.x, y = m.y;

        /* Truncation of v + 0.5, not a rounding for negative v. */
        m.x = (gint16) ((gint) ((x * c - y * s) + 0.5) - xmin);
        m.y = (gint16) ((gint) ((c * y + s * x) + 0.5) - ymin);
        m.angle = (gint) (m.angle + a) % 360;
        g_array_append_val(v->minutiae, m);
    }
    for (gint l = 0; l < 2; l++) {
        for (guint i = 0; i < p->keypoints[l]->len; i++) {
            const Cb2000EngineKeypoint *k = &g_array_index(p->keypoints[l], Cb2000EngineKeypoint, i);
            const gdouble x = k->x10, y = k->y10;
            Cb2000EngineKeypoint r = *k;

            r.x10 = (gint16) ((gint) ((x * c - y * s) + 0.5) - 10 * xmin);
            r.y10 = (gint16) ((gint) ((c * y + s * x) + 0.5) - 10 * ymin);
            r.orientation = (gint16) ((gint) (k->orientation + a) % 180);
            r.match_flags = near_probe_edge(k, p) ? CB2000_ENGINE_KP_EDGE : 0;
            g_array_append_val(v->keypoints[l], r);
        }
    }
}

/* Copies every bit of both planes of t into u at (tx, ty), padding bits
 * included, then the valid bits of v at (vx, vy): v wins where it is
 * valid. Bits that fall outside u's words or rows are dropped. */
static void
union_planes(Cb2000EngineImage *u, const Cb2000EngineImage *t, const Cb2000EngineImage *v,
             gint tx, gint ty, gint vx, gint vy)
{
    const gint n_words = u->plane_words * u->height;
    const gint u_bits = 32 * u->plane_words;

    u->plane_ridge = g_new0(guint32, n_words);
    u->plane_masked = g_new(guint32, n_words);
    memset(u->plane_masked, 0xff, sizeof(guint32) * n_words);

    for (gint y = 0; y < t->height && y + ty < u->height; y++) {
        for (gint x = 0; x < 32 * t->plane_words && x + tx < u_bits; x++) {
            bit_set(u->plane_masked, u->plane_words, x + tx, y + ty,
                    bit_get(t->plane_masked, t->plane_words, x, y));
            bit_set(u->plane_ridge, u->plane_words, x + tx, y + ty,
                    bit_get(t->plane_ridge, t->plane_words, x, y));
        }
    }
    for (gint y = 0; y < v->height && y + vy < u->height; y++) {
        for (gint x = 0; x < 32 * v->plane_words && x + vx < u_bits; x++) {
            if (bit_get(v->plane_masked, v->plane_words, x, y))
                continue;
            bit_set(u->plane_masked, u->plane_words, x + vx, y + vy, FALSE);
            bit_set(u->plane_ridge, u->plane_words, x + vx, y + vy,
                    bit_get(v->plane_ridge, v->plane_words, x, y));
        }
    }
}

/* t's minutiae outside v's valid area (position only, a compatibility
 * detail), then v's minutiae, each one replacing the similar minutiae already
 * there. */
static void
union_minutiae(Cb2000EngineImage *u, const Cb2000EngineImage *t, Cb2000EngineImage *v,
               gint tx, gint ty, gint vx, gint vy)
{
    for (guint i = 0; i < t->minutiae->len; i++) {
        const Cb2000EngineMinutia *m = &g_array_index(t->minutiae, Cb2000EngineMinutia, i);
        Cb2000EngineMinutia q = { 0 };
        gint rx, ry;

        q.x = (gint16) (m->x + tx);
        q.y = (gint16) (m->y + ty);
        rx = q.x - vx;
        ry = q.y - vy;
        if (rx <= 0 || rx >= v->width || ry <= 0 || ry >= v->height ||
            bit_get(v->plane_masked, v->plane_words, rx, ry))
            g_array_append_val(u->minutiae, q);
    }
    for (guint i = 0; i < v->minutiae->len; i++) {
        Cb2000EngineMinutia *m = &g_array_index(v->minutiae, Cb2000EngineMinutia, i);

        m->x = (gint16) (m->x + vx);
        m->y = (gint16) (m->y + vy);
        for (guint k = 0; k < u->minutiae->len;) {
            const Cb2000EngineMinutia *e = &g_array_index(u->minutiae, Cb2000EngineMinutia, k);
            const gint d2 = (m->x - e->x) * (m->x - e->x) + (m->y - e->y) * (m->y - e->y);
            const gboolean near = e->pattern == m->pattern ? d2 <= MINUTIA_DUP_SAME
                                                            : d2 <= MINUTIA_DUP_OTHER;

            if (near) {
                gint turn = ABS(m->angle - e->angle);

                if (360 - turn <= turn)
                    turn = 360 - turn;
                if (turn < MINUTIA_MAX_TURN) {
                    g_array_remove_index(u->minutiae, k);
                    continue;
                }
            }
            k++;
        }
        g_array_append_val(u->minutiae, *m);
    }
}

/* 2-d tree over positions of a keypoint list, built like the pairing's. */
typedef struct {
    gint    idx;            /* into the list */
    gint    left, right, parent;
    guint8  axis;
    guint8  visited;
} KpNode;

typedef struct {
    GArray *list;           /* of Cb2000EngineKeypoint */
    KpNode *nodes;
    gint    n;
    gint    root;
    /* Search state. */
    const Cb2000EngineKeypoint *q;
    gint    best;
    gint    limit;
} KpTree;

static inline Cb2000EngineKeypoint *
kp_at(const KpTree *tr, gint node)
{
    return &g_array_index(tr->list, Cb2000EngineKeypoint, tr->nodes[node].idx);
}

static inline gint
kp_diff(const KpTree *tr, gint node, const Cb2000EngineKeypoint *q, guint8 axis)
{
    const Cb2000EngineKeypoint *k = kp_at(tr, node);

    return axis == 0 ? k->x10 - q->x10 : k->y10 - q->y10;
}

/* The node below which q would be inserted (equal keys go right). */
static gint
kp_descend(const KpTree *tr, const Cb2000EngineKeypoint *q, gint *diff_out)
{
    gint node = tr->root, diff = 0;
    guint8 axis = 0;

    for (;;) {
        gint child;

        diff = kp_diff(tr, node, q, axis);
        child = diff > 0 ? tr->nodes[node].left : tr->nodes[node].right;
        if (child < 0)
            break;
        node = child;
        axis = 1 - axis;
    }
    *diff_out = diff;
    return node;
}

static void
kp_insert(KpTree *tr, gint idx)
{
    const gint id = tr->n++;
    KpNode *k = &tr->nodes[id];

    k->idx = idx;
    k->left = k->right = -1;
    if (tr->root < 0) {
        tr->root = id;
        k->axis = 0;
        k->parent = -1;
    } else {
        gint diff;
        const gint node = kp_descend(tr, kp_at(tr, id), &diff);

        if (diff > 0)
            tr->nodes[node].left = id;
        else
            tr->nodes[node].right = id;
        k->parent = node;
        k->axis = 1 - tr->nodes[node].axis;
    }
}

/* Marks every same-polarity node within the radius as replaced (unless q is
 * an edge keypoint); best = the last one visited. */
static void
kp_visit(KpTree *tr, gint node)
{
    const Cb2000EngineKeypoint *q = tr->q;

    while (node >= 0 && !tr->nodes[node].visited) {
        Cb2000EngineKeypoint *k = kp_at(tr, node);
        /* 64 bits because the coordinates reach here from the print file:
         * a forged pair would overflow a 32-bit square. The distances of a
         * node this engine wrote are orders of magnitude below that. */
        const gint64 dx = k->x10 - q->x10, dy = k->y10 - q->y10;
        const gint64 d2 = dx * dx + dy * dy;
        gint diff;
        gint64 sq;

        tr->nodes[node].visited = 1;
        if (k->polarity == q->polarity && d2 < tr->limit) {
            tr->best = node;
            if (!(q->match_flags & CB2000_ENGINE_KP_EDGE))
                k->match_flags |= KP_REPLACED;
        }
        diff = kp_diff(tr, node, q, tr->nodes[node].axis);
        sq = (gint64) diff * diff;
        if (diff <= 0) {
            kp_visit(tr, tr->nodes[node].right);
            if (tr->limit <= sq)
                return;
            node = tr->nodes[node].left;
        } else {
            kp_visit(tr, tr->nodes[node].left);
            if (tr->limit < sq)
                return;
            node = tr->nodes[node].right;
        }
    }
}

static gint
kp_radius_search(KpTree *tr, const Cb2000EngineKeypoint *q)
{
    gint diff;

    if (tr->root < 0)
        return -1;
    tr->q = q;
    tr->best = -1;
    tr->limit = KEYPOINT_DUP_D2;
    for (gint i = 0; i < tr->n; i++)
        tr->nodes[i].visited = 0;
    for (gint node = kp_descend(tr, q, &diff); node >= 0; node = tr->nodes[node].parent)
        kp_visit(tr, node);
    return tr->best;
}

/* t's keypoints, then v's, each probe keypoint replacing the stored ones
 * within 2 px; a probe keypoint near the probe's edge is dropped instead. */
static void
union_keypoint_list(GArray *out, const GArray *tl, const GArray *vl, const Cb2000EngineImage *v,
                    gdouble angle, gint tx10, gint ty10, gint vx10, gint vy10)
{
    KpTree tr = { 0 };

    for (guint i = 0; i < tl->len; i++) {
        Cb2000EngineKeypoint n = g_array_index(tl, Cb2000EngineKeypoint, i);

        n.x10 = (gint16) (n.x10 + tx10);
        n.y10 = (gint16) (n.y10 + ty10);
        g_array_append_val(out, n);
    }

    tr.list = out;
    tr.nodes = g_new0(KpNode, MAX(out->len, 1));
    tr.root = -1;
    for (guint i = 0; i < out->len; i++) {
        const Cb2000EngineKeypoint *n = &g_array_index(out, Cb2000EngineKeypoint, i);

        if (vx10 <= n->x10 && n->x10 <= vx10 + 10 * v->width &&
            vy10 <= n->y10 && n->y10 <= vy10 + 10 * v->height)
            kp_insert(&tr, (gint) i);
    }

    for (guint i = 0; i < vl->len; i++) {
        const Cb2000EngineKeypoint *k = &g_array_index(vl, Cb2000EngineKeypoint, i);
        Cb2000EngineKeypoint q = *k;
        gint near;

        q.x10 = (gint16) (k->x10 + vx10);
        q.y10 = (gint16) (k->y10 + vy10);
        /* Compatibility detail: the rotation is added a second time. */
        q.orientation = (gint16) ((gint) (k->orientation + angle) % 180);
        near = kp_radius_search(&tr, &q);
        if (near < 0) {
            g_array_append_val(out, q);
        } else if (!(k->match_flags & CB2000_ENGINE_KP_EDGE)) {
            kp_at(&tr, near)->match_flags |= KP_REPLACED;
            g_array_append_val(out, q);
        }
    }
    g_free(tr.nodes);

    for (guint i = 0; i < out->len;) {
        if (g_array_index(out, Cb2000EngineKeypoint, i).match_flags & KP_REPLACED)
            g_array_remove_index(out, i);
        else
            i++;
    }
}

/* Rounds a translation in tenths to whole pixels, half away from zero. */
static gint
whole_pixels(gdouble t10)
{
    return (gint) ((t10 + (0.0 < t10 ? 5.0 : -5.0)) / 10.0);
}

/*
 * Fuses probe p into t with the score's transform: p is rotated like the
 * scorer's best angle and laid at its best offset, and the result covers
 * both. Neither input is changed except for the scratch marks.
 */
static Cb2000EngineImage *
union_image(const Cb2000EngineImage *t, Cb2000EngineImage *p, const Cb2000EngineScore *sc)
{
    const gint border = sc->probe_feature_score - sc->valid >= UNION_FEW_NEW
                        ? UNION_BORDER : UNION_BORDER_FEW_NEW;
    Cb2000EngineImage *v = cb2000_engine_rotate(p, sc->angle, border);
    Cb2000EngineImage *u = image_empty();
    const gint bx = whole_pixels(sc->tx), by = whole_pixels(sc->ty);
    const gint tx = MAX(-bx, 0), ty = MAX(-by, 0);
    const gint vx = MAX(bx, 0), vy = MAX(by, 0);

    rotate_features(p, v, sc->angle);
    u->width = (guint16) MAX(t->width + tx, v->width + vx);
    u->height = (guint16) MAX(t->height + ty, v->height + vy);
    u->plane_words = (u->width + 31) >> 5;
    union_planes(u, t, v, tx, ty, vx, vy);
    union_minutiae(u, t, v, tx, ty, vx, vy);
    for (gint l = 0; l < 2; l++)
        union_keypoint_list(u->keypoints[l], t->keypoints[l], v->keypoints[l], v,
                            sc->angle, 10 * tx, 10 * ty, 10 * vx, 10 * vy);
    u->feature_score = t->feature_score + p->feature_score - sc->valid;
    cb2000_engine_image_free(v);
    return u;
}

/* Enrollment. */

/* The engine's enrollment decision (its dispatcher in mode 1; mode 3 is the
 * assembly further down): fuse the probe into node n when the score is below
 * the bar. Returns TRUE when it did. */
static gboolean
merge_decision(Template *t, const Cb2000EngineImage *n, Cb2000EngineImage *p,
               gint pairs, const Cb2000EngineScore *sc)
{
    const gint bar = pairs >= MERGE_MANY_PAIRS ? MERGE_BAR_MANY : MERGE_BAR;

    if (sc->score >= bar)
        return FALSE;
    template_merge(t, union_image(n, p, sc));
    return TRUE;
}

/* After a fusion, the fused node absorbs the first other node (queue
 * order) that it matches very closely. At most n nodes are tried. */
static void
consolidate(Template *t, GByteArray *buf, gint n)
{
    Cb2000EngineImage *m = t->merging_data;
    gboolean assembled = FALSE;
    Cb2000EngineImage *node;

    if (m == NULL || t->nodes->len <= 1 || n <= 0) {
        t->cursor = -1;
        return;
    }
    t->cursor = 0;
    while ((node = template_next(t)) != NULL) {
        Cb2000EnginePetResult pet = { 0 };
        gint pairs;

        if (n == 0) {
            cb2000_engine_image_free(node);
            break;
        }
        pairs = cb2000_engine_pet_match(m, node, &pet);
        cb2000_engine_clear_match_flags(m);
        cb2000_engine_clear_match_flags(node);
        if (pairs >= ASSEMBLE_MIN_PAIRS) {
            Cb2000EngineScore sc;

            if (node->pixels == NULL)
                cb2000_engine_unpack_pixels(node);
            cb2000_engine_node_score(m, node, &pet, &sc);
            if (sc.score < ASSEMBLE_BAR) {
                assembled = TRUE;
                /* Replaces (and frees) m: the loop ends here. */
                template_assemble(t, union_image(m, node, &sc));
            } else {
                assembled = FALSE;
            }
        }
        cb2000_engine_image_free(node);
        n--;
    }
    if (assembled)
        template_commit(t, buf);
    t->cursor = -1;
}

gint
cb2000_engine_enroll_add(GByteArray *tmpl, const guint8 *frame, gint width, gint height)
{
    g_autoptr(GBytes) record = NULL;
    Template *t = NULL;
    Cb2000EngineImage *probe = NULL;
    gboolean finger_known, merged = FALSE;
    gint rc, code = CB2000_ENGINE_ENROLL_NEW_NODE;

    rc = cb2000_engine_extract_node(frame, width, height, &record);
    if (rc < 0)
        return rc;

    rc = template_load(tmpl, &t);
    if (rc == 0)
        finger_known = t->has_finger;
    else if (rc == CB2000_ENGINE_ERR_TEMPLATE_EMPTY)
        finger_known = FALSE;
    else
        return rc;

    probe = cb2000_engine_node_decode(record);
    if (probe == NULL || (width * height * ENROLL_MIN_COVERAGE) / 100 > probe->feature_score) {
        rc = CB2000_ENGINE_ERR_POOR;
        goto out;
    }
    rc = 0;
    if (!finger_known)
        goto add;

    template_build_queue(t);
    for (;;) {
        Cb2000EngineImage *node = template_next(t);
        Cb2000EnginePetResult pet = { 0 };
        gint pairs;

        if (node == NULL)
            break;
        pairs = cb2000_engine_pet_match(node, probe, &pet);
        code = CB2000_ENGINE_ENROLL_NEW_NODE;
        cb2000_engine_clear_match_flags(node);
        cb2000_engine_clear_match_flags(probe);
        if (pairs >= ENROLL_MIN_PAIRS) {
            Cb2000EngineScore sc;

            if (probe->pixels == NULL)
                cb2000_engine_unpack_pixels(probe);
            cb2000_engine_node_score(node, probe, &pet, &sc);
            if (merge_decision(t, node, probe, pairs, &sc)) {
                consolidate(t, tmpl, (gint) t->queue_len - 1);
                merged = TRUE;
                code = ABS(sc.valid - sc.probe_feature_score) < MERGE_LITTLE_NEW
                       ? CB2000_ENGINE_ENROLL_MERGED_LITTLE : CB2000_ENGINE_ENROLL_MERGED;
            } else {
                code = CB2000_ENGINE_ENROLL_PAIRED;
                template_matched(t);
            }
        }
        cb2000_engine_image_free(node);
    }
    if (merged) {
        template_commit(t, tmpl);
        goto out;
    }
    if (t->nodes->len >= MAX_NODES) {
        template_commit(t, tmpl);
        rc = CB2000_ENGINE_ERR_TEMPLATE_FULL;
        goto out;
    }

add:
    template_add_node(t, probe, tmpl);
    template_commit(t, tmpl);

out:
    cb2000_engine_image_free(probe);
    template_free(t);
    return rc != 0 ? rc : code;
}

gint
cb2000_engine_template_commit(GByteArray *tmpl)
{
    Template *t = NULL;
    const gint rc = template_load(tmpl, &t);

    if (t != NULL)
        template_free(t);
    return rc;
}

GPtrArray *
cb2000_engine_template_nodes(const guint8 *data, gsize len)
{
    Template *t = template_new();
    GPtrArray *nodes = NULL;

    if (template_parse(t, data, len) == 0) {
        nodes = g_ptr_array_new_with_free_func((GDestroyNotify) g_bytes_unref);
        for (guint i = 0; i < t->nodes->len; i++)
            g_ptr_array_add(nodes, g_bytes_ref(((Node *) g_ptr_array_index(t->nodes, i))->body));
    }
    template_free(t);
    return nodes;
}
