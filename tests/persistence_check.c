/* Real filesystem persistence contracts. SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "core/cores.h"
#include "library.h"
#include "storage.h"
#include <unistd.h>
#include "profile.h"
#include "userdata.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The config consumer needs only core names and button defaults, not engine executables. */
static const int8_t buttons[MAP_BUTTONS] = {8, 1, -1, 3, 4, 5, 6, 7, 0, 9, -1, -1, -1, -1, 14, 15};
const CoreInfo CORES[CORE_COUNT] = {
    [CORE_EASYRPG] = {.id = "easyrpg", .buttons = buttons},
    [CORE_RENPY] = {.id = "renpy", .buttons = buttons},
    [CORE_RENPY7] = {.id = "renpy7", .buttons = buttons},
};
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}
static void expect_text(const char *path, const char *wanted)
{
    char buffer[4096];
    FILE *f = fopen(path, "r");
    assert(f);
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[n] = 0;
    assert(fclose(f) == 0);
    assert(!strcmp(buffer, wanted));
}
static void checked_storage(const char *dir)
{
    char path[NP_PATH_MAX], child[NP_PATH_MAX], temporary[NP_PATH_MAX];
    path_join(path, sizeof(path), dir, "target");
    assert(mkdir(path, 0700) == 0);
    path_join(child, sizeof(child), path, "old");
    put(child, "old bytes");
    Settings settings;
    config_defaults(&settings);
    assert(!config_save(&settings, path));
    expect_text(child, "old bytes");
    path_join(path, sizeof(path), dir, "atomic.txt");
    put(path, "old bytes");
    StorageFile write;
    assert(storage_begin(&write, path));
    str_copy(temporary, sizeof(temporary), write.temporary);
    fputs("new bytes", write.file);
    storage_abort(&write);
    expect_text(path, "old bytes");
    assert(!path_exists(temporary));
    assert(storage_begin(&write, path));
    fputs("new bytes", write.file);
    assert(storage_commit(&write));
    expect_text(path, "new bytes");
    assert(storage_begin(&write, path));
    /* Real OS descriptor failure, without production fault-injection hooks. */
    assert(close(fileno(write.file)) == 0);
    assert(!storage_commit(&write));
    expect_text(path, "new bytes");
    assert(storage_begin(&write, path));
    fputs("unpublished", write.file);
    assert(unlink(write.temporary) == 0);
    assert(!storage_commit(&write));
    expect_text(path, "new bytes");
    assert(!storage_begin(&write, "/does/not/exist/settings.ini"));
    char too_long[NP_PATH_MAX + 20];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    assert(!storage_begin(&write, too_long));
    path_join(path, sizeof(path), dir, "keys.txt");
    int8_t keys[MAP_BUTTONS] = {0}, loaded[MAP_BUTTONS];
    keys[PAD_B] = 5;
    assert(config_save_keys(keys, path, "Game\nTitle"));
    config_load_keys(loaded, path);
    assert(loaded[PAD_B] == 5 && loaded[PAD_A] == 0);
    char base[NP_PATH_MAX], list[NP_PATH_MAX];
    path_join(base, sizeof(base), dir, "sounds");
    snprintf(list, sizeof(list), "%s.list", base);
    put(list, "audio/a.ogg\naudio/b.ogg\n");
    SoundList sounds = {0};
    assert(config_load_sounds(&sounds, base));
    sounds.off[1] = true;
    assert(config_save_sounds(&sounds, base, "Game"));
    config_free_sounds(&sounds);
    assert(config_load_sounds(&sounds, base));
    assert(!sounds.off[0] && sounds.off[1]);
    config_free_sounds(&sounds);
    path_join(path, sizeof(path), dir, "keys-blocked");
    assert(mkdir(path, 0700) == 0);
    path_join(child, sizeof(child), path, "old");
    put(child, "key original");
    assert(!config_save_keys(keys, path, "Game"));
    expect_text(child, "key original");
    path_join(base, sizeof(base), dir, "sound-blocked");
    snprintf(path, sizeof(path), "%s.txt", base);
    assert(mkdir(path, 0700) == 0);
    path_join(child, sizeof(child), path, "old");
    put(child, "sound original");
    if (geteuid() != 0)
    {
        char restricted[NP_PATH_MAX], restricted_key[NP_PATH_MAX], restricted_sound[NP_PATH_MAX];
        path_join(restricted, sizeof(restricted), dir, "restricted");
        assert(mkdir(restricted, 0700) == 0);
        path_join(restricted_key, sizeof(restricted_key), restricted, "keys.txt");
        put(restricted_key, "keep keys");
        path_join(restricted_sound, sizeof(restricted_sound), restricted, "sounds");
        char restricted_txt[NP_PATH_MAX];
        snprintf(restricted_txt, sizeof(restricted_txt), "%s.txt", restricted_sound);
        put(restricted_txt, "keep sounds");
        assert(chmod(restricted, 0000) == 0);
        int8_t empty_keys[MAP_BUTTONS] = {0};
        SoundList empty_sounds = {0};
        bool keys_reset = config_save_keys(empty_keys, restricted_key, "Game");
        bool sounds_reset = config_save_sounds(&empty_sounds, restricted_sound, "Game");
        assert(chmod(restricted, 0700) == 0);
        assert(!keys_reset && !sounds_reset);
        expect_text(restricted_key, "keep keys");
        expect_text(restricted_txt, "keep sounds");
    }
    else
        puts("SKIP permission reset errors: root bypasses filesystem permissions");
    char *names[] = {"audio/a.ogg"};
    bool off[] = {true};
    sounds = (SoundList){.names = names, .off = off, .count = 1};
    assert(!config_save_sounds(&sounds, base, "Game"));
    expect_text(child, "sound original");
}
static void history(const char *dir)
{
    char path[NP_PATH_MAX], child[NP_PATH_MAX];
    path_join(path, sizeof(path), dir, "played.txt");
    put(path, "abc\t42\nabc-longer\t99\nabsent\t125\n");
    Game game = {.id = "abc"};
    Library lib = {.games = &game, .count = 1};
    library_load_played(&lib, path);
    assert(game.played == 42);
    assert(library_mark_played(&lib, 0, path));
    FILE *f = fopen(path, "r");
    assert(f);
    char text[512];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    assert(strstr(text, "abc-longer\t99\n"));
    assert(strstr(text, "absent\t125\n"));
    path_join(path, sizeof(path), dir, "blocked-history");
    assert(mkdir(path, 0700) == 0);
    path_join(child, sizeof(child), path, "old");
    put(child, "history original");
    game.played = 123;
    assert(!library_mark_played(&lib, 0, path));
    assert(game.played == 123);
    expect_text(child, "history original");
    path_join(path, sizeof(path), dir, "bad-history.txt");
    put(path, "abc\t-10\nabc\tnonsense\nabc\t99999999999999999999999999999\n");
    game.played = 0;
    library_load_played(&lib, path);
    assert(game.played == 0);
}
static void profiles(const char *dir)
{
    char path[NP_PATH_MAX];
    path_join(path, sizeof(path), dir, "profile.ini");
    Settings global, effective;
    config_defaults(&global);
    Settings original = global;
    GameProfile profile = {0}, loaded;
    assert(!profile_load(&loaded, path));
    profile_resolve(&global, &loaded, &effective);
    assert(effective.game_volume == 10 && effective.automatic_backups);
    assert(profile_set(&profile, PROFILE_INTEGER_SCALE, 1));
    assert(profile_set(&profile, PROFILE_SMOOTH, 1));
    assert(profile_set(&profile, PROFILE_ASPECT, 1));
    assert(profile_set(&profile, PROFILE_UPSCALE, 3));
    assert(profile_set(&profile, PROFILE_UPSCALE_FILTER, UPSCALE_XBR));
    assert(profile_set(&profile, PROFILE_FF_TOGGLE, 1));
    assert(profile_set(&profile, PROFILE_STICK_DPAD, 0));
    assert(profile_set(&profile, PROFILE_SHOW_FPS, 1));
    assert(profile_set(&profile, PROFILE_RENPY_SKIP_UNSEEN, 1));
    assert(profile_set(&profile, PROFILE_GAME_VOLUME, 3));
    assert(profile_set(&profile, PROFILE_SHADER, 8));
    assert(profile_set(&profile, PROFILE_FF_SPEED, 4));
    assert(profile_set(&profile, PROFILE_AUTOMATIC_BACKUPS, 0));
    assert(profile_set_button(&profile, CORE_RENPY, PAD_B, PAD_Y));
    assert(profile_set_button(&profile, CORE_RENPY7, PAD_A, PAD_NONE));
    assert(!profile_set(&profile, PROFILE_GAME_VOLUME, 99));
    assert(!profile_set_button(&profile, CORE_COUNT, PAD_B, PAD_A));
    assert(profile_save(&profile, path));
    assert(profile_load(&loaded, path));
    profile_resolve(&global, &loaded, &effective);
    assert(effective.shader == 8 && effective.game_volume == 3 && effective.ff_speed == 4);
    assert(!effective.automatic_backups && effective.upscale == 3);
    assert(effective.integer_scale && effective.smooth && effective.aspect == 1 &&
           effective.upscale_filter == UPSCALE_XBR);
    assert(effective.ff_toggle == 1 && !effective.stick_dpad && effective.show_fps &&
           effective.renpy_skip_unseen);
    assert(effective.button_map[CORE_RENPY][PAD_B] == PAD_Y);
    assert(effective.button_map[CORE_EASYRPG][PAD_B] == PAD_A);
    assert(effective.button_map[CORE_RENPY7][PAD_A] == PAD_NONE);
    assert(!memcmp(&global, &original, sizeof(global)));
    profile_reset(&loaded, PROFILE_SHADER);
    profile_reset_button(&loaded, CORE_RENPY, PAD_B);
    assert(profile_save(&loaded, path));
    assert(profile_load(&loaded, path));
    profile_resolve(&global, &loaded, &effective);
    assert(effective.shader == 0 && effective.game_volume == 3 &&
           effective.button_map[CORE_RENPY][PAD_B] == PAD_A);
    GameProfile other = {0};
    char other_path[NP_PATH_MAX];
    path_join(other_path, sizeof(other_path), dir, "other-profile.ini");
    assert(profile_set(&other, PROFILE_GAME_VOLUME, 7));
    assert(profile_save(&other, other_path));
    assert(profile_load(&other, other_path));
    profile_resolve(&global, &other, &effective);
    assert(effective.game_volume == 7 && effective.shader == 0 &&
           effective.button_map[CORE_RENPY][PAD_B] == PAD_A);
    memset(&other, 0, sizeof(other));
    profile_resolve(&global, &other, &effective);
    assert(effective.game_volume == 10 && effective.automatic_backups);
    assert(!memcmp(&global, &original, sizeof(global)));
    put(path, "profile_version = 1\ngame_volume = 2\nshader = bogus\nff_speed = 3");
    assert(!profile_load(&loaded, path));
    profile_resolve(&global, &loaded, &effective);
    assert(effective.game_volume == 10 && effective.shader == 0 && effective.ff_speed == 1);
    put(path, "profile_version = 1\ngame_volume = 2\n");
    assert(!profile_load(&loaded, path)); /* truncation exactly after a complete record */
    put(path, "profile_version = 999\ngame_volume = 2\nprofile_end\n");
    assert(!profile_load(&loaded, path));
    put(path, "profile_version = 1\ngame_volume = 99\nprofile_end\n");
    assert(!profile_load(&loaded, path));
    profile_resolve(&global, &loaded, &effective);
    assert(effective.game_volume == 10);
    put(path, "profile_version = 1\ngame_volume = 2\nshader = bogus\nprofile_end\n");
    assert(!profile_load(&loaded, path));
    profile_resolve(&global, &loaded, &effective);
    assert(effective.game_volume == 10 && effective.shader == 0);
    /* A number must consume the entire value, including bytes beyond a 63-byte scanset. */
    char malformed[256];
    snprintf(malformed, sizeof(malformed),
             "profile_version = 1\ngame_volume = 2\nautomatic_backups = 0%64sjunk\nprofile_end\n",
             "");
    put(path, malformed);
    assert(!profile_load(&loaded, path));
    GameProfile inherited = {0};
    assert(!memcmp(&loaded, &inherited, sizeof(loaded)));
    profile_resolve(&global, &loaded, &effective);
    assert(!memcmp(&effective, &original, sizeof(effective)));
    char blocked[NP_PATH_MAX], child[NP_PATH_MAX];
    path_join(blocked, sizeof(blocked), dir, "blocked-profile");
    assert(mkdir(blocked, 0700) == 0);
    path_join(child, sizeof(child), blocked, "old");
    put(child, "profile original");
    assert(!profile_save(&profile, blocked));
    expect_text(child, "profile original");
}
static void metadata(const char *dir)
{
    char path[NP_PATH_MAX];
    path_join(path, sizeof(path), dir, "library-user.txt");
    UserMetadata metadata = {0};
    assert(user_metadata_set_favorite(&metadata, "absent", true));
    assert(user_metadata_add_playtime(&metadata, "absent", 120));
    assert(user_metadata_set_favorite(&metadata, "present", false));
    assert(user_metadata_add_playtime(&metadata, "present", 35));
    assert(user_metadata_save(&metadata, path));
    user_metadata_free(&metadata);
    assert(user_metadata_load(&metadata, path));
    const GameUserData *record = user_metadata_find(&metadata, "absent");
    assert(record && record->favorite && record->playtime_seconds == 120);
    assert(user_metadata_add_playtime(&metadata, "present", 10));
    assert(user_metadata_save(&metadata, path));
    user_metadata_free(&metadata);
    assert(user_metadata_load(&metadata, path));
    record = user_metadata_find(&metadata, "absent");
    assert(record && record->favorite && record->playtime_seconds == 120);
    record = user_metadata_find(&metadata, "present");
    assert(record && !record->favorite && record->playtime_seconds == 45);
    assert(!user_metadata_add_playtime(&metadata, "../escape", 9));
    put(path, "metadata_version = 1\nabsent\t1\t120\npresent\t0\t");
    assert(!user_metadata_load(&metadata, path));
    assert(!user_metadata_find(&metadata, "absent"));
    put(path, "metadata_version = 1\nabsent\t1\t120\n");
    assert(!user_metadata_load(&metadata,
                               path)); /* boundary truncation must not publish a partial list */
    put(path, "metadata_version = 999\nabsent\t1\t120\nmetadata_end\n");
    assert(!user_metadata_load(&metadata, path));
    user_metadata_free(&metadata);
    assert(user_metadata_add_playtime(&metadata, "large", UINT64_MAX));
    assert(user_metadata_add_playtime(&metadata, "large", 1));
    assert(user_metadata_find(&metadata, "large")->playtime_seconds == UINT64_MAX);
    assert(user_metadata_save(&metadata, path));
    assert(user_metadata_load(&metadata, path));
    assert(user_metadata_find(&metadata, "large")->playtime_seconds == UINT64_MAX);
    put(path, "metadata_version = 1\nlarge\t1\t-1\nmetadata_end\n");
    assert(!user_metadata_load(&metadata, path));
    assert(!user_metadata_find(&metadata, "large"));
    put(path, "metadata_version = 1\nlarge\t1\t18446744073709551616\nmetadata_end\n");
    assert(!user_metadata_load(&metadata, path));
    assert(user_metadata_set_favorite(&metadata, "absent", true));
    char blocked[NP_PATH_MAX], child[NP_PATH_MAX];
    path_join(blocked, sizeof(blocked), dir, "blocked-metadata");
    assert(mkdir(blocked, 0700) == 0);
    path_join(child, sizeof(child), blocked, "old");
    put(child, "metadata original");
    assert(!user_metadata_save(&metadata, blocked));
    expect_text(child, "metadata original");
    user_metadata_free(&metadata);
}
static void selection(const char *dir)
{
    char path[NP_PATH_MAX];
    path_join(path, sizeof(path), dir, "settings.ini");
    put(path, "last_game = 1\ngame_volume = 6\nrenpy_button_0 = 9\n");
    Settings settings;
    config_defaults(&settings);
    config_load(&settings, path);
    assert(settings.game_volume == 6 && settings.button_map[CORE_RENPY][0] == 9 &&
           settings.automatic_backups);
    Game games[3] = {{.id = "a"}, {.id = "b"}, {.id = "c"}};
    Library lib = {.games = games, .count = 3};
    assert(library_restore_selection(&lib, &settings) == 1);
    assert(!strcmp(settings.selected_game_id, "b"));
    assert(config_save(&settings, path));
    Game swap = games[0];
    games[0] = games[1];
    games[1] = swap;
    config_defaults(&settings);
    config_load(&settings, path);
    assert(library_restore_selection(&lib, &settings) == 0);
    assert(library_select(&lib, &settings, 2));
    assert(config_save(&settings, path));
    config_defaults(&settings);
    config_load(&settings, path);
    assert(!strcmp(settings.selected_game_id, "c"));
    lib.count = 0;
    assert(library_restore_selection(&lib, &settings) == 0);
    assert(!strcmp(settings.selected_game_id, "c"));
    settings.automatic_backups = false;
    assert(config_save(&settings, path));
    config_defaults(&settings);
    config_load(&settings, path);
    assert(!settings.automatic_backups);
    /* Settings > Library > View: Story by default (also for a legacy file without the key), Shelf persists */
    assert(settings.library_view == 0);
    settings.library_view = 1;
    assert(config_save(&settings, path));
    config_defaults(&settings);
    assert(settings.library_view == 0);
    config_load(&settings, path);
    assert(settings.library_view == 1 && !settings.automatic_backups);
    settings.library_view = 0;
    assert(config_save(&settings, path));
    config_defaults(&settings);
    config_load(&settings, path);
    assert(settings.library_view == 0);
    put(path, "library_view = 7\n");
    config_defaults(&settings);
    config_load(&settings, path);
    assert(settings.library_view == 1); /* clamped to a known view */
    Paths paths;
    config_paths(&paths, dir);
    assert(strstr(paths.metadata, "library-user.txt") && strstr(paths.profiles, "profiles") &&
           strstr(paths.backups, "backups"));
    put(path, "game_volume = nonsense\nupscale = 3junk\nautomatic_backups = invalid\n");
    config_defaults(&settings);
    config_load(&settings, path);
    assert(settings.game_volume == 10 && settings.upscale == 1 && settings.automatic_backups);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    if (!strcmp(argv[2], "storage"))
        checked_storage(argv[1]);
    else if (!strcmp(argv[2], "history"))
        history(argv[1]);
    else if (!strcmp(argv[2], "profiles"))
        profiles(argv[1]);
    else if (!strcmp(argv[2], "metadata"))
        metadata(argv[1]);
    else if (!strcmp(argv[2], "selection"))
        selection(argv[1]);
    else
        assert(!"unknown test");
    printf("PASS %s\n", argv[2]);
    return 0;
}
