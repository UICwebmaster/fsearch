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

#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

G_BEGIN_DECLS

// Thumbnail support following the freedesktop.org thumbnail managing standard:
// https://specifications.freedesktop.org/thumbnail-spec/latest/
//
// - Thumbnails are shared with other applications through $XDG_CACHE_HOME/thumbnails
// - Missing thumbnails are created with the thumbnailers installed on the system
//   ($XDG_DATA_DIRS/thumbnailers/*.thumbnailer, e.g. gdk-pixbuf-thumbnailer, ffmpegthumbnailer, totem, evince)
//   which run in their own process. Images are thumbnailed in-process with GdkPixbuf as a fallback.
// - Failures are recorded in $XDG_CACHE_HOME/thumbnails/fail/fsearch, so they aren't retried
//   until the file gets modified.

typedef struct _FsearchThumbnailer FsearchThumbnailer;

FsearchThumbnailer *
fsearch_thumbnailer_get_default(void);

// Asynchronously looks up (and creates if necessary) the thumbnail for `path`.
// The result is a GdkPixbuf which fits into `size` x `size` device pixels, or NULL if no thumbnail can be provided.
// The callback is invoked in the thread-default main context of the caller.
void
fsearch_thumbnailer_load_async(FsearchThumbnailer *self,
                               const char *path,
                               int size,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

GdkPixbuf *
fsearch_thumbnailer_load_finish(FsearchThumbnailer *self, GAsyncResult *result, GError **error);

// Returns TRUE if thumbnails can potentially be created for files of this content type.
gboolean
fsearch_thumbnailer_can_thumbnail(FsearchThumbnailer *self, const char *content_type);

G_END_DECLS
