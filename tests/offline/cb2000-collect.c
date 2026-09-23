/*
 * cb2000-collect: capture a private matcher corpus with the CB2000 driver
 *
 * Copyright (C) 2026 Kristofer Pagnussat
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Captures one finger for one session through libfprint. The driver itself
 * writes every raw image it reads (1 to 4 per touch) into
 * <corpus>/<finger>/<session>/ via CB2000_DEBUG_IMAGE_DIR; this tool drives
 * the captures and records which file belongs to which touch in manifest.tsv.
 * [touches] counts accepted touches; rejected ones are recorded too.
 *
 * The frames are fingerprint images. Keep <corpus> on private, preferably
 * encrypted storage outside any repository.
 *
 * Usage (fprintd must be stopped):
 *   sudo cb2000-collect <corpus-dir> <finger> <session> [touches]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fprint.h>

#define DEFAULT_TOUCHES 30
/* Pause between touches for the user to lift and shift the finger. */
#define LIFT_DELAY_US   (1200 * 1000)
/* Gives up on a finger that keeps being rejected instead of looping forever. */
#define MAX_ATTEMPTS_PER_TOUCH 3

static const char *fingers[] = {
    "left-index", "left-middle", "left-ring", "left-little",
    "right-index", "right-middle", "right-ring", "right-little",
    NULL,
};

static gboolean
valid_finger(const char *name)
{
    for (int i = 0; fingers[i]; i++)
        if (g_strcmp0(fingers[i], name) == 0)
            return TRUE;
    return FALSE;
}

/* Returns the frames that appeared in dir since the previous call. */
static GPtrArray *
new_frames(const char *dir, GHashTable *seen)
{
    GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
    const char *name;

    while (d && (name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".pgm") || g_hash_table_contains(seen, name))
            continue;
        g_hash_table_add(seen, g_strdup(name));
        g_ptr_array_add(found, g_strdup(name));
    }
    /* Timestamp-prefixed names sort chronologically. */
    g_ptr_array_sort(found, (GCompareFunc) g_strcmp0);
    return found;
}

/* When run through sudo, hand the files back to the invoking user. */
static void
chown_to_invoker(const char *dir)
{
    const char *uid_s = g_getenv("SUDO_UID");
    const char *gid_s = g_getenv("SUDO_GID");
    g_autoptr(GDir) d = NULL;
    const char *name;
    uid_t uid;
    gid_t gid;

    if (!uid_s || !gid_s)
        return;
    uid = (uid_t) g_ascii_strtoull(uid_s, NULL, 10);
    gid = (gid_t) g_ascii_strtoull(gid_s, NULL, 10);

    if (chown(dir, uid, gid) != 0)
        g_printerr("chown %s: %s\n", dir, g_strerror(errno));
    d = g_dir_open(dir, 0, NULL);
    while (d && (name = g_dir_read_name(d))) {
        g_autofree char *path = g_build_filename(dir, name, NULL);
        if (chown(path, uid, gid) != 0)
            g_printerr("chown %s: %s\n", path, g_strerror(errno));
    }
}

