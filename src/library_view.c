/* Non-mutating shelf indexes. SPDX-License-Identifier: GPL-3.0-or-later */
#include "library.h"
#include <stdlib.h>
#include <string.h>

static unsigned fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }
static bool contains(const char *text, const char *query)
{
    size_t n = strlen(query);
    if (!n) return true;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xc0) == 0x80) continue;
        size_t i = 0;
        while (i < n && p[i] && fold(p[i]) == fold((unsigned char)query[i])) ++i;
        if (i == n && (p[i] & 0xc0) != 0x80) return true;
    }
    return false;
}
static int compare(const Game *a, const Game *b, int mode)
{
    int section = library_section(a) - library_section(b);
    if (section) return section;
    if (mode == 1 && a->played != b->played) return a->played > b->played ? -1 : 1;
    const unsigned char *x = (const unsigned char *)a->title, *y = (const unsigned char *)b->title;
    while (*x && fold(*x) == fold(*y)) { ++x; ++y; }
    int title = (int)fold(*x) - (int)fold(*y);
    if (title) return title;
    int location = strcmp(a->location, b->location);
    return location ? location : strcmp(a->id, b->id);
}
/* Merge-sort indexes with an explicit snapshot context: no global qsort context,
 * no mutation of descriptors, O(n log n) even for a large reverse-sorted shelf. */
static void sort(int *indexes, int *temp, int begin, int end, const Library *lib, int mode)
{
    if (end - begin < 2) return;
    int mid = begin + (end - begin) / 2;
    sort(indexes, temp, begin, mid, lib, mode); sort(indexes, temp, mid, end, lib, mode);
    int a = begin, b = mid;
    for (int i = begin; i < end; ++i) {
        if (a < mid && (b == end || compare(&lib->games[indexes[a]], &lib->games[indexes[b]], mode) <= 0))
            temp[i] = indexes[a++];
        else temp[i] = indexes[b++];
    }
    memcpy(indexes + begin, temp + begin, (size_t)(end - begin) * sizeof(int));
}
bool library_view_build(LibraryView *view, const Library *lib, const UserMetadata *metadata,
                        const LibraryFilter *filter, int sort_mode)
{
    if (lib->count < 0 || (filter && !memchr(filter->search, 0, sizeof(filter->search)))) return false;
    int *indexes = lib->count ? malloc((size_t)lib->count * sizeof(int)) : NULL;
    int *temp = lib->count ? malloc((size_t)lib->count * sizeof(int)) : NULL;
    if (lib->count && (!indexes || !temp)) { free(indexes); free(temp); return false; }
    int count = 0;
    for (int i = 0; i < lib->count; ++i) {
        const Game *g = &lib->games[i];
        if (filter) {
            if (filter->section >= 0 && filter->section != library_section(g)) continue;
            if (filter->recent_only && !g->played) continue;
            if (filter->favorites_only) {
                const GameUserData *data = metadata ? user_metadata_find(metadata, g->id) : NULL;
                if (!data || !data->favorite) continue;
            }
            if (!contains(g->title, filter->search) && !contains(g->name, filter->search)) continue;
        }
        indexes[count++] = i;
    }
    sort(indexes, temp, 0, count, lib, sort_mode);
    free(temp); free(view->indices);
    *view = (LibraryView){.indices=indexes, .count=count, .generation=lib->generation};
    return true;
}
void library_view_free(LibraryView *view) { free(view->indices); memset(view, 0, sizeof(*view)); }
int library_view_selection(const LibraryView *view, const Library *lib, const Settings *settings)
{
    if (view->generation != lib->generation || !view->count) return -1;
    for (int i = 0; i < view->count; ++i)
        if (!strcmp(lib->games[view->indices[i]].id, settings->selected_game_id)) return i;
    return 0;
}
int library_view_restore_selection(const LibraryView *view, const Library *lib, Settings *settings)
{
    if (view->generation != lib->generation) return -1;
    /* The initial asynchronous shelf has no snapshot yet. Keep its saved
     * displayed position until a nonempty view can migrate it to an ID. */
    if (!settings->selected_game_id[0] && !view->count) return -1;
    if (!settings->selected_game_id[0]) {
        int row = settings->last_game < 0 ? 0 : settings->last_game;
        if (row >= view->count) row = view->count - 1;
        library_select(lib, settings, view->indices[row]);
    }
    library_restore_selection(lib, settings);
    return library_view_selection(view, lib, settings);
}
bool library_view_select(const LibraryView *view, const Library *lib, Settings *settings, int row)
{
    return view->generation == lib->generation && row >= 0 && row < view->count &&
           library_select(lib, settings, view->indices[row]);
}
int library_view_section_start(const LibraryView *view, const Library *lib, int section, int *count)
{
    int first = 0, n = 0;
    if (view->generation == lib->generation)
        for (int i = 0; i < view->count; ++i)
            if (library_section(&lib->games[view->indices[i]]) == section) { if (!n) first = i; ++n; }
    if (count) *count = n;
    return first;
}
