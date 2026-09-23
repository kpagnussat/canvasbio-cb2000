/*
 * cb2000-harness: replay a private corpus through the driver's engine
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Reads the corpus written by cb2000-collect (<corpus>/<finger>/<session>/
 * with manifest.tsv) and runs it through the functions the driver calls: the
 * capture plan, then the engine. Nothing here decides anything about a
 * frame; it only orchestrates the same calls canvasbio_cb2000.c makes.
 *
 * Protocol:
 *  - Sample: each touch is what the driver hands to the engine, the raw
 *    frames the capture plan kept, best first (no quality gate).
 *  - Enrollment: per finger per session, touches in order through
 *    cb2000_engine_enroll_touch until the template completes.
 *  - Verify: every touch against every template, as the driver's verify
 *    (the engine's identify over one template): match, retry or no match.
 *    Touches examined by an enrollment never probe its template.
 *  - Genuine = same finger. Cross-session is the figure that counts; the
 *    same-session touches left after enrollment overstate accuracy.
 *    Impostor = another finger, split by hand relation.
 *  - Identify: every touch of one session against the templates enrolled
 *    in another session (finger order), in capture order, with the engine's
 *    last-match start carried across, as fprintd does for a user with
 *    several prints.
 *
 * The coverage register is not in the corpus, so the capture plan is
 * replayed with the coverage reads unknown (covered, as the core treats an
 * incomplete reply). The recorded capture setting of every image is checked
 * against the replay, and mismatches are reported.
 *
 * Usage: cb2000-harness [--enroll-order capture|random:<seed>]
 *                       <corpus-dir> <out.tsv> <session>...
 * --enroll-order: the order enrollment looks at the touches in. capture (the
 * default) is what the enroll action sees; random:<seed> measures how much
 * the result depends on which touches built the template.
 * The TSV holds derived numbers only, but it describes private data: keep it
 * next to the corpus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cb2000_core.h"
#include "cb2000_engine.h"

/* One touch reads 1 to 4 images (docs/PROTOCOL.md "Capture settings"). */
#define MAX_IMAGES_PER_TOUCH 4

static const struct {
    const char *name;
    gint        hand;       /* 0 right, 1 left */
    gint        position;   /* 0 index .. 3 little */
} fingers[] = {
    { "right-index", 0, 0 }, { "right-middle", 0, 1 },
    { "right-ring",  0, 2 }, { "right-little", 0, 3 },
    { "left-index",  1, 0 }, { "left-middle",  1, 1 },
    { "left-ring",   1, 2 }, { "left-little",  1, 3 },
};
#define N_FINGERS G_N_ELEMENTS(fingers)

typedef struct {
    gint     attempt;
    guint    n_images;
    guint8   raw[MAX_IMAGES_PER_TOUCH][CB2000_IMG_SIZE];
    gchar    name[MAX_IMAGES_PER_TOUCH][64];
    gchar    kind[MAX_IMAGES_PER_TOUCH][16];
    gboolean replay_mismatch;
    gboolean enroll_seen;      /* looked at by its set's enrollment */
    /* The raw frames the plan kept, best first. */
    guint    n_sample;
    guint8   sample[CB2000_CAPTURE_MAX_FRAMES * CB2000_IMG_SIZE];
} Touch;

/* The touches of one finger in one session, and the template built from them. */
typedef struct {
    guint        finger;
    const char  *session;
    Touch       *touches;
    guint        n_touches;

    gboolean     enrolled;
    guint        enroll_examined;   /* touches the enrollment looked at */
    guint        enroll_retries;
    guint        enroll_moves;
    GBytes      *tmpl;
} Set;

typedef enum {
    CAT_GENUINE_CROSS,
    CAT_GENUINE_SAME,
    CAT_IMPOSTOR_ADJACENT,
    CAT_IMPOSTOR_SAME_HAND,
    CAT_IMPOSTOR_OTHER_HAND,
    N_CATEGORIES,
} Category;

