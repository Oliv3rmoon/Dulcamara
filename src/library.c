/*
 * Dulcamara - the game library (library.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "library.h"

#include "core/cores.h"
#include "rpgmaker.h"
#include "storage.h"

#include <errno.h>
#include <limits.h>

#include "../third_party/miniz/miniz.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_INI (64 * 1024)
#define MAX_LDB (64 * 1024 * 1024)

/* ---------------------------------------------------------------- helpers */

static void hash_id(const char *s, char *out, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    snprintf(out, size, "%016llx", (unsigned long long)h);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool is_archive_name(const char *name)
{
    const char *ext = path_ext(name);
    return !str_icmp(ext, "zip") || !str_icmp(ext, "easyrpg");
}

static bool is_image_name(const char *name)
{
    const char *ext = path_ext(name);
    return !str_icmp(ext, "png") || !str_icmp(ext, "bmp") || !str_icmp(ext, "xyz");
}

/* name without its extension equals want, ignoring ASCII case */
static bool stem_is(const char *name, const char *want)
{
    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    if (n != strlen(want) || n >= 256)
        return false;
    char stem[256];
    memcpy(stem, name, n);
    stem[n] = '\0';
    return rpgm_name_is(stem, want);
}

/* The real name of `want` in dir (any case) into out; false when it isn't there. */
static bool find_in_dir_checked(const char *dir, const char *want, char *out, size_t size, int *error)
{
    *error = 0;
    DIR *d = opendir(dir);
    if (!d) { *error = errno; return false; }
    bool found = false;
    while (!found) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { *error = errno; break; }
        if (rpgm_name_is(e->d_name, want)) {
            int n = snprintf(out, size, "%s/%s", dir, e->d_name);
            if (n < 0 || (size_t)n >= size) *error = ENAMETOOLONG;
            found = true;
        }
    }
    if (closedir(d) != 0 && !*error) *error = errno;
    return found && !*error;
}

static bool find_in_dir(const char *dir, const char *want, char *out, size_t size)
{
    int ignored;
    return find_in_dir_checked(dir, want, out, size, &ignored);
}

static char *read_file(const char *path, size_t limit, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *data = NULL;
    size_t used = 0, cap = 0;
    for (;;)
    {
        if (used + 4096 + 1 > cap)
        {
            if (cap >= limit)
                break;
            cap = cap ? cap * 2 : 16384;
            char *grown = realloc(data, cap);
            if (!grown)
                break;
            data = grown;
        }
        size_t got = fread(data + used, 1, cap - used - 1, f);
        used += got;
        if (got == 0)
            break;
    }
    fclose(f);
    if (data)
        data[used] = '\0';
    *len = used;
    return data;
}

static void title_from_ini(const char *ini, size_t len, Game *g)
{
    if (!ini || !rpgm_ini_title(ini, len, g->title, sizeof(g->title)))
        str_copy(g->title, sizeof(g->title), g->name);
}

/* ---------------------------------------------------------------- folders */

bool library_dir_is_game(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool ldb = false, lmt = false;
    for (struct dirent *e; (e = readdir(d));)
    {
        ldb |= rpgm_name_is(e->d_name, RPGM_DATABASE);
        lmt |= rpgm_name_is(e->d_name, RPGM_MAP_TREE);
    }
    closedir(d);
    return ldb && lmt;
}

/* below, with the rest of Ren'Py's; *error and where (the path that could not be read) on a failure */
static bool library_read_renpy(const char *dir, Game *g, int *error, char *where, size_t where_size);

static bool read_folder_checked(const char *dir, Game *g, int *error, char *where, size_t where_size)
{
    *error = 0;
    if (!library_dir_is_game(dir))
        return library_read_renpy(dir, g, error, where, where_size);
    memset(g, 0, sizeof(*g));
    str_copy(g->location, sizeof(g->location), dir);
    str_copy(g->path, sizeof(g->path), dir);
    rpgm_to_utf8(base_name(dir), strlen(base_name(dir)), 65001, g->name, sizeof(g->name));
    hash_id(dir, g->id, sizeof(g->id));
    g->core = CORE_EASYRPG;
    char ini_path[NP_PATH_MAX];
    size_t len = 0;
    char *ini = find_in_dir(dir, RPGM_INI, ini_path, sizeof(ini_path)) ? read_file(ini_path, MAX_INI, &len) : NULL;
    title_from_ini(ini, len, g);
    free(ini);
    return true;
}

bool library_read_folder(const char *dir, Game *g)
{
    int ignored;
    char where[NP_PATH_MAX];
    return read_folder_checked(dir, g, &ignored, where, sizeof(where));
}

/* ---------------------------------------------------------------- Ren'Py */

/* A transfer can create the scripts and archive headers before all archive data arrives. */
static bool unfinished_transfer(const char *name, size_t length)
{
    static const char suffix[] = ".ava-part";
    const size_t size = sizeof(suffix) - 1;
    return length >= size && !strncasecmp(name + length - size, suffix, size);
}

static bool renpy_file_name(const char *name)
{
    const char *ext = path_ext(name);
    const size_t len = strlen(name);
    return !str_icmp(ext, "rpy") || !str_icmp(ext, "rpyc") || !str_icmp(ext, "rpa") ||
           (len >= 7 && !str_icmp(name + len - 7, "_ren.py"));
}

/* Try scripts beside the current folder's assets first. Some releases keep every script several
 * folders below game/. Only recurse into real directories, and reuse the bounded path buffer. */
