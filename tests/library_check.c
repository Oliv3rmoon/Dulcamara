/*
 * Dulcamara - the library's game detection (src/library.c, src/rpgmaker.c), on the build machine: a games
 * folder of made-up games is written, scanned, sorted into its sections and checked, then EasyRPG's TestGame when it
 * is given.
 * tests/test_library.py builds and runs it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   library-check <empty temporary folder> [TestGame folder]
 */
#include "../src/core/cores.h"
#include "../src/library.h"
#include "../src/rpgmaker.h"

#include "../third_party/miniz/miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
    do                                                                                                             \
    {                                                                                                              \
        ++checks;                                                                                                  \
        if (!(cond))                                                                                               \
        {                                                                                                          \
            ++failures;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

static char base[NP_PATH_MAX], games[NP_PATH_MAX];

static void write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        printf("FAIL: cannot write %s\n", path);
        exit(1);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static void write_text(const char *dir, const char *name, const char *text)
{
    char path[NP_PATH_MAX];
    path_join(path, sizeof(path), dir, name);
    write_file(path, text, strlen(text));
}

static void make_dir(char *out, size_t size, const char *parent, const char *name)
{
    path_join(out, size, parent, name);
    make_dirs(out);
}

/* A compressed integer as liblcf writes it */
static size_t put_int(uint8_t *p, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    do
    {
        tmp[n++] = v & 0x7F;
        v >>= 7;
    } while (v);
    for (int i = 0; i < n; ++i)
        p[i] = tmp[n - 1 - i] | (i < n - 1 ? 0x80 : 0);
    return (size_t)n;
}

/* A database with the chunks a real one starts with, a System chunk naming the title picture and, for RPG Maker
 * 2003, a battle-commands chunk. */
static size_t make_ldb(uint8_t *out, const char *title_name, bool rpg2003)
{
    size_t n = 0;
    n += put_int(out + n, 11);
    memcpy(out + n, "LcfDataBase", 11);
    n += 11;
    /* actors (0x0B): a few bytes of content that the reader skips */
    n += put_int(out + n, 0x0B);
    n += put_int(out + n, 3);
    out[n++] = 1, out[n++] = 0, out[n++] = 0;
    /* system (0x16): ldb_id, then title_name (0x11), then the end */
    uint8_t sys[256];
    size_t s = 0;
    s += put_int(sys + s, 0x0A);
    s += put_int(sys + s, 1);
    sys[s++] = rpg2003 ? 1 : 0;
    s += put_int(sys + s, 0x11);
    s += put_int(sys + s, (uint32_t)strlen(title_name));
    memcpy(sys + s, title_name, strlen(title_name));
    s += strlen(title_name);
    s += put_int(sys + s, 0);
    n += put_int(out + n, 0x16);
    n += put_int(out + n, (uint32_t)s);
    memcpy(out + n, sys, s);
    n += s;
    if (rpg2003)
    {
        n += put_int(out + n, 0x1D);
        n += put_int(out + n, 2);
        out[n++] = 0, out[n++] = 0;
    }
    n += put_int(out + n, 0);
    return n;
}

/* An XYZ picture: w x h, colour i of the palette is (i, 255 - i, 7), pixel (x, y) is colour x + y */
static size_t make_xyz(uint8_t *out, size_t cap, int w, int h)
{
    size_t raw_len = 768 + (size_t)w * h;
    uint8_t *raw = malloc(raw_len);
    for (int i = 0; i < 256; ++i)
        raw[i * 3] = (uint8_t)i, raw[i * 3 + 1] = (uint8_t)(255 - i), raw[i * 3 + 2] = 7;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            raw[768 + y * w + x] = (uint8_t)(x + y);
    memcpy(out, "XYZ1", 4);
    out[4] = (uint8_t)w, out[5] = (uint8_t)(w >> 8), out[6] = (uint8_t)h, out[7] = (uint8_t)(h >> 8);
    mz_ulong packed = (mz_ulong)(cap - 8);
    int rc = mz_compress(out + 8, &packed, raw, (mz_ulong)raw_len);
    free(raw);
    return rc == MZ_OK ? 8 + (size_t)packed : 0;
}

/* Whether s is whole UTF-8 characters (no character cut short). */
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0)
            return false;
        for (++p; n > 0; --n, ++p)
            if ((*p & 0xC0) != 0x80)
                return false;
    }
    return true;
}

static const Game *find(const Library *lib, const char *name)
{
    for (int i = 0; i < lib->count; ++i)
        if (!strcmp(lib->games[i].name, name))
            return &lib->games[i];
    return NULL;
}