static const char *category_names[N_CATEGORIES] = {
    "genuine-cross-session", "genuine-same-session",
    "impostor-adjacent", "impostor-same-hand", "impostor-other-hand",
};

typedef struct {
    gboolean          compared;
    Cb2000EngineMatch result;
    gint              reject;
} PairResult;

typedef struct {
    Set         *sets;
    guint        n_sets;
    /* Global touch index -> set and touch. */
    guint       *touch_set;
    guint       *touch_index;
    guint        n_all_touches;
    PairResult  *results;   /* n_all_touches x n_sets */
    gint         progress;
    gboolean     random_order;
    guint32      seed;
} Harness;

static gboolean
load_pgm(const char *path, guint8 *out)
{
    FILE *f = fopen(path, "rb");
    gint w, h, maxv;
    gboolean ok;

    if (!f)
        return FALSE;
    ok = fscanf(f, "P5 %d %d %d", &w, &h, &maxv) == 3 &&
         w == CB2000_IMG_WIDTH && h == CB2000_IMG_HEIGHT && maxv == 255 &&
         fgetc(f) != EOF &&
         fread(out, 1, CB2000_IMG_SIZE, f) == CB2000_IMG_SIZE;
    fclose(f);
    return ok;
}

/* Timestamp-prefixed names sort in capture order. */
static void
sort_images(Touch *t)
{
    for (guint i = 1; i < t->n_images; i++) {
        for (guint j = i; j > 0 && strcmp(t->name[j - 1], t->name[j]) > 0; j--) {
            guint8 raw[CB2000_IMG_SIZE];
            gchar name[64], kind[16];

            memcpy(raw, t->raw[j], sizeof(raw));
            memcpy(t->raw[j], t->raw[j - 1], sizeof(raw));
            memcpy(t->raw[j - 1], raw, sizeof(raw));
            memcpy(name, t->name[j], sizeof(name));
            memcpy(t->name[j], t->name[j - 1], sizeof(name));
            memcpy(t->name[j - 1], name, sizeof(name));
            memcpy(kind, t->kind[j], sizeof(kind));
            memcpy(t->kind[j], t->kind[j - 1], sizeof(kind));
            memcpy(t->kind[j - 1], kind, sizeof(kind));
        }
    }
}

static gboolean
load_set(const char *corpus, Set *set)
{
    g_autofree char *dir = g_build_filename(corpus, fingers[set->finger].name,
                                            set->session, NULL);
    g_autofree char *manifest_path = g_build_filename(dir, "manifest.tsv", NULL);
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    gint max_attempt = 0;

    if (!g_file_get_contents(manifest_path, &text, NULL, NULL))
        return FALSE;
    lines = g_strsplit(text, "\n", -1);

    for (guint pass = 0; pass < 2; pass++) {
        for (guint i = 1; lines[i]; i++) {
            g_auto(GStrv) col = g_strsplit(lines[i], "\t", -1);
            gint attempt;
            Touch *t;

            if (g_strv_length(col) < 4)
                continue;
            attempt = atoi(col[0]);
            if (attempt < 1)
                continue;
            if (pass == 0) {
                max_attempt = MAX(max_attempt, attempt);
                continue;
            }

            t = &set->touches[attempt - 1];
            t->attempt = attempt;
            if (t->n_images == MAX_IMAGES_PER_TOUCH) {
                g_printerr("%s: attempt %d has more than %d images\n",
                           manifest_path, attempt, MAX_IMAGES_PER_TOUCH);
                return FALSE;
            }
            {
                g_autofree char *path = g_build_filename(dir, col[1], NULL);

                if (!load_pgm(path, t->raw[t->n_images])) {
                    g_printerr("Cannot read %s\n", path);
                    return FALSE;
                }
            }
            g_strlcpy(t->name[t->n_images], col[1], sizeof(t->name[0]));
            g_strlcpy(t->kind[t->n_images], col[2], sizeof(t->kind[0]));
            t->n_images++;
        }
        if (pass == 0) {
            if (max_attempt == 0)
                return FALSE;
            set->touches = g_new0(Touch, max_attempt);
            set->n_touches = max_attempt;
        }
    }

    for (guint i = 0; i < set->n_touches; i++)
        sort_images(&set->touches[i]);
    return TRUE;
}