static bool renpy_files_in(char *path, size_t size, bool top, int *error)
{
    DIR *d = opendir(path);
    if (!d) {
        if (top) *error = errno;
        return false;
    }
    const size_t len = strlen(path);
    bool found = false;
    for (int pass = 0; pass < 2 && !found && !*error; ++pass) {
        if (pass) rewinddir(d);
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(d);
            if (!entry) {
                if (top) *error = errno;
                break;
            }
            const char *name = entry->d_name;
            if (name[0] == '.' || unfinished_transfer(name, strlen(name)) ||
                (!pass && !renpy_file_name(name)))
                continue;
            if (snprintf(path + len, size - len, "/%s", name) < (int)(size - len)) {
                struct stat st;
                if (!pass)
                    found = !stat(path, &st) && S_ISREG(st.st_mode);
                else if (!lstat(path, &st) && S_ISDIR(st.st_mode))
                    found = renpy_files_in(path, size, false, error);
            }
            path[len] = 0;
            if (found) break;
        }
    }
    if (closedir(d) && top && !*error) *error = errno;
    return found && !*error;
}

/* Return the content directory of a recognized Ren'Py game. */
static bool renpy_game_dir(const char *dir, char *game_dir, size_t size, int *error, char *where, size_t where_size)
{
    where[0] = '\0';
    if (!find_in_dir_checked(dir, "game", game_dir, size, error))
    {
        if (*error) str_copy(where, where_size, dir);
        return false;
    }
    struct stat st;
    if (stat(game_dir, &st) != 0) { *error = errno; str_copy(where, where_size, game_dir); return false; }
    if (!S_ISDIR(st.st_mode)) return false;
    bool found = renpy_files_in(game_dir, size, true, error);
    if (*error) str_copy(where, where_size, game_dir);
    return found;
}

/* The number that starts the value of `key` in a Python file's line "key = '8.3.7...'" or "key = (7, 4, ...)". */
static int version_in(const char *text, const char *key)
{
    const size_t n = strlen(key);
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
    {
        if (strncmp(line, key, n))
            continue;
        const char *p = line + n;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p++ != '=')
            continue;
        while (*p == ' ' || *p == '\t' || *p == 'u' || *p == 'b' || *p == '(' || *p == '\'' || *p == '"')
            ++p;
        if (*p >= '0' && *p <= '9')
            return atoi(p);
    }
    return 0;
}

/* The Ren'Py version of the Python a game ships, by its lib/ folder: py3-<platform>/ is Ren'Py 8's, py2-<platform>/
 * Ren'Py 7.4's and later, pythonlib2.7/ the older ones'; 0 when it has none of them. */
static int renpy_lib_version(const char *dir)
{
    char lib[NP_PATH_MAX];
    if (!find_in_dir(dir, "lib", lib, sizeof(lib)))
        return 0;
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    int version = 0;
    for (struct dirent *e; !version && (e = readdir(d));)
    {
        if (!strncmp(e->d_name, "py3-", 4))
            version = 8;
        else if (!strncmp(e->d_name, "py2-", 4) || !strcmp(e->d_name, "pythonlib2.7"))
            version = 7;
    }
    closedir(d);
    return version;
}

/* The game's Ren'Py version: the major number of renpy/vc_version.py's version, or of renpy/__init__.py's
 * version_tuple in older releases, else what its lib/ folder holds; 0 when nothing says (some repacks). */
static int renpy_version(const char *dir)
{
    static const struct
    {
        const char *file, *key;
    } SOURCES[] = {{"vc_version.py", "version"}, {"__init__.py", "version_tuple"}};
    char renpy_dir[NP_PATH_MAX], path[NP_PATH_MAX];
    if (find_in_dir(dir, "renpy", renpy_dir, sizeof(renpy_dir)))
        for (size_t i = 0; i < sizeof(SOURCES) / sizeof(SOURCES[0]); ++i)
        {
            size_t len = 0;
            char *text = find_in_dir(renpy_dir, SOURCES[i].file, path, sizeof(path))
                             ? read_file(path, MAX_INI, &len)
                             : NULL;
            const int version = text ? version_in(text, SOURCES[i].key) : 0;
            free(text);
            if (version)
                return version;
        }
    return renpy_lib_version(dir);
}

/* The name of the game's launcher beside game/ (<name>.exe or <name>.sh, not the 32-bit <name>-32.exe): the first,
 * by name, of those with Ren'Py's <name>.py beside them (so not an installer's unins000.exe), else of all. */
static bool renpy_launcher_name(const char *dir, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    char best[256] = "", best_with_py[256] = "";
    for (struct dirent *e; (e = readdir(d));)
    {
        const char *ext = path_ext(e->d_name);
        if (e->d_name[0] == '.' || (str_icmp(ext, "exe") && str_icmp(ext, "sh")))
            continue;
        size_t n = (size_t)(strrchr(e->d_name, '.') - e->d_name);
        if (n == 0 || n + 4 >= sizeof(best) || (n > 3 && !strncmp(e->d_name + n - 3, "-32", 3)))
            continue;
        char stem[256], script[260], path[NP_PATH_MAX];
        memcpy(stem, e->d_name, n);
        stem[n] = '\0';
        if (!best[0] || str_icmp(stem, best) < 0)
            str_copy(best, sizeof(best), stem);
        snprintf(script, sizeof(script), "%s.py", stem);
        if (find_in_dir(dir, script, path, sizeof(path)) && (!best_with_py[0] || str_icmp(stem, best_with_py) < 0))
            str_copy(best_with_py, sizeof(best_with_py), stem);
    }
    closedir(d);
    const char *name = best_with_py[0] ? best_with_py : best;
    if (name[0])
        rpgm_to_utf8(name, strlen(name), 65001, out, size);
    return name[0] != '\0';
}

static bool library_read_renpy(const char *dir, Game *g, int *error, char *where, size_t where_size)
{
    char game_dir[NP_PATH_MAX];
    if (!renpy_game_dir(dir, game_dir, sizeof(game_dir), error, where, where_size))
        return false;
    memset(g, 0, sizeof(*g));
    str_copy(g->location, sizeof(g->location), dir);
    str_copy(g->path, sizeof(g->path), dir);
    rpgm_to_utf8(base_name(dir), strlen(base_name(dir)), 65001, g->name, sizeof(g->name));
    hash_id(dir, g->id, sizeof(g->id));
    g->engine = renpy_version(dir);
    /* Ren'Py 7 and 6 games (Python 2) on Ren'Py 7's core; Ren'Py 8's, and those that do not say, on Ren'Py 8's */
    g->core = g->engine >= 1 && g->engine <= 7 ? CORE_RENPY7 : CORE_RENPY;
    if (!renpy_launcher_name(dir, g->title, sizeof(g->title)))
        str_copy(g->title, sizeof(g->title), g->name);
    return true;
}