static void make_games(void)
{
    char dir[NP_PATH_MAX], sub[NP_PATH_MAX], title[NP_PATH_MAX];
    uint8_t ldb[1024];

    /* 1. a game with its title in RPG_RT.ini, and two title pictures: the database names the second */
    make_dir(dir, sizeof(dir), games, "Alpha");
    write_file((path_join(sub, sizeof(sub), dir, "RPG_RT.ldb"), sub), ldb, make_ldb(ldb, "Sunrise", true));
    write_text(dir, "RPG_RT.lmt", "LcfMapTree");
    write_text(dir, "RPG_RT.ini", "[RPG_RT]\r\nGameTitle=Alpha Quest\r\nMapEditMode=2\r\n");
    make_dir(title, sizeof(title), dir, "Title");
    write_text(title, "Dawn.png", "not read");
    write_text(title, "sunrise.PNG", "not read");

    /* 2. names in another case, no RPG_RT.ini: the folder's name is the title */
    make_dir(dir, sizeof(dir), games, "beta");
    write_file((path_join(sub, sizeof(sub), dir, "rpg_rt.LDB"), sub), ldb, make_ldb(ldb, "", false));
    write_text(dir, "Rpg_Rt.LMT", "LcfMapTree");

    /* 3. only a database: not a game */
    make_dir(dir, sizeof(dir), games, "Half");
    write_text(dir, "RPG_RT.ldb", "LcfDataBase");

    /* 4. a folder of games: one level down */
    make_dir(dir, sizeof(dir), games, "Series");
    make_dir(sub, sizeof(sub), dir, "Gamma");
    write_text(sub, "RPG_RT.ldb", "x");
    write_text(sub, "RPG_RT.lmt", "x");
    write_text(sub, "RPG_RT.ini", "[RPG_RT]\nGameTitle=Caf\xe9 Myst\xe8re\n"); /* windows-1252 */
    make_dir(sub, sizeof(sub), dir, "readme");

    /* 5. a Japanese title in Shift-JIS: 夢の旅 */
    make_dir(dir, sizeof(dir), games, "Yume");
    write_text(dir, "RPG_RT.ldb", "x");
    write_text(dir, "RPG_RT.lmt", "x");
    write_text(dir, "RPG_RT.ini", "[RPG_RT]\nGameTitle=\x96\xb2\x82\xcc\x97\xb7\n");

    /* 6. Cyrillic, the code page named by EasyRPG's own key: Сказка */
    make_dir(dir, sizeof(dir), games, "Skazka");
    write_text(dir, "RPG_RT.ldb", "x");
    write_text(dir, "RPG_RT.lmt", "x");
    write_text(dir, "RPG_RT.ini", "[EasyRPG]\nEncoding=1251\n\n[RPG_RT]\nGameTitle = \xd1\xea\xe0\xe7\xea\xe0 \n");

    /* 7. a .zip with the game in a folder, its title screen an XYZ picture the database names */
    {
        char path[NP_PATH_MAX];
        path_join(path, sizeof(path), games, "Delta.zip");
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        mz_zip_writer_init_file(&zip, path, 0);
        size_t n = make_ldb(ldb, "Logo", false);
        mz_zip_writer_add_mem(&zip, "Delta/", NULL, 0, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_add_mem(&zip, "Delta/RPG_RT.ldb", ldb, n, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_add_mem(&zip, "Delta/RPG_RT.lmt", "x", 1, MZ_DEFAULT_COMPRESSION);
        const char *ini = "[RPG_RT]\nGameTitle=Delta Zip\n";
        mz_zip_writer_add_mem(&zip, "Delta/RPG_RT.ini", ini, strlen(ini), MZ_DEFAULT_COMPRESSION);
        uint8_t xyz[8192];
        size_t xn = make_xyz(xyz, sizeof(xyz), 16, 8);
        mz_zip_writer_add_mem(&zip, "Delta/Title/Logo.xyz", xyz, xn, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_add_mem(&zip, "Delta/Title/Another.png", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_add_mem(&zip, "Readme.txt", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
    }

    /* 8. an .easyrpg with the game at its root and no ini */
    {
        char path[NP_PATH_MAX];
        path_join(path, sizeof(path), games, "Epsilon.easyrpg");
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        mz_zip_writer_init_file(&zip, path, 0);
        mz_zip_writer_add_mem(&zip, "rpg_rt.ldb", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_add_mem(&zip, "RPG_RT.LMT", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
    }

    /* 9. not games: a zip without a database, a file that is not a zip, a game's save folder, a hidden folder */
    {
        char path[NP_PATH_MAX];
        path_join(path, sizeof(path), games, "Photos.zip");
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        mz_zip_writer_init_file(&zip, path, 0);
        mz_zip_writer_add_mem(&zip, "photo.png", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
        write_text(games, "Broken.zip", "this is not a zip archive");
        make_dir(dir, sizeof(dir), games, "Delta.zip.save");
        write_text(dir, "Save01.lsd", "x");
        make_dir(dir, sizeof(dir), games, ".hidden");
        write_text(dir, "RPG_RT.ldb", "x");
        write_text(dir, "RPG_RT.lmt", "x");
    }
}

static void check_made_up_games(void)
{
    Library lib = {0};
    library_scan(&lib, games);
    library_sort(&lib, 0);
    printf("found %d games:", lib.count);
    for (int i = 0; i < lib.count; ++i)
        printf(" [%s]", lib.games[i].title);
    printf("\n");
    CHECK(lib.count == 7, "7 games, found %d", lib.count);

    const Game *a = find(&lib, "Alpha");
    CHECK(a && !strcmp(a->title, "Alpha Quest"), "Alpha's title from RPG_RT.ini: %s", a ? a->title : "-");
    CHECK(a && !a->archive && !strcmp(a->path, a->location), "Alpha is a folder loaded as it is");
    CHECK(a && strlen(a->id) == 16, "Alpha has a 16-digit id");
    const Game *b = find(&lib, "beta");
    CHECK(b && !strcmp(b->title, "beta"), "beta, no ini, is called by its folder: %s", b ? b->title : "-");
    CHECK(!find(&lib, "Half"), "a folder with only RPG_RT.ldb is not a game");
    const Game *g = find(&lib, "Gamma");
    CHECK(g && strstr(g->location, "/Series/Gamma"), "a game one folder down is found");
    CHECK(g && !strcmp(g->title, "Caf\xc3\xa9 Myst\xc3\xa8re"), "a windows-1252 title becomes UTF-8: %s", g ? g->title : "-");
    CHECK(!find(&lib, "Series") && !find(&lib, "readme"), "the folder of games and its other folders are not games");
    const Game *y = find(&lib, "Yume");
    CHECK(y && !strcmp(y->title, "\xe5\xa4\xa2\xe3\x81\xae\xe6\x97\x85"), "a Shift-JIS title becomes UTF-8: %s",
          y ? y->title : "-");
    const Game *k = find(&lib, "Skazka");
    CHECK(k && !strcmp(k->title, "\xd0\xa1\xd0\xba\xd0\xb0\xd0\xb7\xd0\xba\xd0\xb0"),
          "[EasyRPG] Encoding=1251 is followed, the value trimmed: %s", k ? k->title : "-");

    const Game *d = find(&lib, "Delta");
    CHECK(d && d->archive, "Delta.zip is an archive game");
    CHECK(d && !strcmp(d->inner, "Delta/"), "Delta's game is in its Delta/ folder: %s", d ? d->inner : "-");
    char want[NP_PATH_MAX];
    snprintf(want, sizeof(want), "%s/Delta.zip/Delta", games);
    CHECK(d && !strcmp(d->path, want), "the core loads <archive>/<folder>: %s", d ? d->path : "-");
    CHECK(d && !strcmp(d->title, "Delta Zip"), "Delta's title from the ini inside it: %s", d ? d->title : "-");
    const Game *e = find(&lib, "Epsilon");
    snprintf(want, sizeof(want), "%s/Epsilon.easyrpg", games);
    CHECK(e && e->archive && !e->inner[0] && !strcmp(e->path, want), "an .easyrpg with the game at its root: %s",
          e ? e->path : "-");
    CHECK(e && !strcmp(e->title, "Epsilon"), "an archive without an ini is called by its name");
    CHECK(!find(&lib, "Photos") && !find(&lib, "Broken") && !find(&lib, "Delta.zip.save") && !find(&lib, ".hidden"),
          "archives without a game, broken ones, save folders and hidden folders are skipped");

    /* the order: by title, then the last played first */
    for (int i = 1; i < lib.count; ++i)
        CHECK(str_icmp(lib.games[i - 1].title, lib.games[i].title) <= 0, "sorted by title at %d", i);
    char played[NP_PATH_MAX];
    path_join(played, sizeof(played), base, "played.txt");
    int bi = (int)(b - lib.games);
    library_mark_played(&lib, bi, played);
    library_sort(&lib, 1);
    CHECK(!strcmp(lib.games[0].name, "beta"), "the last played comes first: %s", lib.games[0].name);
    Library again = {0};
    library_scan(&again, games);
    library_load_played(&again, played);
    library_sort(&again, 1);
    CHECK(again.count && !strcmp(again.games[0].name, "beta") && again.games[0].played > 0,
          "when it was played is kept in played.txt");
    library_free(&again);

    /* covers: the picture the database names, in either case, and the engine */
    library_sort(&lib, 0);
    Game *ag = (Game *)find(&lib, "Alpha");
    char cover[NP_PATH_MAX];
    CHECK(ag && library_title_image(ag, cover, sizeof(cover)) && strstr(cover, "/Title/sunrise.PNG"),
          "Alpha's cover is the title picture its database names: %s", cover);
    CHECK(ag && ag->engine == 2003, "Alpha's database is RPG Maker 2003's: %d", ag ? ag->engine : -1);
    Game *bg = (Game *)find(&lib, "beta");
    CHECK(bg && !library_title_image(bg, cover, sizeof(cover)), "beta has no Title folder, so no cover");
    CHECK(bg && bg->engine == 2000, "beta's database is RPG Maker 2000's: %d", bg ? bg->engine : -1);
    Game *dg = (Game *)find(&lib, "Delta");
    CHECK(dg && library_title_image(dg, cover, sizeof(cover)) && !strcmp(cover, "Delta/Title/Logo.xyz"),
          "Delta's cover is the entry Delta/Title/Logo.xyz: %s", cover);
    library_free(&lib);
}

static void check_pieces(void)
{
    /* the database reader, on its own */
    uint8_t ldb[512];
    char name[64];
    int engine = 0;
    size_t n = make_ldb(ldb, "Title2", false);
    CHECK(rpgm_ldb_info(ldb, n, name, sizeof(name), &engine) && !strcmp(name, "Title2") && engine == 2000,
          "rpgm_ldb_info reads title_name and RPG Maker 2000: %s %d", name, engine);
    CHECK(!rpgm_ldb_info((const uint8_t *)"LcfMapTree", 10, name, sizeof(name), &engine),
          "a map tree is not a database");
    CHECK(!rpgm_ldb_info(ldb, 9, name, sizeof(name), &engine), "a cut database is refused");

    /* XYZ pictures */
    uint8_t xyz[8192];
    size_t xn = make_xyz(xyz, sizeof(xyz), 20, 10);
    int w = 0, h = 0;
    uint8_t *rgba = rpgm_decode_xyz(xyz, xn, &w, &h);
    CHECK(rgba && w == 20 && h == 10, "an XYZ picture decodes to 20x10: %dx%d", w, h);
    if (rgba)
    {
        const uint8_t *px = rgba + (3 * 20 + 5) * 4; /* (5, 3): colour 8 */
        CHECK(px[0] == 8 && px[1] == 247 && px[2] == 7 && px[3] == 255, "XYZ pixel (5, 3) is palette colour 8");
        free(rgba);
    }
    CHECK(!rpgm_decode_xyz((const uint8_t *)"XYZ1\x10\x00", 6, &w, &h), "a cut XYZ picture is refused");

    /* the encoding guess */
    CHECK(rpgm_guess_codepage("Plain", 5) == 65001, "ASCII is UTF-8-compatible");
    CHECK(rpgm_guess_codepage("\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd", 10) == 932, "kana in Shift-JIS");
    CHECK(rpgm_guess_codepage("\xcf\xf0\xe8\xe2\xe5\xf2 \xec\xe8\xf0", 10) == 1251, "Russian in windows-1251");
    CHECK(rpgm_guess_codepage("Caf\xe9", 4) == 1252, "an accent in windows-1252");
    CHECK(rpgm_guess_codepage("\xd1\xea\xe0\xe7\xea\xe0", 6) == 1251, "a short Russian word in windows-1251");
    CHECK(rpgm_guess_codepage("\xbe\xc8\xb3\xe7\xc7\xcf\xbc\xbc\xbf\xe4", 10) == 949, "Hangul in CP949");
    char out[16];
    rpgm_to_utf8("\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", 15, 65001, out, sizeof(out));
    CHECK(strlen(out) == 15, "UTF-8 is kept whole when it fits: %zu", strlen(out));
    rpgm_to_utf8("\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", 18, 65001, out, sizeof(out));
    CHECK(strlen(out) == 15, "and cut between characters when it doesn't: %zu", strlen(out));
}

/* Ren'Py games: a folder with game/ holding scripts or archives; its version from renpy/, its title from its
 * launcher, its cover from game/gui. In a games folder of their own. */
static void check_renpy_games(void)
{
    char root[NP_PATH_MAX], dir[NP_PATH_MAX], sub[NP_PATH_MAX], more[NP_PATH_MAX];
    path_join(root, sizeof(root), base, "renpy-games");
    make_dirs(root);

    make_dir(dir, sizeof(dir), root, "Moonlit Tale");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "script.rpyc", "compiled");
    make_dir(more, sizeof(more), sub, "gui");
    write_text(more, "main_menu.png", "png");
    write_text(more, "window_icon.png", "png");
    make_dir(sub, sizeof(sub), dir, "renpy");
    write_text(sub, "vc_version.py", "branch = 'fix'\nversion = '8.3.7.24110503'\nofficial = True\n");
    write_text(dir, "MoonlitTale.exe", "MZ");
    write_text(dir, "MoonlitTale-32.exe", "MZ");
    write_text(dir, "MoonlitTale.sh", "#!/bin/sh\n");

    make_dir(dir, sizeof(dir), root, "old-story-1.0-pc");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "archive.RPA", "RPA-3.0");
    make_dir(more, sizeof(more), sub, "gui");
    write_text(more, "window_icon.png", "png");
    make_dir(sub, sizeof(sub), dir, "renpy");
    write_text(sub, "vc_version.py", "vc_version = 2266\nversion = u'7.4.11.2266'\n");
    write_text(dir, "OldStory.sh", "#!/bin/sh\n");
    write_text(dir, "OldStory.py", "import os\n");
    write_text(dir, "Installer.exe", "MZ");

    make_dir(dir, sizeof(dir), root, "Older");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "script.rpy", "label start:\n");
    make_dir(sub, sizeof(sub), dir, "renpy");
    write_text(sub, "__init__.py", "import os\nversion_tuple = (7, 3, 5, vc_version)\n");

    make_dir(dir, sizeof(dir), root, "Repack");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "script.rpy", "label start:\n");

    make_dir(dir, sizeof(dir), root, "Repack 7");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "script.rpyc", "compiled");
    make_dir(sub, sizeof(sub), dir, "lib");
    make_dir(more, sizeof(more), sub, "py2-linux-x86_64");

    make_dir(dir, sizeof(dir), root, "Classic");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "script.rpy", "label start:\n");
    make_dir(sub, sizeof(sub), dir, "renpy");
    write_text(sub, "__init__.py", "version_tuple = (6, 99, 14, vc_version)\n");

    make_dir(dir, sizeof(dir), root, "Notes");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "readme.txt", "not a game");

    Library lib = {0};
    library_scan(&lib, root);
    CHECK(lib.count == 6, "6 Ren'Py games, found %d", lib.count);
    char cover[NP_PATH_MAX];
    Game *m = (Game *)find(&lib, "Moonlit Tale");
    CHECK(m && m->core == CORE_RENPY && m->engine == 8, "Moonlit Tale is Ren'Py 8: core %d, version %d",
          m ? m->core : -1, m ? m->engine : -1);
    CHECK(m && !strcmp(m->title, "MoonlitTale"), "its title is its launcher's name: %s", m ? m->title : "-");
    CHECK(m && !strcmp(m->path, m->location), "the core loads its folder");
    CHECK(m && library_title_image(m, cover, sizeof(cover)) && strstr(cover, "/game/gui/main_menu.png"),
          "its cover is game/gui/main_menu.png: %s", cover);
    Game *o = (Game *)find(&lib, "old-story-1.0-pc");
    CHECK(o && o->core == CORE_RENPY7 && o->engine == 7, "old-story is Ren'Py 7, on its core: core %d, version %d",
          o ? o->core : -1, o ? o->engine : -1);
    CHECK(o && !strcmp(o->title, "OldStory"), "its title, from the launcher with its .py (not Installer.exe): %s",
          o ? o->title : "-");
    CHECK(o && library_title_image(o, cover, sizeof(cover)) && strstr(cover, "/game/gui/window_icon.png"),
          "without main_menu.png its cover is window_icon.png: %s", cover);
    Game *old = (Game *)find(&lib, "Older");
    CHECK(old && old->engine == 7 && old->core == CORE_RENPY7, "Older's version from renpy/__init__.py: %d",
          old ? old->engine : -1);
    Game *classic = (Game *)find(&lib, "Classic");
    CHECK(classic && classic->engine == 6 && classic->core == CORE_RENPY7, "a Ren'Py 6 game runs on Ren'Py 7: %d %d",
          classic ? classic->engine : -1, classic ? classic->core : -1);
    Game *r7 = (Game *)find(&lib, "Repack 7");
    CHECK(r7 && r7->engine == 7 && r7->core == CORE_RENPY7, "a repack with lib/py2-* is Ren'Py 7: %d %d",
          r7 ? r7->engine : -1, r7 ? r7->core : -1);
    Game *r = (Game *)find(&lib, "Repack");
    CHECK(r && r->core == CORE_RENPY && r->engine == 0, "a repack without renpy/ is tried as Ren'Py 8: %d",
          r ? r->engine : -1);
    CHECK(r && !strcmp(r->title, "Repack"), "and is called by its folder: %s", r ? r->title : "-");
    CHECK(r && !library_title_image(r, cover, sizeof(cover)), "and has no cover");
    CHECK(!find(&lib, "Notes"), "a game/ folder without scripts is not a game");
    library_free(&lib);
}

/* The titles of a library, in its order, one space between. */
static void titles(const Library *lib, char *out, size_t size)
{
    out[0] = '\0';
    for (int i = 0; i < lib->count; ++i)
        snprintf(out + strlen(out), size - strlen(out), "%s%s", i ? " " : "", lib->games[i].title);
}

/* The sections: in a games folder of both kinds, sorted, RPG Maker's games come first and Ren'Py's (8's and 7's
 * together) after, each section in its own order, by title or the last played first. One kind alone is one section. */
static void check_sections(void)
{
    char root[NP_PATH_MAX], dir[NP_PATH_MAX], sub[NP_PATH_MAX], order[256];
    path_join(root, sizeof(root), base, "mixed-games");
    make_dirs(root);
    /* by title alone they would mix: Alfa (Ren'Py), Bravo (RPG Maker), Mike (Ren'Py 7), Yankee (RPG Maker), Zulu */
    static const char *const RPG_MAKER[] = {"Yankee", "Bravo"}, *const RENPY[] = {"Zulu", "Mike", "Alfa"};
    for (size_t i = 0; i < sizeof(RPG_MAKER) / sizeof(RPG_MAKER[0]); ++i)
    {
        make_dir(dir, sizeof(dir), root, RPG_MAKER[i]);
        write_text(dir, "RPG_RT.ldb", "x");
        write_text(dir, "RPG_RT.lmt", "x");
    }
    for (size_t i = 0; i < sizeof(RENPY) / sizeof(RENPY[0]); ++i)
    {
        make_dir(dir, sizeof(dir), root, RENPY[i]);
        make_dir(sub, sizeof(sub), dir, "game");
        write_text(sub, "script.rpy", "label start:\n");
    }
    path_join(dir, sizeof(dir), root, "Mike");
    make_dir(sub, sizeof(sub), dir, "renpy");
    write_text(sub, "__init__.py", "version_tuple = (7, 4, 11, vc_version)\n");

    Library lib = {0};
    library_scan(&lib, root);
    library_sort(&lib, 0);
    titles(&lib, order, sizeof(order));
    CHECK(!strcmp(order, "Bravo Yankee Alfa Mike Zulu"), "by title, RPG Maker's section first: %s", order);
    int count = -1, first = library_section_start(&lib, SECTION_RPG_MAKER, &count);
    CHECK(first == 0 && count == 2, "RPG Maker's section: %d games from %d", count, first);
    first = library_section_start(&lib, SECTION_RENPY, &count);
    CHECK(first == 2 && count == 3, "Ren'Py's section: %d games from %d", count, first);
    const Game *m = find(&lib, "Mike");
    CHECK(m && m->core == CORE_RENPY7 && library_section(m) == SECTION_RENPY,
          "a Ren'Py 7 game is in Ren'Py's section, on Ren'Py 7's core: core %d", m ? m->core : -1);

    /* Zulu, played last of them all, still comes after RPG Maker's games */
    for (int i = 0; i < lib.count; ++i)
    {
        const char *name = lib.games[i].name;
        lib.games[i].played = !strcmp(name, "Zulu")     ? 300
                              : !strcmp(name, "Yankee") ? 200
                              : !strcmp(name, "Mike")   ? 100
                                                        : 0;
    }
    library_sort(&lib, 1);
    titles(&lib, order, sizeof(order));
    CHECK(!strcmp(order, "Yankee Bravo Zulu Mike Alfa"), "the last played first, in each section: %s", order);
    library_free(&lib);

    library_scan(&lib, games);
    library_sort(&lib, 0);
    first = library_section_start(&lib, SECTION_RPG_MAKER, &count);
    CHECK(first == 0 && count == lib.count && count > 0, "RPG Maker's games alone: one section of %d", count);
    library_section_start(&lib, SECTION_RENPY, &count);
    CHECK(count == 0, "and Ren'Py's is empty: %d", count);
    library_free(&lib);
    path_join(root, sizeof(root), base, "renpy-games");
    library_scan(&lib, root);
    library_sort(&lib, 1);
    first = library_section_start(&lib, SECTION_RENPY, &count);
    CHECK(first == 0 && count == lib.count && count > 0, "Ren'Py's games alone: one section of %d", count);
    library_free(&lib);
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* The reason recorded for a folder or file the scan did not list, by the end of its path; NULL when none. */
static const char *skip_reason(const Library *lib, const char *tail)
{
    const size_t tn = strlen(tail);
    for (int i = 0; i < lib->skipped_count; ++i)
    {
        const char *p = lib->skipped[i].path;
        const size_t pn = strlen(p);
        if (pn >= tn && !strcmp(p + pn - tn, tail) && (pn == tn || p[pn - tn - 1] == '/'))
            return lib->skipped[i].reason;
    }
    return NULL;
}

static void rpg_game(const char *parent, const char *name)
{
    char dir[NP_PATH_MAX];
    make_dir(dir, sizeof(dir), parent, name);
    write_text(dir, "RPG_RT.ldb", "x");
    write_text(dir, "RPG_RT.lmt", "x");
}

/* The id the library has always given a game: FNV-1a of its location, as 16 hex digits. */
static void expected_id(const char *location, char *out, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *s = location; *s; ++s)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    snprintf(out, size, "%016llx", (unsigned long long)h);
}

/* Smarter detection: games one folder deeper than before, Ren'Py's scripts in game/'s subfolders, and a reason for
 * every folder that looks like a game but is not listed. */
static void check_detection(void)
{
    char root[NP_PATH_MAX], dir[NP_PATH_MAX], sub[NP_PATH_MAX], more[NP_PATH_MAX];
    path_join(root, sizeof(root), base, "detect-games");
    make_dirs(root);

    /* two folders down: games/Windows/Series/Game (RPG Maker) and games/Windows/Fantasy/<build>/game (Ren'Py) */
    make_dir(dir, sizeof(dir), root, "Windows");
    make_dir(sub, sizeof(sub), dir, "Series");
    rpg_game(sub, "Deep RPG");
    make_dir(sub, sizeof(sub), dir, "Fantasy Opposite");
    make_dir(more, sizeof(more), sub, "FantasyOpposite-1.0-pc");
    make_dir(more, sizeof(more), more, "game");
    write_text(more, "script.rpyc", "compiled");
    /* three folders down: not listed, and said why */
    make_dir(dir, sizeof(dir), root, "One");
    make_dir(dir, sizeof(dir), dir, "Two");
    make_dir(dir, sizeof(dir), dir, "Three");
    rpg_game(dir, "Too Deep");
    /* Ren'Py with its scripts and archives only in game/'s subfolders */
    make_dir(dir, sizeof(dir), root, "Scripts Below");
    make_dir(sub, sizeof(sub), dir, "game");
    make_dir(sub, sizeof(sub), sub, "scripts");
    write_text(sub, "script.rpyc", "compiled");
    make_dir(dir, sizeof(dir), root, "Archives Below");
    make_dir(sub, sizeof(sub), dir, "game");
    make_dir(more, sizeof(more), sub, "gui");
    write_text(more, "main_menu.png", "png");
    make_dir(sub, sizeof(sub), sub, "archives");
    write_text(sub, "images.RPA", "RPA-3.0");
    /* A partial transfer can already contain enough files to look playable, even in the previous index. */
    make_dir(dir, sizeof(dir), root, "Champion.AVA-PART");
    make_dir(sub, sizeof(sub), dir, "game");
    write_text(sub, "archives.rpa", "truncated");
    Game partial;
    CHECK(library_read_folder(dir, &partial) && !library_game_valid(&partial, root),
          "cached partial transfer rejected");
    make_dir(dir, sizeof(dir), root, "Collection.ava-part");
    rpg_game(dir, "Nested Transfer");
    path_join(sub, sizeof(sub), dir, "Nested Transfer");
    CHECK(library_read_folder(sub, &partial) && !library_game_valid(&partial, root),
          "cached game inside a partial transfer rejected");
    make_dir(dir, sizeof(dir), root, "Nested Script");
    make_dir(sub, sizeof(sub), dir, "game");
    make_dir(sub, sizeof(sub), sub, "src");
    make_dir(sub, sizeof(sub), sub, "episode");
    write_text(sub, "story_ren.py", "label start:");
    make_dir(dir, sizeof(dir), root, "Not a Script");
    make_dir(sub, sizeof(sub), dir, "game");
    make_dir(sub, sizeof(sub), sub, "empty.rpa");

    /* other engines */
    make_dir(dir, sizeof(dir), root, "Lust Doll");
    write_text(dir, "Game.exe", "MZ");
    make_dir(sub, sizeof(sub), dir, "www");
    make_dir(sub, sizeof(sub), sub, "js");
    write_text(sub, "rpg_core.js", "//");
    make_dir(dir, sizeof(dir), root, "MZ Game");
    make_dir(sub, sizeof(sub), dir, "js");
    write_text(sub, "rmmz_core.js", "//");
    make_dir(dir, sizeof(dir), root, "Ace Game");
    write_text(dir, "Game.exe", "MZ");
    write_text(dir, "Game.rgss3a", "RGSSAD");
    make_dir(dir, sizeof(dir), root, "XP Game");
    write_text(dir, "Game.ini", "[Game]\r\nLibrary=RGSS104E.dll\r\nScripts=Data\\Scripts.rxdata\r\nTitle=XP\r\n");
    make_dir(dir, sizeof(dir), root, "Unity Game");
    write_text(dir, "UnityGame.exe", "MZ");
    write_text(dir, "UnityPlayer.dll", "MZ");
    make_dir(sub, sizeof(sub), dir, "UnityGame_Data");
    make_dir(sub, sizeof(sub), sub, "Managed");
    write_text(sub, "Assembly-CSharp.dll", "MZ");
    /* Ren'Py still packed: a .zip of it, and archives Dulcamara cannot open */
    {
        char path[NP_PATH_MAX];
        path_join(path, sizeof(path), root, "Story-1.0-pc.zip");
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        mz_zip_writer_init_file(&zip, path, 0);
        mz_zip_writer_add_mem(&zip, "Story-1.0-pc/game/script.rpyc", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_add_mem(&zip, "Story-1.0-pc/Story.exe", "x", 1, MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
    }
    write_text(root, "Story.7z", "7z\xbc\xaf\x27\x1c");
    write_text(root, "Story.RAR", "Rar!\x1a\x07");
    /* not games, and nothing to say: an empty folder, a folder of games, a text file */
    make_dir(dir, sizeof(dir), root, "Empty Folder");
    make_dir(dir, sizeof(dir), root, "Collection");
    rpg_game(dir, "Listed");
    write_text(root, "readme.txt", "hello");
    /* collections that merely hold a loose www/ folder or an RPG Maker XP-VX Ace file beside real games are still
     * looked into, as they were before games two folders down were looked for */
    make_dir(dir, sizeof(dir), root, "Mixed Collection");
    make_dir(sub, sizeof(sub), dir, "www");
    rpg_game(dir, "Beside www");
    make_dir(dir, sizeof(dir), root, "Mixed RGSS");
    write_text(dir, "Leftover.rgss3a", "RGSSAD");
    rpg_game(dir, "Beside RGSS");
    /* a link out of the games folder is not followed into: games/Links/Out -> <base>/outside, with a game in it */
    char outside[NP_PATH_MAX], link[NP_PATH_MAX];
    make_dir(outside, sizeof(outside), base, "outside");
    rpg_game(outside, "Outside Game");
    make_dir(dir, sizeof(dir), root, "Links");
    path_join(link, sizeof(link), dir, "Out");
    CHECK(!symlink(outside, link), "a link to a folder outside the games folder");

    Library lib = {0};
    char error[NP_PATH_MAX + 64];
    CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)), "the detection scan: %s", error);
    printf("detection: %d games:", lib.count);
    for (int i = 0; i < lib.count; ++i)
        printf(" [%s]", lib.games[i].name);
    printf("; %d not shown:\n", lib.skipped_count);
    for (int i = 0; i < lib.skipped_count; ++i)
        printf("  %s: %s\n", lib.skipped[i].path + strlen(root) + 1, lib.skipped[i].reason);

    const Game *deep = find(&lib, "Deep RPG");
    CHECK(deep && strstr(deep->location, "/Windows/Series/Deep RPG") && deep->core == CORE_EASYRPG,
          "an RPG Maker game two folders down is found");
    CHECK(deep && library_game_valid(deep, root), "and its location is a valid one (the index keeps it)");
    const Game *fantasy = find(&lib, "FantasyOpposite-1.0-pc");
    CHECK(fantasy && core_is_renpy(fantasy->core), "a Ren'Py game two folders down is found");
    const Game *scripts = find(&lib, "Scripts Below");
    CHECK(scripts && core_is_renpy(scripts->core), "Ren'Py with its scripts only in game/scripts/ is found");
    const Game *archives = find(&lib, "Archives Below");
    CHECK(archives && core_is_renpy(archives->core), "Ren'Py with its archives only in game/archives/ is found");
    CHECK(find(&lib, "Listed") && !skip_reason(&lib, "Collection"), "a folder of games is not a rejected game");
    CHECK(!find(&lib, "Too Deep"), "a game three folders down is not listed");
    CHECK(find(&lib, "Nested Script") && !find(&lib, "Not a Script"),
          "nested _ren.py is a script and an empty .rpa directory is not");
    const char *partial_reason = skip_reason(&lib, "Champion.AVA-PART");
    CHECK(partial_reason && strstr(partial_reason, "finish copying") && !find(&lib, "Nested Transfer"),
          "partial transfers get an explanation and hide unfinished descendants");
    path_join(dir, sizeof(dir), root, "Champion.AVA-PART");
    path_join(more, sizeof(more), root, "Champion.ava-part-complete");
    CHECK(rename(dir, more) == 0, "complete the transfer");
    library_scan(&lib, root);
    CHECK(find(&lib, "Champion.ava-part-complete"), "the next scan lists the finished game");
    const char *why = skip_reason(&lib, "One/Two/Three/Too Deep");
    CHECK(why && strstr(why, "nested too deep"), "and the reason says it is nested too deep: %s", why ? why : "-");
    why = skip_reason(&lib, "Lust Doll");
    CHECK(why && strstr(why, "RPG Maker MV/MZ"), "an RPG Maker MV game (www/) is named: %s", why ? why : "-");
    why = skip_reason(&lib, "MZ Game");
    CHECK(why && strstr(why, "RPG Maker MV/MZ"), "an RPG Maker MZ game (js/rmmz_core.js) is named: %s",
          why ? why : "-");
    why = skip_reason(&lib, "Ace Game");
    CHECK(why && strstr(why, "RPG Maker XP/VX/VX Ace"), "an RPG Maker VX Ace game (Game.rgss3a) is named: %s",
          why ? why : "-");
    why = skip_reason(&lib, "XP Game");
    CHECK(why && strstr(why, "RPG Maker XP/VX/VX Ace"), "an RPG Maker XP game (RGSS Game.ini) is named: %s",
          why ? why : "-");
    why = skip_reason(&lib, "Unity Game");
    CHECK(why && strstr(why, "no supported game found here"), "another engine gets the generic reason: %s",
          why ? why : "-");
    CHECK(!skip_reason(&lib, "Unity Game/UnityGame_Data"), "and its own folders are not reported again");
    why = skip_reason(&lib, "Story-1.0-pc.zip");
    CHECK(why && strstr(why, "Ren'Py") && strstr(why, "extract"), "a Ren'Py game in a .zip: %s", why ? why : "-");
    why = skip_reason(&lib, "Story.7z");
    CHECK(why && strstr(why, ".7z") && strstr(why, "extract"), "a .7z: %s", why ? why : "-");
    why = skip_reason(&lib, "Story.RAR");
    CHECK(why && strstr(why, ".rar") && strstr(why, "extract"), "a .rar: %s", why ? why : "-");
    CHECK(!skip_reason(&lib, "Empty Folder") && !skip_reason(&lib, "readme.txt") && !skip_reason(&lib, "Links"),
          "an empty folder, a text file and a folder of links say nothing");
    CHECK(!find(&lib, "Outside Game") && !skip_reason(&lib, "Out/Outside Game"),
          "a link out of the games folder is not followed");
    why = skip_reason(&lib, "Links/Out");
    CHECK(why && strstr(why, "link") && strstr(why, "outside the games folder") && strstr(why, "not followed"),
          "and it says so: %s", why ? why : "-");
    CHECK(find(&lib, "Beside www") && !skip_reason(&lib, "Mixed Collection"),
          "a game beside a loose www/ folder is listed, and its collection is not called an MV game");
    CHECK(find(&lib, "Beside RGSS") && !skip_reason(&lib, "Mixed RGSS"),
          "a game beside a loose .rgss3a is listed, and its collection is not called an XP-VX Ace game");
    CHECK(!skip_reason(&lib, "Lust Doll/www") && !skip_reason(&lib, "Lust Doll/www/js"),
          "an MV game's own folders are not reported again");
    library_free(&lib);
    CHECK(!lib.skipped && !lib.skipped_count, "library_free frees the reasons too");

    /* a location three folders down is not one the index accepts */
    Game g = {0};
    path_join(dir, sizeof(dir), root, "One/Two/Three/Too Deep");
    CHECK(library_read_folder(dir, &g) && !library_game_valid(&g, root), "a game three folders down is invalid");

    /* the old games folder: the same games with the ids they always had, and the reasons for what it skips */
    library_scan(&lib, games);
    CHECK(lib.count == 7, "the made-up games are still 7: %d", lib.count);
    for (int i = 0; i < lib.count; ++i)
    {
        char id[24];
        expected_id(lib.games[i].location, id, sizeof(id));
        CHECK(!strcmp(id, lib.games[i].id), "%s keeps its id %s", lib.games[i].name, id);
    }
    why = skip_reason(&lib, "Half");
    CHECK(why && strstr(why, "no supported game found here"), "a folder with only RPG_RT.ldb: %s", why ? why : "-");
    CHECK(!skip_reason(&lib, "Series") && !skip_reason(&lib, "Series/readme") && !skip_reason(&lib, "Photos.zip") &&
              !skip_reason(&lib, "Broken.zip") && !skip_reason(&lib, "Delta.zip.save"),
          "folders of games, empty folders, archives of photos, broken and save folders say nothing");
    library_free(&lib);
}

