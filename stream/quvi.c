/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <quvi.h>

#include "talloc.h"
#include "mp_msg.h"
#include "options.h"
#include "stream.h"

struct mp_resolve_result *mp_resolve_quvi(const char *url, struct MPOpts *opts)
{
    quvi_t q = quvi_new();
    if (quvi_ok(q) == QUVI_FALSE)
        return NULL;

    // Don't try to use quvi on an URL that's not directly supported, since
    // quvi will do a network access anyway in order to check for HTTP
    // redirections etc.
    // The documentation says this will fail on "shortened" URLs.
    if (quvi_supports(q, (char *) url, QUVI_SUPPORTS_MODE_OFFLINE, 
                                       QUVI_SUPPORTS_TYPE_ANY) == QUVI_FALSE) {
        quvi_free(q);
        return NULL;
    }

    mp_msg(MSGT_OPEN, MSGL_INFO, "[quvi] Checking URL...\n");

    // Can use quvi_query_formats() to get a list of formats like this:
    // "fmt05_240p|fmt18_360p|fmt34_360p|fmt35_480p|fmt43_360p|fmt44_480p"
    // (This example is youtube specific.)
    // That call requires an extra net access. quvi_next_media_url() doesn't
    // seem to do anything useful. So we can't really do anything useful
    // except pass through the user's format setting.
    quvi_media_t m = quvi_media_new(q, (char *) url);
    if (quvi_ok(q) == QUVI_FALSE) {
        mp_msg(MSGT_OPEN, MSGL_ERR, "[quvi] %s\n", quvi_errmsg(q));
        quvi_free(q);
        return NULL;
    }
    quvi_media_stream_select(m, opts->quvi_format);
    if (quvi_ok(q) == QUVI_FALSE) {
        mp_msg(MSGT_OPEN, MSGL_ERR, "[quvi] %s\n", quvi_errmsg(q));
        quvi_free(q);
        return NULL;
    }

    struct mp_resolve_result *result =
        talloc_zero(NULL, struct mp_resolve_result);

    char *val;
    
    quvi_media_get(m, QUVI_MEDIA_STREAM_PROPERTY_URL, &val);
    if (quvi_ok(q) == QUVI_TRUE)
        result->url = talloc_strdup(result, val);

    quvi_media_get(m, QUVI_MEDIA_PROPERTY_TITLE, &val);
    if (quvi_ok(q) == QUVI_TRUE)
        result->title = talloc_strdup(result, val);

    quvi_media_free(m);
    quvi_free(q);

    if (!result->url) {
        talloc_free(result);
        result = NULL;
    }

    return result;
}