/* game/gui/main_menu.png, else game/gui/window_icon.png */
static bool renpy_title_image(const Game *g, char *out, size_t size)
{
    char game_dir[NP_PATH_MAX], gui[NP_PATH_MAX];
    if (!find_in_dir(g->location, "game", game_dir, sizeof(game_dir)) || !find_in_dir(game_dir, "gui", gui, sizeof(gui)))
        return false;
    return find_in_dir(gui, "main_menu.png", out, size) || find_in_dir(gui, "window_icon.png", out, size);
}

/* ---------------------------------------------------------------- archives */

/* An entry's name with '/' between folders (some Windows tools wrote '\'). */
static void entry_name(mz_zip_archive *zip, mz_uint i, char *out, size_t size)
{
    out[0] = '\0';
    mz_zip_reader_get_filename(zip, i, out, (mz_uint)size);
    for (char *p = out; *p; ++p)
        if (*p == '\\')
            *p = '/';
}

/* The entry called dir + want (want in any case); -1 when there is none. */
static int find_entry(mz_zip_archive *zip, const char *dir, const char *want)
{
    const size_t dn = strlen(dir);
    const mz_uint n = mz_zip_reader_get_num_files(zip);
    char name[NP_PATH_MAX];
    for (mz_uint i = 0; i < n; ++i)
    {
        entry_name(zip, i, name, sizeof(name));
        if (strlen(name) > dn && !strncmp(name, dir, dn) && !strchr(name + dn, '/') && rpgm_name_is(name + dn, want))
            return (int)i;
    }
    return -1;
}

static char *extract(mz_zip_archive *zip, int index, size_t limit, size_t *len)
{
    mz_zip_archive_file_stat st;
    if (index < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)index, &st) || st.m_uncomp_size > limit)
        return NULL;
    size_t got = 0;
    char *data = mz_zip_reader_extract_to_heap(zip, (mz_uint)index, &got, 0);
    if (!data)
        return NULL;
    char *text = realloc(data, got + 1); /* a terminator for the ini's text */
    if (!text)
    {
        mz_free(data);
        return NULL;
    }
    text[got] = '\0';
    *len = got;
    return text;
}

#define REASON_RENPY_ZIP "a Ren'Py game still inside a .zip: extract it into a folder"
#define REASON_ZIP_TOO_LARGE "this ZIP is too large for the RPG Maker player: extract it into a folder"

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Whether EasyRPG's ZIP reader (filesystem_zip.cpp, which has no ZIP64) reads all n entries miniz read. It reads only
 * the classic end record (the last "PK\5\6" in the file's final 64 KiB), the central directory at its 32-bit offset,
 * entry by entry while their signature holds, and, as a file is opened, its local header; offsets and sizes are 32-bit
 * and 0xFFFFFFFF (ZIP64's "see the extra field") is not understood. A ZIP64 end record beside valid classic fields,
 * as some tools always write, is never read by it. */
static bool easyrpg_reads(mz_zip_archive *zip, mz_uint n)
{
    const mz_uint64 size = zip->m_archive_size;
    if (size > 0xFFFFFFFFull || n > 0xFFFFu || size < 22)
        return false;
    /* FindCentralDirectory: 65,535 bytes up to the last 18, scanned back for the signature */
    enum { WINDOW = 0xFFFF, TAIL = 18 };
    const mz_uint64 start = size >= WINDOW + TAIL ? size - WINDOW - TAIL : 0;
    const size_t len = (size_t)(size >= WINDOW + TAIL ? WINDOW : (size < WINDOW ? size : WINDOW));
    unsigned char *tail = malloc(len + TAIL);
    const size_t got = tail ? zip->m_pRead(zip->m_pIO_opaque, start, tail, (size_t)(size - start)) : 0;
    long at = -1;
    for (long i = (long)len - 4; got == size - start && i >= 0 && at < 0; --i)
        if (!memcmp(tail + i, "PK\5\6", 4))
            at = i;
    const bool found = at >= 0 && (size_t)at + 20 <= got;
    const uint32_t directory = found ? le32(tail + at + 16) : 0;
    free(tail);
    if (!found || directory == 0xFFFFFFFFu)
        return false;
    /* ReadCentralDirectoryEntry, then each ReadLocalHeader */
    mz_uint64 at_entry = directory;
    mz_uint walked = 0;
    for (;;)
    {
        unsigned char cdh[46], ldh[30];
        if (at_entry + sizeof(cdh) > size || zip->m_pRead(zip->m_pIO_opaque, at_entry, cdh, sizeof(cdh)) != sizeof(cdh) ||
            le32(cdh) != 0x02014b50u)
            break;
        const uint32_t compressed = le32(cdh + 20), uncompressed = le32(cdh + 24), local = le32(cdh + 42);
        if (compressed == 0xFFFFFFFFu || uncompressed == 0xFFFFFFFFu || local == 0xFFFFFFFFu || ++walked > n)
            return false;
        if ((mz_uint64)local + sizeof(ldh) > size ||
            zip->m_pRead(zip->m_pIO_opaque, local, ldh, sizeof(ldh)) != sizeof(ldh) || le32(ldh) != 0x04034b50u ||
            le32(ldh + 18) == 0xFFFFFFFFu || le32(ldh + 22) == 0xFFFFFFFFu)
            return false;
        at_entry += sizeof(cdh) + (mz_uint64)(cdh[28] | cdh[29] << 8) + (cdh[30] | cdh[31] << 8) +
                    (cdh[32] | cdh[33] << 8);
    }
    return walked == n;
}

/* An archive holding no RPG Maker game: a Ren'Py game's scripts or archives in it are worth saying so. */
static void archive_reason(mz_zip_archive *zip, char *reason, size_t reason_size)
{
    const mz_uint n = mz_zip_reader_get_num_files(zip);
    char name[NP_PATH_MAX];
    for (mz_uint i = 0; i < n && reason_size; ++i)
    {
        entry_name(zip, i, name, sizeof(name));
        if (renpy_file_name(base_name(name)))
        {
            str_copy(reason, reason_size, REASON_RENPY_ZIP);
            return;
        }
    }
}

/* library_read_archive, with why an archive that holds a game is not one to list in reason ("" when there is
 * nothing to say). */
static bool read_archive_checked(const char *archive, Game *g, char *reason, size_t reason_size)
{
    if (reason_size)
        reason[0] = '\0';
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, archive, 0))
        return false;
    /* the shallowest folder holding RPG_RT.ldb and RPG_RT.lmt */
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    char name[NP_PATH_MAX], best[NP_PATH_MAX] = "";
    int best_depth = -1;
    for (mz_uint i = 0; i < n; ++i)
    {
        entry_name(&zip, i, name, sizeof(name));
        const char *file = base_name(name);
        if (!rpgm_name_is(file, RPGM_DATABASE))
            continue;
        char dir[NP_PATH_MAX];
        size_t dn = (size_t)(file - name);
        memcpy(dir, name, dn);
        dir[dn] = '\0';
        int depth = 0;
        for (const char *p = dir; *p; ++p)
            depth += *p == '/';
        if ((best_depth < 0 || depth < best_depth) && find_entry(&zip, dir, RPGM_MAP_TREE) >= 0)
        {
            str_copy(best, sizeof(best), dir);
            best_depth = depth;
        }
    }
    if (best_depth < 0)
    {
        archive_reason(&zip, reason, reason_size);
        mz_zip_reader_end(&zip);
        return false;
    }
    /* EasyRPG's ZIP reader has no ZIP64: an archive past its limits would be listed and then fail to start */
    if (!easyrpg_reads(&zip, n))
    {
        if (reason_size)
            str_copy(reason, reason_size, REASON_ZIP_TOO_LARGE);
        mz_zip_reader_end(&zip);
        return false;
    }
    memset(g, 0, sizeof(*g));
    g->archive = true;
    g->core = CORE_EASYRPG;
    str_copy(g->location, sizeof(g->location), archive);
    str_copy(g->inner, sizeof(g->inner), best);
    hash_id(archive, g->id, sizeof(g->id));
    rpgm_to_utf8(base_name(archive), strlen(base_name(archive)), 65001, g->name, sizeof(g->name));
    char *dot = strrchr(g->name, '.');
    if (dot && dot != g->name)
        *dot = '\0';
    /* EasyRPG names the folders inside an archive in UTF-8: an older archive's names are in the game's code page */
    char inner[NP_PATH_MAX];
    if (rpgm_guess_codepage(best, strlen(best)) != 65001)
        rpgm_to_utf8(best, strlen(best), 0, inner, sizeof(inner));
    else
        str_copy(inner, sizeof(inner), best);
    size_t in = strlen(inner);
    if (in && inner[in - 1] == '/')
        inner[in - 1] = '\0';
    if (inner[0])
        snprintf(g->path, sizeof(g->path), "%s/%s", archive, inner);
    else
        str_copy(g->path, sizeof(g->path), archive);
    size_t len = 0;
    char *ini = extract(&zip, find_entry(&zip, best, RPGM_INI), MAX_INI, &len);
    title_from_ini(ini, len, g);
    free(ini);
    mz_zip_reader_end(&zip);
    return true;
}