/* The reason recorded for a skipped path that starts with `head` (an over-long path is recorded cut short). */
static const char *skip_reason_from(const Library *lib, const char *head)
{
    for (int i = 0; i < lib->skipped_count; ++i)
        if (!strncmp(lib->skipped[i].path, head, strlen(head)))
            return lib->skipped[i].reason;
    return NULL;
}

/* Whether every game of `names` (NULL-ended) is listed. */
static bool all_listed(const Library *lib, const char *const *names)
{
    for (; *names; ++names)
        if (!find(lib, *names))
        {
            printf("  missing: %s\n", *names);
            return false;
        }
    return true;
}

/* `n` copies of a UTF-8 character */
static void repeat(char *out, size_t size, const char *ch, int n)
{
    out[0] = '\0';
    for (int i = 0; i < n; ++i)
        strncat(out, ch, size - strlen(out) - 1);
}

/* The games already listed (the games folder and the folders in it, depths 0 and 1) are never dropped by a partial
 * scan: a failure there fails the scan, which keeps the library it had and names the entry. A failure two folders
 * down (depth 2, where the scan did not look before), or a path too long for the app at any depth, is reported as
 * not shown with its reason, and every other game is listed. */
static void check_failed_scan(void)
{
    char root[NP_PATH_MAX], dir[NP_PATH_MAX], sub[NP_PATH_MAX], locked[NP_PATH_MAX], link[NP_PATH_MAX];
    path_join(root, sizeof(root), base, "failing-games");
    make_dirs(root);
    rpg_game(root, "Good");
    make_dir(dir, sizeof(dir), root, "Collection");
    make_dir(sub, sizeof(sub), dir, "Series");
    rpg_game(sub, "Deep OK");
    Library lib = {0};
    char error[NP_PATH_MAX + 64];
    CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)) && lib.count == 2, "the first scan");
    static const char *const EVERY[] = {"Good", "Deep OK", "New Game", NULL};
    rpg_game(root, "New Game");

    /* depths 0 and 1, as before: a dangling link one folder down fails the scan */
    make_dir(dir, sizeof(dir), root, "Box");
    path_join(link, sizeof(link), dir, "Dangling");
    CHECK(!symlink("/nonexistent/dulcamara-test", link), "a dangling link one folder down");
    CHECK(!library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
          "a dangling link at depth 1 still fails the scan");
    CHECK(strstr(error, "Box/Dangling") != NULL, "and the error names it: %s", error);
    CHECK(lib.count == 2 && find(&lib, "Good") && find(&lib, "Deep OK") && !find(&lib, "New Game"),
          "and the library is the one it had");
    unlink(link);

    if (geteuid() == 0)
        printf("SKIP the unreadable-folder checks: running as root, which reads every folder\n");
    else
    {
        /* depth 1: an unreadable folder still fails the scan */
        make_dir(locked, sizeof(locked), dir, "Locked Folder");
        chmod(locked, 0);
        CHECK(!library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
              "an unreadable folder at depth 1 still fails the scan");
        CHECK(strstr(error, "Box/Locked Folder") != NULL, "and the error names it: %s", error);
        CHECK(lib.count == 2 && !find(&lib, "New Game"), "and the library is the one it had");
        chmod(locked, 0700);
        rmdir(locked);

        /* depth 2: an unreadable folder is reported, and the scan goes on */
        path_join(sub, sizeof(sub), root, "Collection/Series");
        make_dir(locked, sizeof(locked), sub, "Locked Deep");
        chmod(locked, 0);
        CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
              "an unreadable folder two folders down does not fail the scan: %s", error);
        CHECK(all_listed(&lib, EVERY), "every other game is listed (%d)", lib.count);
        const char *why = skip_reason(&lib, "Collection/Series/Locked Deep");
        CHECK(why && strstr(why, "could not be read"), "and the unreadable folder says why: %s", why ? why : "-");
        chmod(locked, 0700);
        rmdir(locked);

        /* inside a Ren'Py game's game/: a subfolder that cannot be opened holds no scripts */
        char game_dir[NP_PATH_MAX], scripts[NP_PATH_MAX], shut[NP_PATH_MAX];
        make_dir(dir, sizeof(dir), root, "Locked Inside");
        make_dir(game_dir, sizeof(game_dir), dir, "game");
        make_dir(shut, sizeof(shut), game_dir, "aaa-locked");
        make_dir(scripts, sizeof(scripts), game_dir, "scripts");
        write_text(scripts, "script.rpyc", "compiled");
        make_dir(dir, sizeof(dir), root, "Only Locked Inside");
        make_dir(game_dir, sizeof(game_dir), dir, "game");
        make_dir(locked, sizeof(locked), game_dir, "locked");
        chmod(shut, 0);
        chmod(locked, 0);
        CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
              "an unreadable folder inside game/ does not fail the scan: %s", error);
        CHECK(all_listed(&lib, EVERY), "every other game is listed (%d)", lib.count);
        const Game *inside = find(&lib, "Locked Inside");
        CHECK(inside && core_is_renpy(inside->core), "a Ren'Py game with scripts beside the locked folder is listed");
        CHECK(!find(&lib, "Only Locked Inside"), "one with nothing but a locked folder in game/ is not");
        why = skip_reason(&lib, "Only Locked Inside/game/locked");
        CHECK(why && strstr(why, "could not be read"), "and its locked folder (depth 2) says why: %s",
              why ? why : "-");
        chmod(shut, 0700);
        chmod(locked, 0700);
    }

    /* depth 2: a dangling link is reported, and the scan goes on */
    path_join(sub, sizeof(sub), root, "Collection/Series");
    path_join(link, sizeof(link), sub, "Gone");
    CHECK(!symlink("/nonexistent/dulcamara-test", link), "a dangling link two folders down");
    CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
          "a dangling link two folders down does not fail the scan: %s", error);
    CHECK(all_listed(&lib, EVERY), "every other game is listed (%d)", lib.count);
    const char *why = skip_reason(&lib, "Collection/Series/Gone");
    CHECK(why && strstr(why, "could not be read"), "and the link says why: %s", why ? why : "-");
    unlink(link);

    /* a Japanese collection, title and build folder: the build folder (depth 2) passes the app's 512 bytes */
    char collection[256], title[256], build[256], deeper[1024], deepest[1400];
    repeat(collection, sizeof(collection), "\xe5\x90\x8c", 50); /* 50 x U+540C, 150 bytes */
    repeat(title, sizeof(title), "\xe3\x81\x82", 60);           /* 60 x U+3042, 180 bytes */
    repeat(build, sizeof(build), "\xe7\x89\x88", 50);           /* 50 x U+7248, 150 bytes */
    make_dir(dir, sizeof(dir), root, collection);
    snprintf(deeper, sizeof(deeper), "%s/%s", dir, title);
    snprintf(deepest, sizeof(deepest), "%s/%s", deeper, build);
    CHECK(strlen(deeper) < NP_PATH_MAX && strlen(deepest) >= NP_PATH_MAX, "the build folder's path is %zu bytes",
          strlen(deepest));
    CHECK(!mkdir(deeper, 0700) && !mkdir(deepest, 0700), "folders with long Japanese names");
    CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
          "a path too long two folders down does not fail the scan: %s", error);
    CHECK(all_listed(&lib, EVERY), "every other game is listed (%d)", lib.count);
    why = skip_reason_from(&lib, deeper);
    CHECK(why && strstr(why, "path too long"), "and the over-long path says why: %s", why ? why : "-");
    for (int i = 0; i < lib.skipped_count; ++i)
        if (!strncmp(lib.skipped[i].path, deeper, strlen(deeper)))
            CHECK(utf8_valid(lib.skipped[i].path), "its recorded path is cut at a character: %s",
                  lib.skipped[i].path);
    rmdir(deepest);
    rmdir(deeper);
    rmdir(dir);

    /* a path too long one folder down (depth 1): reported too, as such a game could never be listed */
    char long_a[241], long_b[241];
    memset(long_a, 'a', 240), long_a[240] = '\0';
    memset(long_b, 'b', 240), long_b[240] = '\0';
    make_dir(dir, sizeof(dir), root, long_a);
    snprintf(deeper, sizeof(deeper), "%s/%s", dir, long_b);
    CHECK(strlen(deeper) >= NP_PATH_MAX && !mkdir(deeper, 0700), "a folder one down longer than the app's paths");
    CHECK(library_scan_checked(&lib, root, NULL, NULL, error, sizeof(error)),
          "a path too long one folder down does not fail the scan: %s", error);
    CHECK(all_listed(&lib, EVERY), "every other game is listed (%d)", lib.count);
    why = skip_reason_from(&lib, dir);
    CHECK(why && strstr(why, "path too long"), "and it says why: %s", why ? why : "-");
    rmdir(deeper);
    library_free(&lib);
}