/* Capture plan and frame order for every touch of a set. The collector
 * opens the device once per set, and every open starts in group 0. */
static guint
prepare_touches(Set *set)
{
    guint group = 0;
    guint mismatches = 0;

    for (guint ti = 0; ti < set->n_touches; ti++) {
        Touch *t = &set->touches[ti];
        Cb2000CapturePlan plan;
        Cb2000CaptureStep step = CB2000_CAPTURE_AGAIN;
        const guint8 *kept[CB2000_CAPTURE_MAX_FRAMES] = { NULL };
        const guint8 *fallback = NULL;
        guint order[CB2000_CAPTURE_MAX_FRAMES];
        gboolean use_fallback;
        guint i;

        t->n_sample = 0;
        t->replay_mismatch = FALSE;
        if (t->n_images == 0)
            continue;

        cb2000_core_capture_begin(&plan, group);
        for (i = 0; i < t->n_images && step == CB2000_CAPTURE_AGAIN; i++) {
            gchar expected[16];
            Cb2000FrameUse use;

            g_snprintf(expected, sizeof(expected), "raw-g%ui%u", plan.group, plan.setting);
            if (strcmp(expected, t->kind[i]) != 0)
                t->replay_mismatch = TRUE;

            step = cb2000_core_capture_step(&plan,
                                            cb2000_core_frame_brightness(t->raw[i]),
                                            -1, -1, -1, &use);
            if (use == CB2000_FRAME_KEEP)
                kept[plan.n_frames - 1] = t->raw[i];
            else if (use == CB2000_FRAME_FALLBACK)
                fallback = t->raw[i];
        }
        if (step == CB2000_CAPTURE_AGAIN || i < t->n_images)
            t->replay_mismatch = TRUE;
        if (t->replay_mismatch)
            mismatches++;
        group = plan.group;

        t->n_sample = cb2000_core_capture_order(&plan, order, &use_fallback);
        for (guint f = 0; f < t->n_sample; f++)
            memcpy(t->sample + f * CB2000_IMG_SIZE,
                   use_fallback ? fallback : kept[order[f]], CB2000_IMG_SIZE);
    }
    return mismatches;
}

/* Fills @order with the touch indices in the order enrollment looks at them. */
static void
enroll_order(const Set *set, gboolean random_order, guint32 seed, guint *order)
{
    guint n = set->n_touches;
    GRand *rand;

    for (guint i = 0; i < n; i++)
        order[i] = i;
    if (!random_order)
        return;

    /* Each set draws its own shuffle from the seed. */
    rand = g_rand_new_with_seed(seed * 1000003u + set->finger * 7919u +
                                (guint32) set->session[0]);
    for (guint i = n; i > 1; i--) {
        guint j = (guint) g_rand_int_range(rand, 0, (gint32) i);
        guint tmp = order[i - 1];

        order[i - 1] = order[j];
        order[j] = tmp;
    }
    g_rand_free(rand);
}