bool library_read_archive(const char *archive, Game *g)
{
    return read_archive_checked(archive, g, NULL, 0);
}

/* ---------------------------------------------------------------- the library */

static bool add(Library *lib, const Game *g)
{
    if (lib->count == lib->capacity)
    {
        int cap = lib->capacity ? lib->capacity * 2 : 32;
        Game *grown = realloc(lib->games, (size_t)cap * sizeof(Game));
        if (!grown)
            return false;
        lib->games = grown;
        lib->capacity = cap;
    }
    lib->games[lib->count++] = *g;
    return true;
}

static bool skipped_name(const char *name)
{
    size_t n = strlen(name);
    /* hidden files, and the folders EasyRPG keeps an archive game's saves in (<archive>.save) */
    return name[0] == '.' || (n > 5 && !str_icmp(name + n - 5, ".save"));
}

#define REASON_UNFINISHED "unfinished transfer: finish copying the game, then rescan"

/* What the scan says of what it does not list (LibrarySkip.reason). */
#define REASON_MV "an RPG Maker MV/MZ game: Dulcamara plays RPG Maker 2000/2003 and Ren'Py games"
#define REASON_RGSS "an RPG Maker XP/VX/VX Ace game: Dulcamara plays RPG Maker 2000/2003 and Ren'Py games"
#define REASON_NONE "no supported game found here (RPG Maker 2000/2003 or Ren'Py)"
#define REASON_DEEP "nested too deep: move it up, at most two folders inside games/"
#define REASON_LONG "path too long: shorten its folder names or move it up"
#define REASON_LINK_OUT "a link to a folder outside the games folder: not followed"

/* games/, games/<A>/ and games/<A>/<B>/ hold the games listed; a game one folder further down is reported */
#define SCAN_DEPTH 2

typedef struct
{
    Library *lib;
    LibraryScanProgress progress;
    void *context;
    unsigned visited;
    const char *root;
    size_t root_len;
    char root_real[PATH_MAX];
    int root_real_state; /* 0 not looked up yet, 1 root_real is it, -1 it has none */
    char failed[NP_PATH_MAX + 8]; /* what a failed scan could not read, relative to root when in it */
} Scan;

static void note_failure(Scan *s, const char *path, const char *name)
{
    if (s->failed[0])
        return;
    const int saved = errno;
    const char *rel = path;
    if (!strcmp(path, s->root))
        rel = "";
    else if (!strncmp(path, s->root, s->root_len) && path[s->root_len] == '/')
        rel = path + s->root_len + 1;
    if (name)
        snprintf(s->failed, sizeof(s->failed), "%s%s%s", rel, rel[0] ? "/" : "", name);
    else
        snprintf(s->failed, sizeof(s->failed), "%s", rel[0] ? rel : path);
    errno = saved;
}