/* miniz's writer to a file at any offset, so that what it never writes (the space reserved before the first entry)
 * is a hole in a sparse file */
static size_t write_at(void *opaque, mz_uint64 offset, const void *data, size_t n)
{
    FILE *f = opaque;
    return fseeko(f, (off_t)offset, SEEK_SET) == 0 ? fwrite(data, 1, n, f) : 0;
}

/* An RPG Maker game in a .zip, written after `before` bytes that are never written (a sparse file over 4 GiB when
 * before is that large), with flags (MZ_ZIP_FLAG_WRITE_ZIP64 forces the ZIP64 format). */
static bool write_game_zip(const char *path, mz_uint64 before, mz_uint flags)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    zip.m_pWrite = write_at;
    zip.m_pIO_opaque = f;
    bool ok = mz_zip_writer_init_v2(&zip, before, flags) &&
              mz_zip_writer_add_mem(&zip, "Game/RPG_RT.ldb", "x", 1, MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "Game/RPG_RT.lmt", "x", 1, MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return fclose(f) == 0 && ok;
}

/* An RPG Maker game in a .zip with `extra` more entries (MZ_ZIP_FLAG_WRITE_ZIP64 in flags forces the ZIP64 format) */
static bool write_many_zip(const char *path, unsigned extra, mz_uint flags)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    bool ok = mz_zip_writer_init_file_v2(&zip, path, 0, flags) &&
              mz_zip_writer_add_mem(&zip, "Game/RPG_RT.ldb", "x", 1, MZ_NO_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "Game/RPG_RT.lmt", "x", 1, MZ_NO_COMPRESSION);
    for (unsigned i = 0; ok && i < extra; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "Game/Pad/%05u", i);
        ok = mz_zip_writer_add_mem(&zip, name, "", 0, MZ_NO_COMPRESSION);
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    return mz_zip_writer_end(&zip) && ok;
}

