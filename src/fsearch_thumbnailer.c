/*
   FSearch - A fast file search utility
   Copyright © 2026 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define G_LOG_DOMAIN "fsearch-thumbnailer"

#include "fsearch_thumbnailer.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

// Maximum number of thumbnails which are created in parallel
#define THUMBNAILER_MAX_THREADS 4
// External thumbnailers get killed if they take longer than that
#define THUMBNAILER_TIMEOUT_SECONDS 10
// Images larger than that aren't thumbnailed in-process
#define THUMBNAILER_MAX_IN_PROCESS_FILE_SIZE (64 * 1024 * 1024)
#define THUMBNAILER_FAIL_DIR_NAME "fsearch"
#define THUMBNAILER_SOFTWARE_NAME "FSearch"

struct _FsearchThumbnailer {
    GThreadPool *pool;
    // Requests are handled in LIFO order: rows which were scrolled into view last are most likely still visible
    gint64 request_counter;

    char *cache_dir;

    GMutex lock;
    // content type (char *) -> exec line (char *) of the thumbnailer which handles it
    GHashTable *thumbnailers;
    // content types (char *) GdkPixbuf can load
    GHashTable *pixbuf_mime_types;
    // content type (char *) -> GINT_TO_POINTER(can thumbnail ? 1 : 2)
    GHashTable *can_thumbnail_cache;
};

typedef struct {
    char *path;
    int size;
    gint64 request_id;
} ThumbnailRequest;

static void
thumbnail_request_free(ThumbnailRequest *request) {
    g_clear_pointer(&request->path, g_free);
    g_free(request);
}

typedef struct {
    const char *name;
    int size;
} ThumbnailFlavor;

static const ThumbnailFlavor flavors[] = {
    {"normal", 128},
    {"large", 256},
    {"x-large", 512},
    {"xx-large", 1024},
};

static const ThumbnailFlavor *
get_flavor_for_size(int size) {
    for (guint i = 0; i < G_N_ELEMENTS(flavors); i++) {
        if (size <= flavors[i].size) {
            return &flavors[i];
        }
    }
    return &flavors[G_N_ELEMENTS(flavors) - 1];
}

// --- Thumbnailer registry ---

static void
load_thumbnailers_from_dir(GHashTable *thumbnailers, GHashTable *seen_files, const char *dir_path) {
    g_autoptr(GDir) dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        return;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(name, ".thumbnailer") || g_hash_table_contains(seen_files, name)) {
            // A thumbnailer with the same name in a directory with higher priority overrides this one
            continue;
        }
        g_hash_table_add(seen_files, g_strdup(name));

        g_autofree char *path = g_build_filename(dir_path, name, NULL);
        g_autoptr(GKeyFile) key_file = g_key_file_new();
        if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
            continue;
        }

        g_autofree char *try_exec = g_key_file_get_string(key_file, "Thumbnailer Entry", "TryExec", NULL);
        if (try_exec) {
            g_autofree char *program = g_find_program_in_path(try_exec);
            if (!program) {
                continue;
            }
        }

        g_autofree char *exec = g_key_file_get_string(key_file, "Thumbnailer Entry", "Exec", NULL);
        g_auto(GStrv) mime_types = g_key_file_get_string_list(key_file, "Thumbnailer Entry", "MimeType", NULL, NULL);
        if (!exec || !mime_types) {
            continue;
        }

        for (guint i = 0; mime_types[i]; i++) {
            if (mime_types[i][0] == '\0' || g_hash_table_contains(thumbnailers, mime_types[i])) {
                continue;
            }
            g_hash_table_insert(thumbnailers, g_strdup(mime_types[i]), g_strdup(exec));
        }
        g_debug("found thumbnailer: %s", path);
    }
}

static GHashTable *
load_thumbnailers(void) {
    GHashTable *thumbnailers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) seen_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_autofree char *user_dir = g_build_filename(g_get_user_data_dir(), "thumbnailers", NULL);
    load_thumbnailers_from_dir(thumbnailers, seen_files, user_dir);

    const char *const *system_dirs = g_get_system_data_dirs();
    for (guint i = 0; system_dirs[i]; i++) {
        g_autofree char *dir = g_build_filename(system_dirs[i], "thumbnailers", NULL);
        load_thumbnailers_from_dir(thumbnailers, seen_files, dir);
    }
    return thumbnailers;
}

static GHashTable *
load_pixbuf_mime_types(void) {
    GHashTable *mime_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GSList *formats = gdk_pixbuf_get_formats();
    for (GSList *f = formats; f; f = f->next) {
        GdkPixbufFormat *format = f->data;
        if (gdk_pixbuf_format_is_disabled(format)) {
            continue;
        }
        g_auto(GStrv) format_mime_types = gdk_pixbuf_format_get_mime_types(format);
        for (guint i = 0; format_mime_types && format_mime_types[i]; i++) {
            g_hash_table_add(mime_types, g_strdup(format_mime_types[i]));
        }
    }
    g_slist_free(formats);
    return mime_types;
}

static gboolean
content_type_in_table(GHashTable *table, const char *content_type) {
    if (g_hash_table_contains(table, content_type)) {
        return TRUE;
    }
    // Handle aliases and sub-classes (e.g. image/x-canon-cr2 is a sub-class of image/x-dcraw)
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, table);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (g_content_type_is_a(content_type, key)) {
            return TRUE;
        }
    }
    return FALSE;
}

// Must be called with self->lock held
static char *
get_thumbnailer_exec_locked(FsearchThumbnailer *self, const char *content_type) {
    const char *exec = g_hash_table_lookup(self->thumbnailers, content_type);
    if (exec) {
        return g_strdup(exec);
    }
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&iter, self->thumbnailers);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_content_type_is_a(content_type, key)) {
            return g_strdup(value);
        }
    }
    return NULL;
}

gboolean
fsearch_thumbnailer_can_thumbnail(FsearchThumbnailer *self, const char *content_type) {
    g_return_val_if_fail(self, FALSE);
    if (!content_type) {
        return FALSE;
    }

    g_mutex_lock(&self->lock);
    gint cached = GPOINTER_TO_INT(g_hash_table_lookup(self->can_thumbnail_cache, content_type));
    if (cached == 0) {
        const gboolean can_thumbnail = content_type_in_table(self->thumbnailers, content_type)
                                    || content_type_in_table(self->pixbuf_mime_types, content_type);
        cached = can_thumbnail ? 1 : 2;
        g_hash_table_insert(self->can_thumbnail_cache, g_strdup(content_type), GINT_TO_POINTER(cached));
    }
    g_mutex_unlock(&self->lock);

    return cached == 1;
}

// --- Thumbnail cache ---

static char *
get_thumbnail_basename(const char *uri) {
    g_autofree char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    return g_strconcat(md5, ".png", NULL);
}

// Returns TRUE if the thumbnail at `thumbnail_path` was created for the current version of the file
static gboolean
is_thumbnail_valid(GdkPixbuf *thumbnail, const char *uri, guint64 mtime) {
    const char *thumb_uri = gdk_pixbuf_get_option(thumbnail, "tEXt::Thumb::URI");
    const char *thumb_mtime = gdk_pixbuf_get_option(thumbnail, "tEXt::Thumb::MTime");
    if (!thumb_uri || !thumb_mtime || strcmp(thumb_uri, uri) != 0) {
        return FALSE;
    }
    guint64 value = 0;
    if (!g_ascii_string_to_unsigned(thumb_mtime, 10, 0, G_MAXUINT64, &value, NULL)) {
        return FALSE;
    }
    return value == mtime;
}

static GdkPixbuf *
load_valid_thumbnail(const char *thumbnail_path, const char *uri, guint64 mtime) {
    if (!g_file_test(thumbnail_path, G_FILE_TEST_IS_REGULAR)) {
        return NULL;
    }
    g_autoptr(GdkPixbuf) thumbnail = gdk_pixbuf_new_from_file(thumbnail_path, NULL);
    if (!thumbnail || !is_thumbnail_valid(thumbnail, uri, mtime)) {
        return NULL;
    }
    return g_steal_pointer(&thumbnail);
}

static gboolean
ensure_dir(const char *path) {
    if (g_mkdir_with_parents(path, 0700) != 0) {
        g_debug("failed to create directory %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static char *
create_tmp_file(const char *dir) {
    char *tmpl = g_build_filename(dir, "fsearch-XXXXXX.png", NULL);
    const int fd = g_mkstemp_full(tmpl, O_RDWR, 0600);
    if (fd < 0) {
        g_free(tmpl);
        return NULL;
    }
    close(fd);
    return tmpl;
}

// Saves `pixbuf` with the metadata required by the thumbnail spec, atomically replacing `dest_path`
static gboolean
save_thumbnail(GdkPixbuf *pixbuf, const char *dest_dir, const char *dest_path, const char *uri, guint64 mtime,
               goffset size, const char *content_type) {
    if (!ensure_dir(dest_dir)) {
        return FALSE;
    }
    g_autofree char *tmp_path = create_tmp_file(dest_dir);
    if (!tmp_path) {
        return FALSE;
    }

    g_autofree char *mtime_str = g_strdup_printf("%" G_GUINT64_FORMAT, mtime);
    g_autofree char *size_str = g_strdup_printf("%" G_GOFFSET_FORMAT, size);
    char *keys[] = {"tEXt::Thumb::URI",
                    "tEXt::Thumb::MTime",
                    "tEXt::Thumb::Size",
                    "tEXt::Software",
                    content_type ? "tEXt::Thumb::Mimetype" : NULL,
                    NULL};
    char *values[] = {(char *)uri, mtime_str, size_str, THUMBNAILER_SOFTWARE_NAME, (char *)content_type, NULL};

    g_autoptr(GError) error = NULL;
    if (!gdk_pixbuf_savev(pixbuf, tmp_path, "png", keys, values, &error)) {
        g_debug("failed to save thumbnail for %s: %s", uri, error->message);
        g_unlink(tmp_path);
        return FALSE;
    }
    if (g_rename(tmp_path, dest_path) != 0) {
        g_unlink(tmp_path);
        return FALSE;
    }
    return TRUE;
}

// --- Thumbnail creation ---

typedef struct {
    gboolean done;
    gboolean timed_out;
} SubprocessWaitData;

static void
on_subprocess_exited(GObject *source, GAsyncResult *res, gpointer user_data) {
    SubprocessWaitData *data = user_data;
    g_subprocess_wait_finish(G_SUBPROCESS(source), res, NULL);
    data->done = TRUE;
}

static gboolean
on_subprocess_timeout(gpointer user_data) {
    SubprocessWaitData *data = user_data;
    data->timed_out = TRUE;
    return G_SOURCE_REMOVE;
}

// Runs the thumbnailer and waits until it exits, but at most THUMBNAILER_TIMEOUT_SECONDS
static gboolean
run_thumbnailer(const char *const *argv) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) process = g_subprocess_newv(argv,
                                                       G_SUBPROCESS_FLAGS_STDIN_INHERIT
                                                           | G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                       &error);
    if (!process) {
        g_debug("failed to launch thumbnailer %s: %s", argv[0], error->message);
        return FALSE;
    }

    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    SubprocessWaitData data = {};
    g_subprocess_wait_async(process, NULL, on_subprocess_exited, &data);

    GSource *timeout_source = g_timeout_source_new_seconds(THUMBNAILER_TIMEOUT_SECONDS);
    g_source_set_callback(timeout_source, on_subprocess_timeout, &data, NULL);
    g_source_attach(timeout_source, context);

    while (!data.done && !data.timed_out) {
        g_main_context_iteration(context, TRUE);
    }
    if (!data.done) {
        g_debug("thumbnailer %s timed out", argv[0]);
        g_subprocess_force_exit(process);
        while (!data.done) {
            g_main_context_iteration(context, TRUE);
        }
    }

    g_source_destroy(timeout_source);
    g_source_unref(timeout_source);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    return !data.timed_out && g_subprocess_get_successful(process);
}

static char *
expand_exec_arg(const char *arg, const char *uri, const char *input_path, const char *output_path, int size) {
    GString *expanded = g_string_new(NULL);
    for (const char *c = arg; *c; c++) {
        if (*c != '%' || c[1] == '\0') {
            g_string_append_c(expanded, *c);
            continue;
        }
        c++;
        switch (*c) {
        case 'u':
            g_string_append(expanded, uri);
            break;
        case 'i':
            g_string_append(expanded, input_path);
            break;
        case 'o':
            g_string_append(expanded, output_path);
            break;
        case 's':
            g_string_append_printf(expanded, "%d", size);
            break;
        case '%':
            g_string_append_c(expanded, '%');
            break;
        default:
            break;
        }
    }
    return g_string_free(expanded, FALSE);
}

static GdkPixbuf *
create_thumbnail_with_thumbnailer(const char *exec,
                                  const char *path,
                                  const char *uri,
                                  const char *tmp_dir,
                                  const ThumbnailFlavor *flavor) {
    g_auto(GStrv) exec_argv = NULL;
    if (!g_shell_parse_argv(exec, NULL, &exec_argv, NULL)) {
        return NULL;
    }
    g_autofree char *output_path = create_tmp_file(tmp_dir);
    if (!output_path) {
        return NULL;
    }

    // Arguments are expanded individually (instead of expanding the Exec line and parsing it afterwards),
    // so file names with spaces or shell meta characters can't break or inject arguments
    const guint argc = g_strv_length(exec_argv);
    g_auto(GStrv) argv = g_new0(char *, argc + 1);
    for (guint i = 0; i < argc; i++) {
        argv[i] = expand_exec_arg(exec_argv[i], uri, path, output_path, flavor->size);
    }

    GdkPixbuf *thumbnail = NULL;
    if (run_thumbnailer((const char *const *)argv)) {
        thumbnail = gdk_pixbuf_new_from_file(output_path, NULL);
    }
    g_unlink(output_path);
    return thumbnail;
}

static GdkPixbuf *
create_thumbnail_in_process(const char *path, goffset file_size, const ThumbnailFlavor *flavor) {
    if (file_size > THUMBNAILER_MAX_IN_PROCESS_FILE_SIZE) {
        return NULL;
    }
    int width = 0;
    int height = 0;
    if (!gdk_pixbuf_get_file_info(path, &width, &height) || width <= 0 || height <= 0) {
        return NULL;
    }
    g_autoptr(GdkPixbuf) pixbuf = NULL;
    if (width <= flavor->size && height <= flavor->size) {
        // Thumbnails must never be larger than the original image
        pixbuf = gdk_pixbuf_new_from_file(path, NULL);
    }
    else {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(path, flavor->size, flavor->size, TRUE, NULL);
    }
    if (!pixbuf) {
        return NULL;
    }
    return gdk_pixbuf_apply_embedded_orientation(pixbuf);
}

static GdkPixbuf *
scale_to_fit(GdkPixbuf *pixbuf, int size) {
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    if (width <= size && height <= size) {
        return g_object_ref(pixbuf);
    }
    const double scale = MIN((double)size / width, (double)size / height);
    return gdk_pixbuf_scale_simple(pixbuf,
                                   MAX(1, (int)(width * scale + 0.5)),
                                   MAX(1, (int)(height * scale + 0.5)),
                                   GDK_INTERP_BILINEAR);
}

static GdkPixbuf *
lookup_or_create_thumbnail(FsearchThumbnailer *self, const char *path, int size) {
    if (g_str_has_prefix(path, self->cache_dir)) {
        // Never create thumbnails of thumbnails
        return NULL;
    }

    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file,
                                                  G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE
                                                                                 "," G_FILE_ATTRIBUTE_TIME_MODIFIED
                                                                                 "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                  G_FILE_QUERY_INFO_NONE,
                                                  NULL,
                                                  NULL);
    if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
        return NULL;
    }

    const char *content_type = g_file_info_get_content_type(info);
    if (!fsearch_thumbnailer_can_thumbnail(self, content_type)) {
        return NULL;
    }

    const guint64 mtime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    const goffset file_size = g_file_info_get_size(info);
    g_autofree char *uri = g_file_get_uri(file);
    g_autofree char *basename = get_thumbnail_basename(uri);

    const ThumbnailFlavor *flavor = get_flavor_for_size(size);
    g_autofree char *flavor_dir = g_build_filename(self->cache_dir, flavor->name, NULL);
    g_autofree char *thumbnail_path = g_build_filename(flavor_dir, basename, NULL);

    // 1. Thumbnail already exists (created by us or any other application, e.g. a file manager)
    g_autoptr(GdkPixbuf) thumbnail = load_valid_thumbnail(thumbnail_path, uri, mtime);
    if (thumbnail) {
        return scale_to_fit(thumbnail, size);
    }

    // 2. A previous attempt to create the thumbnail failed
    g_autofree char *fail_dir = g_build_filename(self->cache_dir, "fail", THUMBNAILER_FAIL_DIR_NAME, NULL);
    g_autofree char *fail_path = g_build_filename(fail_dir, basename, NULL);
    g_autoptr(GdkPixbuf) fail_marker = load_valid_thumbnail(fail_path, uri, mtime);
    if (fail_marker) {
        return NULL;
    }

    // 3. Create the thumbnail
    if (!ensure_dir(flavor_dir)) {
        return NULL;
    }
    g_mutex_lock(&self->lock);
    g_autofree char *exec = get_thumbnailer_exec_locked(self, content_type);
    g_mutex_unlock(&self->lock);

    if (exec) {
        thumbnail = create_thumbnail_with_thumbnailer(exec, path, uri, flavor_dir, flavor);
    }
    if (!thumbnail && g_content_type_is_a(content_type, "image/*")) {
        thumbnail = create_thumbnail_in_process(path, file_size, flavor);
    }

    if (!thumbnail) {
        g_debug("failed to create thumbnail for %s (%s)", path, content_type);
        g_autoptr(GdkPixbuf) marker = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
        if (marker) {
            gdk_pixbuf_fill(marker, 0);
            save_thumbnail(marker, fail_dir, fail_path, uri, mtime, file_size, content_type);
        }
        return NULL;
    }

    g_autoptr(GdkPixbuf) flavor_thumbnail = scale_to_fit(thumbnail, flavor->size);
    save_thumbnail(flavor_thumbnail, flavor_dir, thumbnail_path, uri, mtime, file_size, content_type);

    return scale_to_fit(flavor_thumbnail, size);
}

// --- Worker threads ---

static void
thumbnailer_worker(gpointer data, gpointer user_data) {
    g_autoptr(GTask) task = data;
    FsearchThumbnailer *self = user_data;

    if (g_task_return_error_if_cancelled(task)) {
        return;
    }

    ThumbnailRequest *request = g_task_get_task_data(task);
    GdkPixbuf *thumbnail = lookup_or_create_thumbnail(self, request->path, request->size);
    g_task_return_pointer(task, thumbnail, g_object_unref);
}

static gint
compare_requests(gconstpointer a, gconstpointer b, gpointer user_data) {
    ThumbnailRequest *request_a = g_task_get_task_data((GTask *)a);
    ThumbnailRequest *request_b = g_task_get_task_data((GTask *)b);
    // Newest request first
    if (request_a->request_id == request_b->request_id) {
        return 0;
    }
    return request_a->request_id > request_b->request_id ? -1 : 1;
}

static FsearchThumbnailer *
fsearch_thumbnailer_new(void) {
    FsearchThumbnailer *self = g_new0(FsearchThumbnailer, 1);
    g_mutex_init(&self->lock);
    self->cache_dir = g_build_filename(g_get_user_cache_dir(), "thumbnails", NULL);
    self->thumbnailers = load_thumbnailers();
    self->pixbuf_mime_types = load_pixbuf_mime_types();
    self->can_thumbnail_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->pool = g_thread_pool_new(thumbnailer_worker, self, THUMBNAILER_MAX_THREADS, FALSE, NULL);
    g_thread_pool_set_sort_function(self->pool, compare_requests, NULL);
    return self;
}

FsearchThumbnailer *
fsearch_thumbnailer_get_default(void) {
    static gsize initialized = 0;
    static FsearchThumbnailer *thumbnailer = NULL;
    if (g_once_init_enter(&initialized)) {
        thumbnailer = fsearch_thumbnailer_new();
        g_once_init_leave(&initialized, 1);
    }
    return thumbnailer;
}

void
fsearch_thumbnailer_load_async(FsearchThumbnailer *self,
                               const char *path,
                               int size,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
    g_return_if_fail(self);
    g_return_if_fail(path);

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, fsearch_thumbnailer_load_async);
    // Cancelled requests which are still queued must not show up later
    g_task_set_return_on_cancel(task, FALSE);

    ThumbnailRequest *request = g_new0(ThumbnailRequest, 1);
    request->path = g_strdup(path);
    request->size = CLAMP(size, 16, 1024);
    request->request_id = ++self->request_counter;
    g_task_set_task_data(task, request, (GDestroyNotify)thumbnail_request_free);

    // The thread pool takes ownership of the task
    g_thread_pool_push(self->pool, task, NULL);
}

GdkPixbuf *
fsearch_thumbnailer_load_finish(FsearchThumbnailer *self, GAsyncResult *result, GError **error) {
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}