/* The engine's enrollment, touch by touch, until the template completes. */
static void
enroll(Set *set, const guint *order)
{
    Cb2000EngineEnrollment e;
    guint k;

    cb2000_engine_enrollment_init(&e);
    for (k = 0; k < set->n_touches && !set->enrolled; k++) {
        Touch *t = &set->touches[order[k]];
        gint reject;

        t->enroll_seen = TRUE;
        switch (cb2000_engine_enroll_touch(&e, t->sample, t->n_sample,
                                           CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT, &reject)) {
        case CB2000_ENGINE_ENROLL_COMPLETE:
            set->enrolled = TRUE;
            set->tmpl = g_bytes_new(e.tmpl->data, e.tmpl->len);
            break;
        case CB2000_ENGINE_ENROLL_RETRY:
            set->enroll_retries++;
            break;
        case CB2000_ENGINE_ENROLL_MOVE_FINGER:
            set->enroll_moves++;
            break;
        case CB2000_ENGINE_ENROLL_MORE:
            break;
        }
    }
    set->enroll_examined = k;
    cb2000_engine_enrollment_clear(&e);
}

/* The driver's verify: the engine's identify over one template. */
static void
verify(const Touch *t, const Set *tmpl, PairResult *r)
{
    g_autoptr(GPtrArray) gallery = g_ptr_array_new();
    guint last = 0;
    gint matched;

    g_ptr_array_add(gallery, tmpl->tmpl);
    r->result = cb2000_engine_identify(gallery, t->sample, t->n_sample,
                                       CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                       &last, &matched, &r->reject);
    r->compared = TRUE;
}

static Category
categorize(const Set *probe, const Set *tmpl)
{
    if (probe->finger == tmpl->finger)
        return probe == tmpl ? CAT_GENUINE_SAME : CAT_GENUINE_CROSS;
    if (fingers[probe->finger].hand != fingers[tmpl->finger].hand)
        return CAT_IMPOSTOR_OTHER_HAND;
    if (ABS(fingers[probe->finger].position - fingers[tmpl->finger].position) == 1)
        return CAT_IMPOSTOR_ADJACENT;
    return CAT_IMPOSTOR_SAME_HAND;
}

/* Genuine comparisons use only the other session's template, or for the
 * same session only the touches its enrollment never looked at. */
static gboolean
pair_wanted(const Harness *h, guint set_idx, guint touch_idx, guint tmpl_idx)
{
    const Set *probe = &h->sets[set_idx];
    const Set *tmpl = &h->sets[tmpl_idx];

    if (!tmpl->enrolled || probe->touches[touch_idx].n_images == 0)
        return FALSE;
    if (probe == tmpl && probe->touches[touch_idx].enroll_seen)
        return FALSE;
    return TRUE;
}

static void
probe_worker(gpointer data, gpointer user_data)
{
    Harness *h = user_data;
    guint g = GPOINTER_TO_UINT(data) - 1;
    guint si = h->touch_set[g];
    guint ti = h->touch_index[g];
    gint done;

    for (guint k = 0; k < h->n_sets; k++) {
        if (pair_wanted(h, si, ti, k))
            verify(&h->sets[si].touches[ti], &h->sets[k], &h->results[g * h->n_sets + k]);
    }
    done = g_atomic_int_add(&h->progress, 1);
    if (done % 100 == 0)
        g_printerr("  %d/%u touches\n", done, h->n_all_touches);
}

typedef struct {
    guint compared;
    guint retry;
    guint match;
} Tally;

static void
print_rate(const char *label, const Tally *t)
{
    guint decided = t->compared - t->retry;

    g_print("  %-26s %5u/%-6u %6.2f%%   retries (not decided) %u\n",
            label, t->match, decided, decided ? 100.0 * t->match / decided : 0.0,
            t->retry);
}

static void
tally_add(Tally *t, const PairResult *r)
{
    t->compared++;
    if (r->result == CB2000_ENGINE_MATCH_RETRY)
        t->retry++;
    if (r->result == CB2000_ENGINE_MATCH_FOUND)
        t->match++;
}

/* Every touch of session @probe_s, in capture order, against the templates
 * enrolled in session @tmpl_s (in finger order), with the engine's
 * last-match start carried across. */