/* An RPG Maker game in a .zip with one more entry that says it holds 5 GiB (a few bytes stored compressed, never
 * read): a ZIP64 entry size. */
static bool write_big_entry_zip(const char *path)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    static const unsigned char deflated[] = {0x03, 0x00}; /* an empty deflate stream */
    bool ok = mz_zip_writer_init_file_v2(&zip, path, 0, MZ_ZIP_FLAG_WRITE_ZIP64) &&
              mz_zip_writer_add_mem(&zip, "Game/RPG_RT.ldb", "x", 1, MZ_NO_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "Game/RPG_RT.lmt", "x", 1, MZ_NO_COMPRESSION) &&
              mz_zip_writer_add_mem_ex(&zip, "Game/Movie/big.avi", deflated, sizeof(deflated), NULL, 0,
                                       MZ_DEFAULT_LEVEL | MZ_ZIP_FLAG_COMPRESSED_DATA, 5ull << 30, 0) &&
              mz_zip_writer_finalize_archive(&zip);
    return mz_zip_writer_end(&zip) && ok;
}

/* EasyRPG's ZIP reader has no ZIP64 (filesystem_zip.cpp): it reads the classic end record, the central directory and
 * each local header, with 32-bit offsets and sizes and a 16-bit entry count. A .zip past those limits is not listed
 * (it would fail at launch); it is reported. A .zip within them is listed, even one that carries a ZIP64 end record
 * beside valid classic fields (as some tools always write), which EasyRPG never reads. */
