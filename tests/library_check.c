/*
 * Dulcamara - the library's game detection (src/library.c, src/rpgmaker.c), on the build machine: a games
 * folder of made-up games is written, scanned and checked, then EasyRPG's TestGame when it is given.
 * tests/test_library.py builds and runs it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   library-check <empty temporary folder> [TestGame folder]
 */
#include "../src/library.h"
#include "../src/rpgmaker.h"

#include "../third_party/miniz/miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    check_pieces();
    if (argc > 2)
        check_testgame(argv[2]);
    printf("%d checks, %d failed\n%s\n", checks, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