int
main(int argc, char **argv)
{
    g_autoptr(FpContext) ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GHashTable) seen = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *manifest_path = NULL;
    FpDevice *dev = NULL;
    GPtrArray *devices;
    FILE *manifest;
    int touches = DEFAULT_TOUCHES;
    int recorded = 0;
    int attempt = 0;

    if (argc < 4 || argc > 5 || !valid_finger(argv[2])) {
        g_printerr("Usage: sudo %s <corpus-dir> <finger> <session> [touches]\n"
                   "  finger: left|right-index|middle|ring|little\n", argv[0]);
        return 2;
    }
    if (argc == 5)
        touches = atoi(argv[4]);
    if (touches < 1 || touches > 1000) {
        g_printerr("touches must be between 1 and 1000\n");
        return 2;
    }

    dir = g_build_filename(argv[1], argv[2], argv[3], NULL);
    if (g_file_test(dir, G_FILE_TEST_EXISTS)) {
        g_printerr("%s already exists; use a new session name instead of mixing data.\n", dir);
        return 1;
    }
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_printerr("Cannot create %s: %s\n", dir, g_strerror(errno));
        return 1;
    }

    /* The driver writes the raw frames here. */
    g_setenv("CB2000_DEBUG_IMAGE_DIR", dir, TRUE);

    ctx = fp_context_new();
    devices = fp_context_get_devices(ctx);
    for (guint i = 0; devices && i < devices->len; i++) {
        FpDevice *candidate = g_ptr_array_index(devices, i);
        if (g_strcmp0(fp_device_get_driver(candidate), "canvasbio-cb2000") == 0)
            dev = candidate;
    }
    if (!dev) {
        g_printerr("No CanvasBio CB2000 found (is the driver installed and fprintd stopped?)\n");
        return 1;
    }
    if (!fp_device_open_sync(dev, NULL, &error)) {
        g_printerr("Cannot open device: %s\n", error->message);
        return 1;
    }

    manifest_path = g_build_filename(dir, "manifest.tsv", NULL);
    manifest = fopen(manifest_path, "w");
    if (!manifest) {
        g_printerr("Cannot write %s: %s\n", manifest_path, g_strerror(errno));
        return 1;
    }
    fprintf(manifest, "attempt\tfile\tkind\toutcome\n");
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_print("Finger %s, session %s: %d touches. Lift and shift the finger slightly between touches.\n",
            argv[2], argv[3], touches);

    while (recorded < touches && attempt < touches * MAX_ATTEMPTS_PER_TOUCH) {
        g_autoptr(FpImage) img = NULL;
        g_autoptr(GError) capture_error = NULL;
        g_autoptr(GPtrArray) frames = NULL;
        g_autofree char *outcome = NULL;
        gboolean touched = FALSE;

        attempt++;
        g_print("[%d/%d] touch the sensor...\n", recorded + 1, touches);
        img = fp_device_capture_sync(dev, TRUE, NULL, &capture_error);

        if (!capture_error)
            outcome = g_strdup("ok");
        else if (capture_error->domain == FP_DEVICE_RETRY)
            outcome = g_strdup_printf("retry:%s", capture_error->message);
        else
            outcome = g_strdup_printf("error:%s", capture_error->message);

        /* One touch reads 1 to 4 images, one file each, named
         * <time>_<action>_raw-g<group>i<setting>.pgm. */
        frames = new_frames(dir, seen);
        for (guint i = 0; i < frames->len; i++) {
            const char *name = g_ptr_array_index(frames, i);
            const char *kind = strrchr(name, '_');
            g_autofree char *kind_s = g_strndup(kind ? kind + 1 : name,
                                                strlen(kind ? kind + 1 : name) - strlen(".pgm"));

            fprintf(manifest, "%d\t%s\t%s\t%s\n", attempt, name, kind_s, outcome);
            if (g_str_has_prefix(kind_s, "raw"))
                touched = TRUE;
        }
        /* Only accepted touches fill the session. A rejected touch keeps its
         * frames and manifest rows (the harness replays the capture plan on
         * them) but is not one of the usable touches the protocol asks for. */
        if (touched && !capture_error)
            recorded++;
        fflush(manifest);

        if (capture_error && capture_error->domain != FP_DEVICE_RETRY &&
            frames->len == 0) {
            g_printerr("Capture failed without a frame: %s\n", capture_error->message);
            break;
        }

        g_print("    %s - lift your finger\n", outcome);
        g_usleep(LIFT_DELAY_US);
    }

    fclose(manifest);
    fp_device_close_sync(dev, NULL, NULL);
    chown_to_invoker(dir);

    g_print("Recorded %d touches in %s\n", recorded, dir);
    return recorded == touches ? 0 : 1;
}