static void check_large_zips(void)
{
    char root[NP_PATH_MAX], huge[NP_PATH_MAX], forced[NP_PATH_MAX], normal[NP_PATH_MAX], many[NP_PATH_MAX],
        many64[NP_PATH_MAX], most[NP_PATH_MAX], big[NP_PATH_MAX];
    path_join(root, sizeof(root), base, "large-zips");
    make_dirs(root);
    path_join(huge, sizeof(huge), root, "Huge.zip");
    path_join(forced, sizeof(forced), root, "Forced64.zip");
    path_join(normal, sizeof(normal), root, "Normal.zip");
    path_join(many, sizeof(many), root, "Many.zip");
    path_join(many64, sizeof(many64), root, "Many64.zip");
    path_join(most, sizeof(most), root, "Most.zip");
    path_join(big, sizeof(big), root, "BigEntry.zip");
    CHECK(write_game_zip(normal, 0, 0), "a normal .zip");
    CHECK(write_game_zip(forced, 0, MZ_ZIP_FLAG_WRITE_ZIP64), "a small .zip in the ZIP64 format");
    CHECK(write_many_zip(most, 65535 - 2, 0), "a .zip of 65,535 entries");
    CHECK(write_many_zip(many, 65536 - 2, 0), "a .zip of 65,536 entries");
    CHECK(write_many_zip(many64, 65536 - 2, MZ_ZIP_FLAG_WRITE_ZIP64), "and one written as ZIP64");
    CHECK(write_big_entry_zip(big), "a .zip with an entry of 5 GiB");
    const mz_uint64 four_gib = 4ull << 30;
    bool sparse = write_game_zip(huge, four_gib + 4096, 0);
    struct stat st;
    sparse = sparse && stat(huge, &st) == 0 && (uint64_t)st.st_size > four_gib &&
             (uint64_t)st.st_blocks * 512 < (64u << 20);
    if (!sparse)
    {
        printf("SKIP the .zip over 4 GiB: this filesystem cannot make it sparse (or write it)\n");
        remove(huge);
    }

    Game g;
    CHECK(library_read_archive(forced, &g), "a small .zip with a ZIP64 end record and valid classic fields is offered");
    CHECK(library_read_archive(most, &g), "a .zip of 65,535 entries is offered");
    CHECK(!library_read_archive(many, &g), "a .zip of 65,536 entries is not");
    CHECK(!library_read_archive(many64, &g), "nor one of 65,536 written as ZIP64");
    CHECK(!library_read_archive(big, &g), "nor one with an entry of 5 GiB");
    if (sparse)
        CHECK(!library_read_archive(huge, &g), "nor a .zip over 4 GiB (its entries past 4 GiB)");
    Library lib = {0};
    library_scan(&lib, root);
    printf("large zips: %d games, %d not shown\n", lib.count, lib.skipped_count);
    for (int i = 0; i < lib.skipped_count; ++i)
        printf("  %s: %s\n", base_of(lib.skipped[i].path), lib.skipped[i].reason);
    CHECK(lib.count == 3 && find(&lib, "Normal") && find(&lib, "Normal")->archive && find(&lib, "Forced64") &&
              find(&lib, "Most"),
          "the normal, the ZIP64-recorded and the 65,535-entry .zip are listed: %d", lib.count);
    static const char *const TOO_LARGE[] = {"Many.zip", "Many64.zip", "BigEntry.zip", "Huge.zip"};
    for (size_t i = 0; i < sizeof(TOO_LARGE) / sizeof(TOO_LARGE[0]); ++i)
    {
        if (i == 3 && !sparse)
            continue;
        const char *why = skip_reason(&lib, TOO_LARGE[i]);
        CHECK(why && strstr(why, "too large for the RPG Maker player") && strstr(why, "extract it into a folder"),
              "%s says why: %s", TOO_LARGE[i], why ? why : "-");
    }
    library_free(&lib);
    remove(huge);
}