static void
identify_run(const Harness *h, const char *probe_s, const char *tmpl_s)
{
    g_autoptr(GPtrArray) gallery = g_ptr_array_new();
    g_autoptr(GArray) owner = g_array_new(FALSE, FALSE, sizeof(guint));
    guint right = 0, wrong = 0, none = 0, retry = 0, probes = 0;
    guint last = 0;

    for (guint k = 0; k < h->n_sets; k++) {
        if (strcmp(h->sets[k].session, tmpl_s) != 0 || !h->sets[k].enrolled)
            continue;
        g_ptr_array_add(gallery, h->sets[k].tmpl);
        g_array_append_val(owner, h->sets[k].finger);
    }
    if (gallery->len == 0)
        return;

    for (guint s = 0; s < h->n_sets; s++) {
        const Set *set = &h->sets[s];

        if (strcmp(set->session, probe_s) != 0)
            continue;
        for (guint i = 0; i < set->n_touches; i++) {
            const Touch *t = &set->touches[i];
            gint matched, reject;

            if (t->n_images == 0)
                continue;
            probes++;
            switch (cb2000_engine_identify(gallery, t->sample, t->n_sample,
                                           CB2000_IMG_WIDTH, CB2000_IMG_HEIGHT,
                                           &last, &matched, &reject)) {
            case CB2000_ENGINE_MATCH_FOUND:
                if (g_array_index(owner, guint, matched) == set->finger)
                    right++;
                else
                    wrong++;
                break;
            case CB2000_ENGINE_MATCH_RETRY:
                retry++;
                break;
            case CB2000_ENGINE_MATCH_NONE:
                none++;
                break;
            }
        }
    }
    g_print("  probes %-2s x templates %-2s (%u templates): %u touches, right %u (%.1f%% of decided), "
            "wrong %u, no match %u, retry %u\n",
            probe_s, tmpl_s, gallery->len, probes, right,
            probes - retry ? 100.0 * right / (probes - retry) : 0.0, wrong, none, retry);
}