static bool add_skip(Scan *s, const char *path, const char *reason)
{
    Library *lib = s->lib;
    if (lib->skipped_count == lib->skipped_capacity)
    {
        int cap = lib->skipped_capacity ? lib->skipped_capacity * 2 : 8;
        LibrarySkip *grown = realloc(lib->skipped, (size_t)cap * sizeof(LibrarySkip));
        if (!grown) { errno = ENOMEM; return false; }
        lib->skipped = grown;
        lib->skipped_capacity = cap;
    }
    LibrarySkip *skip = &lib->skipped[lib->skipped_count++];
    str_copy(skip->path, sizeof(skip->path), path);
    str_copy(skip->reason, sizeof(skip->reason), reason);
    return true;
}

/* An entry two folders down (where the scan did not look before) that could not be read: reported, not a failure. */
static bool skip_unreadable(Scan *s, const char *path, int error)
{
    char reason[128];
    snprintf(reason, sizeof(reason), "could not be read: %s", strerror(error));
    return add_skip(s, path, reason);
}

/* An entry whose path does not fit the app's paths (NP_PATH_MAX): reported by as much of its path as fits, cut at a
 * whole UTF-8 character. No game there could be listed, so it never fails the scan. */
static bool skip_too_long(Scan *s, const char *dir, const char *name)
{
    char path[NP_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    const size_t n = strlen(path);
    size_t start = n; /* where the last character starts */
    while (start > 0 && ((unsigned char)path[start - 1] & 0xC0) == 0x80)
        --start;
    if (start > 0)
    {
        const unsigned char lead = (unsigned char)path[start - 1];
        const size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (n - (start - 1) < need)
            path[start - 1] = '\0';
    }
    return add_skip(s, path, REASON_LONG);
}

/* Whether the scan goes into a folder: not through a link that leads out of the games folder. */
static bool may_enter(Scan *s, const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode))
        return true;
    if (!s->root_real_state)
        s->root_real_state = realpath(s->root, s->root_real) ? 1 : -1;
    char target[PATH_MAX];
    if (s->root_real_state < 0 || !realpath(path, target))
        return false;
    const size_t n = strlen(s->root_real);
    return !strncmp(target, s->root_real, n) && (target[n] == '/' || (n == 1 && target[0] == '/'));
}

enum { LOOKS_NOTHING, LOOKS_GAME, LOOKS_MV, LOOKS_RGSS };

static bool ends_with(const char *name, const char *tail)
{
    const size_t n = strlen(name), t = strlen(tail);
    return n > t && rpgm_name_is(name + n - t, tail);
}

/* What a folder that is not a game looks like, by the names in it (and an RPG Maker XP-to-VX Ace Game.ini's
 * Library= line): another engine's game, a game of some kind, or nothing in particular. Only a report: a folder that
 * cannot be read looks like nothing. */
static int folder_looks(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return LOOKS_NOTHING;
    bool mv = false, rgss = false, game = false;
    char js[NP_PATH_MAX] = "", ini[NP_PATH_MAX] = "";
    for (struct dirent *e; (e = readdir(d));)
    {
        const char *name = e->d_name, *ext = path_ext(name);
        if (name[0] == '.')
            continue;
        if (rpgm_name_is(name, "www"))
            mv = true;
        else if (rpgm_name_is(name, "js"))
            snprintf(js, sizeof(js), "%s/%s", dir, name);
        else if (!str_icmp(ext, "rgssad") || !str_icmp(ext, "rgss2a") || !str_icmp(ext, "rgss3a"))
            rgss = true;
        else if (rpgm_name_is(name, "Game.ini"))
            snprintf(ini, sizeof(ini), "%s/%s", dir, name);
        else if (!str_icmp(ext, "exe") || rpgm_name_is(name, "game") || rpgm_name_is(name, RPGM_DATABASE) ||
                 rpgm_name_is(name, RPGM_MAP_TREE) || rpgm_name_is(name, "package.json") ||
                 rpgm_name_is(name, "UnityPlayer.dll") || ends_with(name, "_Data") || renpy_file_name(name))
            game = true;
    }
    closedir(d);
    char found[NP_PATH_MAX];
    if (mv || (js[0] && (find_in_dir(js, "rpg_core.js", found, sizeof(found)) ||
                         find_in_dir(js, "rmmz_core.js", found, sizeof(found)))))
        return LOOKS_MV;
    if (!rgss && ini[0])
    {
        size_t len = 0;
        char *text = read_file(ini, MAX_INI, &len);
        static const char RGSS[] = "rgss"; /* Library=RGSS104E.dll, System\RGSS301.dll... */
        for (size_t i = 0; text && i + 4 <= len && !rgss; ++i)
        {
            size_t k = 0;
            while (k < 4 && (text[i + k] | 0x20) == RGSS[k])
                ++k;
            rgss = k == 4;
        }
        free(text);
        game = true;
    }
    return rgss ? LOOKS_RGSS : game ? LOOKS_GAME : LOOKS_NOTHING;
}

/* Return -1 on an incomplete scan (where, when it is not path, the path that could not be read), 0 for a non-game
 * (reason, when it is one worth a word), 1 for an added game. */
static int try_entry(Library *lib, const char *path, const char *name, bool *is_dir, char *where, size_t where_size,
                     char *reason, size_t reason_size)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *is_dir = S_ISDIR(st.st_mode);
    if (*is_dir) {
        DIR *check = opendir(path);
        if (!check) return -1;
        if (closedir(check) != 0) return -1;
    } else if (is_archive_name(name)) {
        FILE *check = fopen(path, "rb");
        if (!check) return -1;
        if (fclose(check) != 0) return -1;
    } else {
        const char *ext = path_ext(name);
        if (!str_icmp(ext, "7z") || !str_icmp(ext, "rar"))
            snprintf(reason, reason_size, "still inside a .%s archive: extract it into a folder",
                     !str_icmp(ext, "7z") ? "7z" : "rar");
        return 0;
    }
    Game game; int detection_error = 0;
    bool found = *is_dir ? read_folder_checked(path, &game, &detection_error, where, where_size)
                        : read_archive_checked(path, &game, reason, reason_size);
    if (detection_error) { errno = detection_error; return -1; }
    if (found && !add(lib, &game)) { errno = ENOMEM; return -1; }
    return found ? 1 : 0;
}