static void check_testgame(const char *dir)
{
    Library lib = {0};
    library_scan(&lib, dir);
    library_sort(&lib, 0);
    printf("TestGame: %d games\n", lib.count);
    static const struct
    {
        const char *name, *title, *cover;
        int engine;
    } WANT[] = {
        {"TestGame-2000", "RPG Maker 2000 Test suite", "/Title/Title2.png", 2000},
        {"TestGame-2003", "RPG Maker 2003 Test suite", "/Title/Title1.png", 2003},
        {"TestGame-EasyRPG", "EasyRPG Test suite", "/Title/Title1.png", 0},
        {"TestGame-Maniac", "RPG Maker Maniac Test suite", "/Title/Title1.png", 0},
        {"TestGame-PowerMode2003", "Power Mode 2003 test suite", "/Title/Title1.png", 2003},
    };
    CHECK(lib.count == 5, "TestGame's five suites, found %d", lib.count);
    for (size_t i = 0; i < sizeof(WANT) / sizeof(WANT[0]); ++i)
    {
        Game *g = (Game *)find(&lib, WANT[i].name);
        char cover[NP_PATH_MAX] = "";
        const bool has = g && library_title_image(g, cover, sizeof(cover));
        printf("  %-24s %-30s engine %-4d cover %s\n", WANT[i].name, g ? g->title : "(missing)", g ? g->engine : 0,
               has ? strstr(cover, "/Title/") : "none");
        CHECK(g && !strcmp(g->title, WANT[i].title), "%s's title: %s", WANT[i].name, g ? g->title : "-");
        CHECK(has && strstr(cover, WANT[i].cover), "%s's cover: %s", WANT[i].name, cover);
        CHECK(!WANT[i].engine || (g && g->engine == WANT[i].engine), "%s's engine: %d", WANT[i].name,
              g ? g->engine : 0);
    }
    library_free(&lib);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <empty folder> [TestGame folder]\n", argv[0]);
        return 2;
    }
    str_copy(base, sizeof(base), argv[1]);
    path_join(games, sizeof(games), base, "games");
    make_dirs(games);
    make_games();
    check_made_up_games();
    check_renpy_games();
    check_sections();
    check_pieces();
    check_detection();
    check_failed_scan();
    check_large_zips();
    if (argc > 2)
        check_testgame(argv[2]);
    printf("%d checks, %d failed\n%s\n", checks, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