int
main(int argc, char **argv)
{
    Harness h = { 0 };
    Tally cat[N_CATEGORIES] = { { 0 } };
    Tally genuine_finger[N_FINGERS] = { { 0 } };
    GThreadPool *pool;
    FILE *out;
    guint mismatches = 0;
    guint g;
    gint argi = 1;

    while (argi + 1 < argc && strcmp(argv[argi], "--enroll-order") == 0) {
        const char *mode = argv[argi + 1];

        if (strcmp(mode, "capture") == 0) {
            h.random_order = FALSE;
        } else if (g_str_has_prefix(mode, "random:")) {
            h.random_order = TRUE;
            h.seed = (guint32) strtoul(mode + 7, NULL, 10);
        } else {
            break;
        }
        argi += 2;
    }
    if (argc - argi < 3 || g_str_has_prefix(argv[argi], "--")) {
        g_printerr("Usage: %s [--enroll-order capture|random:<seed>]\n"
                   "       <corpus-dir> <out.tsv> <session>...\n",
                   argv[0]);
        return 2;
    }

    h.sets = g_new0(Set, N_FINGERS * (argc - argi - 2));
    for (gint s = argi + 2; s < argc; s++) {
        for (guint f = 0; f < N_FINGERS; f++) {
            Set *set = &h.sets[h.n_sets];

            set->finger = f;
            set->session = argv[s];
            if (!load_set(argv[argi], set))
                continue;
            h.n_sets++;
        }
    }
    if (h.n_sets == 0) {
        g_printerr("No sets found under %s\n", argv[argi]);
        return 1;
    }

    g_print("%u sets\n", h.n_sets);
    g_print("enrollment (%d samples, %s order):\n", CB2000_ENGINE_ENROLL_SAMPLES,
            h.random_order ? "random" : "capture");
    if (h.random_order)
        g_print("  seed %u\n", h.seed);
    for (guint s = 0; s < h.n_sets; s++) {
        Set *set = &h.sets[s];
        guint *order = g_new(guint, set->n_touches);

        mismatches += prepare_touches(set);
        enroll_order(set, h.random_order, h.seed, order);
        enroll(set, order);
        g_free(order);
        h.n_all_touches += set->n_touches;
        g_print("  %-13s %-2s touches %3u | %s after %u touches, retries %u, move-finger %u, template %zu bytes\n",
                fingers[set->finger].name, set->session, set->n_touches,
                set->enrolled ? "enrolled" : "NOT ENROLLED",
                set->enroll_examined, set->enroll_retries, set->enroll_moves,
                set->tmpl ? g_bytes_get_size(set->tmpl) : 0);
    }
    g_print("capture plan replay mismatches: %u of %u touches\n", mismatches, h.n_all_touches);

    h.touch_set = g_new(guint, h.n_all_touches);
    h.touch_index = g_new(guint, h.n_all_touches);
    h.results = g_new0(PairResult, (gsize) h.n_all_touches * h.n_sets);
    g = 0;
    for (guint s = 0; s < h.n_sets; s++) {
        for (guint i = 0; i < h.sets[s].n_touches; i++) {
            h.touch_set[g] = s;
            h.touch_index[g] = i;
            g++;
        }
    }

    pool = g_thread_pool_new(probe_worker, &h, (gint) g_get_num_processors(), TRUE, NULL);
    for (g = 0; g < h.n_all_touches; g++)
        g_thread_pool_push(pool, GUINT_TO_POINTER(g + 1), NULL);
    g_thread_pool_free(pool, FALSE, TRUE);

    out = fopen(argv[argi + 1], "w");
    if (!out) {
        g_printerr("Cannot write %s\n", argv[argi + 1]);
        return 1;
    }
    fprintf(out, "probe_finger\tprobe_session\tattempt\tsample_frames\t"
                 "template_finger\ttemplate_session\tcategory\tdecision\treject\n");
    for (g = 0; g < h.n_all_touches; g++) {
        const Set *probe = &h.sets[h.touch_set[g]];
        const Touch *t = &probe->touches[h.touch_index[g]];

        for (guint k = 0; k < h.n_sets; k++) {
            const PairResult *r = &h.results[g * h.n_sets + k];
            const Set *tmpl = &h.sets[k];
            Category c;

            if (!r->compared)
                continue;
            c = categorize(probe, tmpl);
            tally_add(&cat[c], r);
            if (c == CAT_GENUINE_CROSS)
                tally_add(&genuine_finger[probe->finger], r);
            fprintf(out, "%s\t%s\t%d\t%u\t%s\t%s\t%s\t%s\t%d\n",
                    fingers[probe->finger].name, probe->session, t->attempt, t->n_sample,
                    fingers[tmpl->finger].name, tmpl->session, category_names[c],
                    r->result == CB2000_ENGINE_MATCH_FOUND ? "match" :
                    r->result == CB2000_ENGINE_MATCH_RETRY ? "retry" : "no-match",
                    r->reject);
        }
    }
    fclose(out);

    g_print("accept rate per verify touch (accepted/decided):\n");
    for (guint c = 0; c < N_CATEGORIES; c++)
        print_rate(category_names[c], &cat[c]);
    g_print("genuine cross-session by finger:\n");
    for (guint f = 0; f < N_FINGERS; f++)
        print_rate(fingers[f].name, &genuine_finger[f]);
    g_print("identify over another session's templates (fprintd with several prints):\n");
    for (gint a = argi + 2; a < argc; a++) {
        for (gint b = argi + 2; b < argc; b++) {
            if (a != b)
                identify_run(&h, argv[a], argv[b]);
        }
    }

    for (guint s = 0; s < h.n_sets; s++) {
        g_clear_pointer(&h.sets[s].tmpl, g_bytes_unref);
        g_free(h.sets[s].touches);
    }
    g_free(h.sets);
    g_free(h.touch_set);
    g_free(h.touch_index);
    g_free(h.results);
    return 0;
}