/* The games and archives in the folders of a folder one past the deepest the scan lists: each is reported as nested
 * too deep. Only a report: what cannot be read here is passed over. */
static bool report_deeper(Scan *s, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return true;
    bool ok = true;
    for (struct dirent *e; ok && (e = readdir(d));)
    {
        if (skipped_name(e->d_name))
            continue;
        if (s->progress && !s->progress(s->context, s->visited, (unsigned)s->lib->count)) {
            errno = ECANCELED; ok = false; break;
        }
        char path[NP_PATH_MAX], where[NP_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
            continue;
        if (unfinished_transfer(e->d_name, strlen(e->d_name))) {
            ok = add_skip(s, path, REASON_UNFINISHED);
            continue;
        }
        Game game;
        int error = 0;
        bool found = is_archive_name(e->d_name) ? library_read_archive(path, &game)
                     : may_enter(s, path) && read_folder_checked(path, &game, &error, where, sizeof(where)) && !error;
        if (found)
            ok = add_skip(s, path, REASON_DEEP);
    }
    closedir(d);
    return ok;
}

/* Where an entry is: in nothing in particular, inside a folder that looks like a game (whose report covers "no
 * supported game found" beneath it), or inside one that looks like another engine's game (whose report covers all
 * beneath it). */
enum { INSIDE_NOTHING, INSIDE_GAME_LIKE, INSIDE_OTHER_ENGINE };

static bool scan_dir(Scan *s, const char *dir, int depth, int inside);

/* One entry of a folder at depth: listed, reported, or (a folder) looked into. */
static bool scan_entry(Scan *s, const char *path, const char *name, int depth, int inside)
{
    if (unfinished_transfer(name, strlen(name)))
        return add_skip(s, path, REASON_UNFINISHED);
    bool is_dir = false;
    char where[NP_PATH_MAX] = "", reason[128] = "";
    int result = try_entry(s->lib, path, name, &is_dir, where, sizeof(where), reason, sizeof(reason));
    /* The games and folders of games/ and of the folders in it (depths 0 and 1) were listed before games two folders
     * down were looked for: a failure there fails the scan, which keeps the library it had, so that no game already
     * listed is dropped. Two folders down, and for a path too long to hold a game at any depth, it is reported. */
    if (result < 0 && errno == ENAMETOOLONG)
        return add_skip(s, path, REASON_LONG);
    if (result < 0 && depth == SCAN_DEPTH && errno != ENOMEM)
        return skip_unreadable(s, path, errno);
    if (result < 0) { note_failure(s, where[0] ? where : path, NULL); return false; }
    if (result > 0)
        return true;
    if (reason[0])
        return add_skip(s, path, reason);
    if (!is_dir)
        return true;
    if (!may_enter(s, path))
        return add_skip(s, path, REASON_LINK_OUT);
    /* A folder that looks like another engine's game is looked into too: a collection with a loose www/ or .rgss3a
     * beside its games still lists them. It is reported only when nothing beneath it is listed. */
    const int looks = folder_looks(path);
    const bool other_engine = looks == LOOKS_MV || looks == LOOKS_RGSS;
    const int beneath = other_engine || inside == INSIDE_OTHER_ENGINE           ? INSIDE_OTHER_ENGINE
                        : inside == INSIDE_GAME_LIKE || looks == LOOKS_GAME ? INSIDE_GAME_LIKE
                                                                              : INSIDE_NOTHING;
    const int games = s->lib->count, skips = s->lib->skipped_count;
    if (!(depth < SCAN_DEPTH ? scan_dir(s, path, depth + 1, beneath) : report_deeper(s, path)))
        return false;
    if (s->lib->count != games || inside == INSIDE_OTHER_ENGINE)
        return true;
    if (other_engine)
        return add_skip(s, path, looks == LOOKS_MV ? REASON_MV : REASON_RGSS);
    if (looks == LOOKS_GAME && inside == INSIDE_NOTHING && s->lib->skipped_count == skips)
        return add_skip(s, path, REASON_NONE);
    return true;
}

static bool scan_dir(Scan *s, const char *dir, int depth, int inside)
{
    DIR *d = opendir(dir);
    if (!d) { note_failure(s, dir, NULL); return false; }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(d);
        if (!entry) { ok = errno == 0; if (!ok) note_failure(s, dir, NULL); break; }
        if (skipped_name(entry->d_name)) continue;
        if (s->progress && !s->progress(s->context, s->visited, (unsigned)s->lib->count)) {
            errno = ECANCELED; ok = false; break;
        }
        ++s->visited;
        char path[NP_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
            if (!skip_too_long(s, dir, entry->d_name)) { ok = false; break; }
            continue;
        }
        if (!scan_entry(s, path, entry->d_name, depth, inside)) { ok = false; break; }
    }
    int saved = errno;
    if (closedir(d) != 0 && ok) { saved = errno; ok = false; note_failure(s, dir, NULL); }
    errno = saved;
    return ok;
}

