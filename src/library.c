/*
 * Dulcamara - the game library (library.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "library.h"

#include "core/cores.h"
#include "rpgmaker.h"

#include "../third_party/miniz/miniz.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static bool find_in_dir(const char *dir, const char *want, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));)
        if (rpgm_name_is(e->d_name, want))
        {
            path_join(out, size, dir, e->d_name);
            found = true;
        }
    closedir(d);
    return found;
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

static bool library_read_renpy(const char *dir, Game *g); /* below, with the rest of Ren'Py's */

bool library_read_folder(const char *dir, Game *g)
{
    if (!library_dir_is_game(dir))
        return library_read_renpy(dir, g);
    memset(g, 0, sizeof(*g));
    str_copy(g->location, sizeof(g->location), dir);
    str_copy(g->path, sizeof(g->path), dir);
    str_copy(g->name, sizeof(g->name), base_name(dir));
    hash_id(dir, g->id, sizeof(g->id));
    g->core = CORE_EASYRPG;
    char ini_path[NP_PATH_MAX];
    size_t len = 0;
    char *ini = find_in_dir(dir, RPGM_INI, ini_path, sizeof(ini_path)) ? read_file(ini_path, MAX_INI, &len) : NULL;
    title_from_ini(ini, len, g);
    free(ini);
    return true;
}

/* ---------------------------------------------------------------- Ren'Py */

/* A folder with game/ holding Ren'Py's scripts (.rpy, .rpyc) or archives (.rpa): game/ into game_dir. */
static bool renpy_game_dir(const char *dir, char *game_dir, size_t size)
{
    if (!find_in_dir(dir, "game", game_dir, size) || !path_is_dir(game_dir))
        return false;
    DIR *d = opendir(game_dir);
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));)
    {
        const char *ext = path_ext(e->d_name);
        found = !str_icmp(ext, "rpy") || !str_icmp(ext, "rpyc") || !str_icmp(ext, "rpa");
    }
    closedir(d);
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
        str_copy(out, size, name);
    return name[0] != '\0';
}

static bool library_read_renpy(const char *dir, Game *g)
{
    char game_dir[NP_PATH_MAX];
    if (!renpy_game_dir(dir, game_dir, sizeof(game_dir)))
        return false;
    memset(g, 0, sizeof(*g));
    str_copy(g->location, sizeof(g->location), dir);
    str_copy(g->path, sizeof(g->path), dir);
    str_copy(g->name, sizeof(g->name), base_name(dir));
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

bool library_read_archive(const char *archive, Game *g)
{
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
        mz_zip_reader_end(&zip);
        return false;
    }
    memset(g, 0, sizeof(*g));
    g->archive = true;
    g->core = CORE_EASYRPG;
    str_copy(g->location, sizeof(g->location), archive);
    str_copy(g->inner, sizeof(g->inner), best);
    hash_id(archive, g->id, sizeof(g->id));
    str_copy(g->name, sizeof(g->name), base_name(archive));
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

/* A game in this entry of games/ (or of a folder one level down): added when there is one. */
static bool try_entry(Library *lib, const char *path, const char *name, bool *is_dir)
{
    Game g;
    *is_dir = path_is_dir(path);
    if (*is_dir)
        return library_read_folder(path, &g) && add(lib, &g);
    return is_archive_name(name) && library_read_archive(path, &g) && add(lib, &g);
}

void library_scan(Library *lib, const char *root)
{
    lib->count = 0;
    DIR *d = opendir(root);
    if (!d)
        return;
    for (struct dirent *e; (e = readdir(d));)
    {
        if (skipped_name(e->d_name))
            continue;
        char path[NP_PATH_MAX];
        path_join(path, sizeof(path), root, e->d_name);
        bool is_dir;
        if (try_entry(lib, path, e->d_name, &is_dir) || !is_dir)
            continue;
        /* not a game: a folder of games, one level down (games/Series/Game) */
        DIR *sub = opendir(path);
        if (!sub)
            continue;
        for (struct dirent *s; (s = readdir(sub));)
        {
            if (skipped_name(s->d_name))
                continue;
            char inner[NP_PATH_MAX];
            path_join(inner, sizeof(inner), path, s->d_name);
            bool inner_dir;
            try_entry(lib, inner, s->d_name, &inner_dir);
        }
        closedir(sub);
    }
    closedir(d);
}

void library_free(Library *lib)
{
    free(lib->games);
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
        char id[24];
        long long when;
        if (sscanf(line, "%23s %lld", id, &when) != 2)
            continue;
        for (int i = 0; i < lib->count; ++i)
            if (!strcmp(lib->games[i].id, id))
                lib->games[i].played = when;
    }
    fclose(f);
}

void library_mark_played(Library *lib, int index, const char *file)
{
    if (index < 0 || index >= lib->count)
        return;
    lib->games[index].played = (long long)time(NULL);
    /* the file keeps games that are not in the library now (a drive that is out): rewritten with this one changed */
    char *old = NULL;
    size_t len = 0;
    old = read_file(file, 1 << 20, &len);
    char temp[NP_PATH_MAX];
    snprintf(temp, sizeof(temp), "%s.new", file);
    FILE *f = fopen(temp, "w");
    if (!f)
    {
        free(old);
        return;
    }
    const Game *g = &lib->games[index];
    fprintf(f, "%s\t%lld\n", g->id, g->played);
    for (char *line = old; line && *line;)
    {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        if (strncmp(line, g->id, strlen(g->id)) != 0 && line[0])
            fprintf(f, "%s\n", line);
        line = end ? end + 1 : NULL;
    }
    fclose(f);
    free(old);
    remove(file);
    rename(temp, file);
}