bool library_scan_checked(Library *lib, const char *root, LibraryScanProgress progress,
                          void *context, char *error, size_t error_size)
{
    Library next = {0};
    if (error_size) error[0] = 0;
    Scan *s = calloc(1, sizeof(*s));
    if (!s) {
        if (error_size) snprintf(error, error_size, "Cannot scan games: %s", strerror(ENOMEM));
        return false;
    }
    *s = (Scan){.lib = &next, .progress = progress, .context = context, .root = root, .root_len = strlen(root)};
    bool ok = scan_dir(s, root, 0, INSIDE_NOTHING);
    if (ok && progress && !progress(context, s->visited, (unsigned)next.count)) {
        errno = ECANCELED; ok = false;
    }
    if (!ok) {
        const int saved = errno;
        if (error_size) {
            if (s->failed[0] && saved != ECANCELED)
                snprintf(error, error_size, "Cannot scan games: %s: %s", s->failed, strerror(saved));
            else
                snprintf(error, error_size, "Cannot scan games: %s", strerror(saved));
        }
        free(s);
        library_free(&next); return false;
    }
    free(s);
    for (int i = 0; i < next.skipped_count; ++i)
        np_log("library: not shown: %s: %s", next.skipped[i].path, next.skipped[i].reason);
    library_free(lib); *lib = next;
    return true;
}

void library_scan(Library *lib, const char *root)
{
    library_scan_checked(lib, root, NULL, NULL, NULL, 0);
}

static bool utf8_string(const char *s, size_t capacity)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = strnlen(s, capacity);
    if (n == capacity) return false;
    for (size_t i = 0; i < n;) {
        unsigned c = p[i++];
        if (c < 128) continue;
        unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!extra || i + extra > n) return false;
        if ((c == 0xe0 && p[i] < 0xa0) || (c == 0xed && p[i] >= 0xa0) ||
            (c == 0xf0 && p[i] < 0x90) || (c == 0xf4 && p[i] >= 0x90)) return false;
        while (extra--) if ((p[i++] & 0xc0) != 0x80) return false;
    }
    return true;
}

bool library_game_valid(const Game *g, const char *root)
{
    if (!utf8_string(g->title, sizeof(g->title)) || !g->title[0] ||
        !utf8_string(g->name, sizeof(g->name)) || !g->name[0] ||
        !memchr(g->location, 0, sizeof(g->location)) || !g->location[0] ||
        !memchr(g->path, 0, sizeof(g->path)) || !g->path[0] ||
        !memchr(g->inner, 0, sizeof(g->inner)) || !memchr(g->id, 0, sizeof(g->id)) ||
        !game_id_valid(g->id) || g->core < 0 || g->core >= NP_CORES || g->played < 0 ||
        (g->engine != 0 && g->engine != 2000 && g->engine != 2003 && (g->engine < 1 || g->engine > 8))) return false;
    char id[24]; hash_id(g->location, id, sizeof(id));
    if (strcmp(id, g->id)) return false;
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') --n;
    if (strncmp(root, g->location, n) || g->location[n] != '/') return false;
    /* in games/ or up to SCAN_DEPTH folders down, each folder a real name (not "", "." or "..") */
    const char *relative = g->location + n + 1;
    int folders = 0;
    for (const char *part = relative;; ++folders) {
        const char *slash = strchr(part, '/');
        size_t len = slash ? (size_t)(slash - part) : strlen(part);
        if (!len || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (unfinished_transfer(part, len)) return false;
        if (!slash) break;
        part = slash + 1;
    }
    if (folders > SCAN_DEPTH) return false;
    if (!g->archive) return !g->inner[0] && !strcmp(g->location, g->path);
    if (g->core != CORE_EASYRPG || !is_archive_name(g->location) || g->inner[0] == '/' ||
        strstr(g->inner, "../") || strchr(g->inner, '\\')) return false;
    /* path may contain the archive's codepage-converted inner name. */
    n = strlen(g->location);
    return !strncmp(g->path, g->location, n) && (!g->path[n] || g->path[n] == '/');
}

bool library_revalidate(const Game *g)
{
    Game actual;
    bool found = g->archive ? library_read_archive(g->location, &actual) : library_read_folder(g->location, &actual);
    return found && !strcmp(actual.id, g->id) && !strcmp(actual.path, g->path) && actual.core == g->core;
}

void library_free(Library *lib)
{
    free(lib->games);
    free(lib->skipped);
    memset(lib, 0, sizeof(*lib));
}

int library_section(const Game *g)
{
    return core_is_renpy(g->core) ? SECTION_RENPY : SECTION_RPG_MAKER;
}

int library_section_start(const Library *lib, int section, int *count)
{
    int first = -1, n = 0;
    for (int i = 0; i < lib->count; ++i)
        if (library_section(&lib->games[i]) == section)
        {
            if (first < 0)
                first = i;
            ++n;
        }
    if (count)
        *count = n;
    return first < 0 ? 0 : first;
}

static int by_title(const void *a, const void *b)
{
    const Game *x = a, *y = b;
    if (library_section(x) != library_section(y))
        return library_section(x) - library_section(y);
    int c = str_icmp(x->title, y->title);
    return c ? c : strcmp(x->location, y->location);
}

static int by_played(const void *a, const void *b)
{
    const Game *x = a, *y = b;
    if (library_section(x) != library_section(y))
        return library_section(x) - library_section(y);
    if (x->played != y->played)
        return x->played > y->played ? -1 : 1;
    return by_title(a, b);
}

void library_sort(Library *lib, int mode)
{
    if (lib->count > 1)
        qsort(lib->games, (size_t)lib->count, sizeof(Game), mode == 1 ? by_played : by_title);
}

/* ---------------------------------------------------------------- the title screen's picture */

/* Of the names in Title/, the one the database names (as its bytes, or converted to UTF-8 as the copy may have
 * been), else the only picture there, else the first in alphabetical order. */
typedef struct
{
    char raw[256], utf8[256];
    char named[NP_PATH_MAX], first[NP_PATH_MAX];
    int images;
} Pick;

static void pick_init(Pick *p, const char *title_name)
{
    memset(p, 0, sizeof(*p));
    str_copy(p->raw, sizeof(p->raw), title_name);
    if (title_name[0])
        rpgm_to_utf8(title_name, strlen(title_name), 0, p->utf8, sizeof(p->utf8));
}

static void pick_offer(Pick *p, const char *file, const char *full)
{
    if (!is_image_name(file))
        return;
    ++p->images;
    if (!p->named[0] && p->raw[0] && (stem_is(file, p->raw) || (p->utf8[0] && stem_is(file, p->utf8))))
        str_copy(p->named, sizeof(p->named), full);
    if (!p->first[0] || strcmp(full, p->first) < 0)
        str_copy(p->first, sizeof(p->first), full);
}

static bool pick_result(const Pick *p, char *out, size_t size)
{
    const char *chosen = p->named[0] ? p->named : p->first[0] ? p->first : NULL;
    if (!chosen)
        return false;
    str_copy(out, size, chosen);
    return true;
}

static bool folder_title_image(Game *g, char *out, size_t size)
{
    char path[NP_PATH_MAX], title_name[256] = "";
    size_t len = 0;
    char *ldb = find_in_dir(g->location, RPGM_DATABASE, path, sizeof(path)) ? read_file(path, MAX_LDB, &len) : NULL;
    if (ldb)
        rpgm_ldb_info((const uint8_t *)ldb, len, title_name, sizeof(title_name), &g->engine);
    free(ldb);
    char title_dir[NP_PATH_MAX];
    if (!find_in_dir(g->location, "Title", title_dir, sizeof(title_dir)))
        return false;
    Pick pick;
    pick_init(&pick, title_name);
    DIR *d = opendir(title_dir);
    if (!d)
        return false;
    for (struct dirent *e; (e = readdir(d));)
    {
        char full[NP_PATH_MAX];
        path_join(full, sizeof(full), title_dir, e->d_name);
        pick_offer(&pick, e->d_name, full);
    }
    closedir(d);
    return pick_result(&pick, out, size);
}

static bool archive_title_image(Game *g, char *out, size_t size)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, g->location, 0))
        return false;
    char title_name[256] = "";
    size_t len = 0;
    char *ldb = extract(&zip, find_entry(&zip, g->inner, RPGM_DATABASE), MAX_LDB, &len);
    if (ldb)
        rpgm_ldb_info((const uint8_t *)ldb, len, title_name, sizeof(title_name), &g->engine);
    free(ldb);
    Pick pick;
    pick_init(&pick, title_name);
    const size_t in = strlen(g->inner);
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    char name[NP_PATH_MAX];
    for (mz_uint i = 0; i < n; ++i)
    {
        entry_name(&zip, i, name, sizeof(name));
        if (strncmp(name, g->inner, in) != 0)
            continue;
        const char *rest = name + in, *slash = strchr(rest, '/');
        if (!slash || slash - rest != 5 || strchr(slash + 1, '/'))
            continue;
        char folder[6];
        memcpy(folder, rest, 5);
        folder[5] = '\0';
        if (rpgm_name_is(folder, "Title"))
            pick_offer(&pick, slash + 1, name);
    }
    mz_zip_reader_end(&zip);
    return pick_result(&pick, out, size);
}

bool library_title_image(Game *g, char *out, size_t size)
{
    if (size)
        out[0] = '\0';
    if (core_is_renpy(g->core))
        return renpy_title_image(g, out, size);
    return g->archive ? archive_title_image(g, out, size) : folder_title_image(g, out, size);
}

/* ---------------------------------------------------------------- last played */

void library_load_played(Library *lib, const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        char id[24], number[64], extra, *end;
        if (!strchr(line, '\n') && !(feof)(f))
        {
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) {}
            continue;
        }
        if (sscanf(line, "%23s %63s %c", id, number, &extra) != 2 || !game_id_valid(id))
            continue;
        errno = 0;
        long long when = strtoll(number, &end, 10);
        if (end == number || *end || errno || when < 0)
            continue;
        for (int i = 0; i < lib->count; ++i)
            if (!strcmp(lib->games[i].id, id))
                lib->games[i].played = when;
    }
    fclose(f);
}

bool library_mark_played(Library *lib, int index, const char *file)
{
    if (index < 0 || index >= lib->count)
        return false;
    FILE *old = fopen(file, "r");
    if (!old && errno != ENOENT)
        return false;
    StorageFile write;
    if (!storage_begin(&write, file))
    {
        if (old)
            fclose(old);
        return false;
    }
    const Game *g = &lib->games[index];
    const long long when = (long long)time(NULL);
    fprintf(write.file, "%s\t%lld\n", g->id, when);
    char line[128];
    bool ok = true;
    while (old && fgets(line, sizeof(line), old))
    {
        if (!strchr(line, '\n') && !(feof)(old))
        {
            ok = false; /* do not replace unreadable/oversized history with a partial copy */
            break;
        }
        char id[24];
        if (sscanf(line, "%23s", id) != 1 || strcmp(id, g->id))
        {
            fputs(line, write.file);
            if (!strchr(line, '\n'))
                fputc('\n', write.file);
        }
    }
    if (old)
    {
        if ((ferror)(old))
            ok = false;
        if (fclose(old) != 0)
            ok = false;
    }
    if (!ok)
    {
        storage_abort(&write);
        return false;
    }
    if (!storage_commit(&write))
        return false;
    lib->games[index].played = when;
    return true;
}

int library_restore_selection(const Library *lib, Settings *settings)
{
    for (int i = 0; i < lib->count; ++i)
        if (!strcmp(lib->games[i].id, settings->selected_game_id))
            return settings->last_game = i;
    int index = settings->last_game < 0 ? 0 : settings->last_game;
    if (index >= lib->count)
        index = lib->count ? lib->count - 1 : 0;
    settings->last_game = index;
    /* Absent-drive selection is retained. Migrate only an actual legacy index. */
    if (!settings->selected_game_id[0] && lib->count)
        str_copy(settings->selected_game_id, sizeof(settings->selected_game_id), lib->games[index].id);
    return index;
}

bool library_select(const Library *lib, Settings *settings, int index)
{
    if (index < 0 || index >= lib->count || !game_id_valid(lib->games[index].id))
        return false;
    settings->last_game = index;
    str_copy(settings->selected_game_id, sizeof(settings->selected_game_id), lib->games[index].id);
    return true;
}
