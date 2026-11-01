/*
 * Dulcamara - updates from Oliv3rmoon/Dulcamara's GitHub releases (update_release.h), as
 * Mandrake (and SwanStationPS5 before it) updates itself.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Checks api.github.com for the repository's latest release (not a draft or a pre-release), or,
 * with Settings > System > Test builds on, for its 20 most recent releases, test builds
 * (pre-releases) among them, so that testers needn't install each test build by hand;
 * update_release.c decides which is offered: a newer X.Y.Z tag carrying PPSA99010.zip and
 * SHA256SUMS, both this repository's own files, the newest such of the list with Test builds on
 * (never a draft). Either way, the same checks. Installing downloads the ZIP, checks it against
 * the SHA-256 that SHA256SUMS gives it, and writes the PPSA99010 folder inside it over the
 * installed app. Each file goes to <name>.new and is renamed into place: a running eboot.bin
 * can't be overwritten, but it can be replaced by name, and the new one runs from the next start.
 */
#include "update.h"

#include "drives.h"
#include "app.h"
#include "i18n.h"
#include "net.h"
#include "update_release.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APP_DIR_DEFAULT "/data/homebrew/" NP_TITLE_ID

static SDL_atomic_t state;
static char version[32], zip_url[256], zip_sha256[65], message[256];
static bool test_build; /* what is offered is a test build (a pre-release) */

int update_state(void)
{
    /* a test build found while Test builds was on is no longer offered once it is off: not installed, not shown */
    if (SDL_AtomicGet(&state) == UPDATE_AVAILABLE && test_build && !app.settings.update_test_builds &&
        SDL_AtomicCAS(&state, UPDATE_AVAILABLE, UPDATE_NONE))
        np_log("update: %s is a test build and Test builds is off now; not offered", version);
    return SDL_AtomicGet(&state);
}

const char *update_version(void)
{
    return version;
}

const char *update_message(void)
{
    return message;
}

static void fail(const char *why)
{
    str_copy(message, sizeof(message), why);
    np_log("update: %s", why);
    SDL_AtomicSet(&state, UPDATE_FAILED);
}

/* test_builds_on: not 0 when Settings > System > Test builds was on as the check started */
static int check_thread(void *test_builds_on)
{
    const bool test_builds = (uintptr_t)test_builds_on != 0;
    char *body = NULL;
    size_t len = 0;
    /* releases/latest: the newest release that is neither a draft nor a pre-release. With Test builds on, the
     * 20 most recent releases, pre-releases among them, as a list. */
    int status = test_builds ? net_request(UPDATE_LIST_URL, NULL, NULL, NP_NAME "/" NP_VERSION, &body, &len)
                             : net_request(UPDATE_LATEST_URL, NULL, NULL, NP_NAME "/" NP_VERSION, &body, &len);
    if (status == 404)
    {
        free(body);
        fail(tr("No release found (the repository may be private)"));
        return 0;
    }
    if (status != 200 || !body)
    {
        free(body);
        fail(tr("Couldn't reach GitHub"));
        return 0;
    }
    Release release;
    ReleaseVerdict verdict = test_builds ? release_evaluate_list(body, len, NP_VERSION, true, &release)
                                         : release_evaluate(body, len, NP_VERSION, &release);
    free(body);
    version[0] = zip_url[0] = zip_sha256[0] = '\0';
    /* which was asked: "latest", or "releases (test builds on)" */
    np_log("update: " UPDATE_REPO " %s %s, this %s: %s", test_builds ? "releases (test builds on)" : "latest",
           release.version[0] ? release.version : "?", NP_VERSION, release_verdict_text(verdict));
    if (verdict != RELEASE_OFFER)
    {
        SDL_AtomicSet(&state, UPDATE_NONE);
        return 0;
    }
    /* SHA256SUMS: the SHA-256 the download must have */
    status = net_request(release.sums_url, NULL, NULL, NP_NAME "/" NP_VERSION, &body, &len);
    if (status != 200 || !body)
    {
        free(body);
        fail(tr("Couldn't reach GitHub"));
        return 0;
    }
    char sha[65];
    int listed = release_sums_lookup(body, len, UPDATE_ZIP, sha);
    free(body);
    if (listed != 1 || (release.zip_digest[0] && strcmp(release.zip_digest, sha) != 0))
    {
        np_log("update: %s's " UPDATE_SUMS " %s; not offered", release.version,
                           listed == 0 ? "doesn't list " UPDATE_ZIP
                           : listed < 0 ? "has a malformed or repeated line for " UPDATE_ZIP
                                        : "disagrees with GitHub's digest of " UPDATE_ZIP);
        SDL_AtomicSet(&state, UPDATE_NONE);
        return 0;
    }
    str_copy(version, sizeof(version), release.version);
    str_copy(zip_url, sizeof(zip_url), release.zip_url);
    str_copy(zip_sha256, sizeof(zip_sha256), sha);
    test_build = release.prerelease;
    np_log("update: %s%s available, " UPDATE_ZIP " sha256 %s", version, release.prerelease ? " (a test build)" : "",
           zip_sha256);
    SDL_AtomicSet(&state, UPDATE_AVAILABLE);
    return 0;
}

void update_check(void)
{
    int s = update_state();
    if (s == UPDATE_CHECKING || s == UPDATE_INSTALLING || s == UPDATE_INSTALLED)
        return;
    SDL_AtomicSet(&state, UPDATE_CHECKING);
    /* Test builds goes with the thread, as it is now: Settings can change it while the check runs */
    SDL_Thread *t = SDL_CreateThread(check_thread, "update", (void *)(uintptr_t)app.settings.update_test_builds);
    if (t)
        SDL_DetachThread(t);
    else
        fail("no thread");
}

/* Where the running app is stored. The update has to land there, whether it's in /data/homebrew or on an
 * extended or USB drive (/mnt/ext1/homebrew/PPSA99010 ...). The mount the app runs from (/app0, or the
 * ShadowMountPlus one) is matched to a folder on disk by its eboot.bin: same size and modification time. */
static bool same_eboot(const char *dir, const struct stat *running)
{
    char file[NP_PATH_MAX];
    struct stat st;
    path_join(file, sizeof(file), dir, "eboot.bin");
    return stat(file, &st) == 0 && st.st_size == running->st_size && st.st_mtime == running->st_mtime;
}

static const char *app_dir(void)
{
    static char found[NP_PATH_MAX];
    static bool done;
    if (done)
        return found;
    done = true;
    str_copy(found, sizeof(found), APP_DIR_DEFAULT);
    static const char *const running_dirs[] = {"/app0", "/system_ex/app/" NP_TITLE_ID};
    const char *roots[1 + DRIVE_COUNT] = {"/data"};
    for (int d = 0; d < DRIVE_COUNT; ++d)
        roots[1 + d] = drive_paths[d];
    struct stat running;
    bool have = false;
    for (size_t i = 0; i < sizeof(running_dirs) / sizeof(running_dirs[0]) && !have; ++i)
    {
        char file[NP_PATH_MAX];
        path_join(file, sizeof(file), running_dirs[i], "eboot.bin");
        have = stat(file, &running) == 0;
    }
    if (!have)
        return found;
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r)
    {
        char dir[NP_PATH_MAX];
        /* <root>/<id> and <root>/<any folder>/<id>, such as <root>/homebrew/<id> */
        path_join(dir, sizeof(dir), roots[r], NP_TITLE_ID);
        if (same_eboot(dir, &running))
        {
            str_copy(found, sizeof(found), dir);
            np_log("update: the app is in %s", found);
            return found;
        }
        DIR *d = opendir(roots[r]);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] == '.')
                continue;
            char sub[NP_PATH_MAX];
            path_join(sub, sizeof(sub), roots[r], e->d_name);
            path_join(dir, sizeof(dir), sub, NP_TITLE_ID);
            if (same_eboot(dir, &running))
            {
                str_copy(found, sizeof(found), dir);
                closedir(d);
                np_log("update: the app is in %s", found);
                return found;
            }
        }
        closedir(d);
    }
    np_log("update: couldn't find where the app is stored, using %s", found);
    return found;
}

static int install_thread(void *unused)
{
    (void)unused;
    char zip[NP_PATH_MAX];
    path_join(zip, sizeof(zip), app.paths.root, "cache/update.zip");
    char dir[NP_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.root, "cache");
    make_dirs(dir);
    if (net_download(zip_url, zip) != NET_OK)
    {
        fail(tr("The download failed"));
        return 0;
    }
    /* nothing is unpacked unless the ZIP is the one SHA256SUMS names */
    char got[65] = "";
    if (!release_file_sha256(zip, got) || strcmp(got, zip_sha256) != 0)
    {
        np_log("update: " UPDATE_ZIP " sha256 %s, " UPDATE_SUMS " says %s", got, zip_sha256);
        remove(zip);
        fail(tr("The download doesn't match its SHA256SUMS"));
        return 0;
    }
    mz_zip_archive archive;
    memset(&archive, 0, sizeof(archive));
    if (!mz_zip_reader_init_file(&archive, zip, 0))
    {
        fail(tr("The download isn't a valid zip"));
        return 0;
    }
    int files = 0;
    bool ok = true;
    mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count && ok; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&archive, i, &st) || st.m_is_directory)
            continue;
        /* PPSA99010/<path> -> <where the app is stored>/<path> */
        const char *inside = strstr(st.m_filename, NP_TITLE_ID "/");
        if (!inside || strstr(inside, ".."))
            continue;
        inside += strlen(NP_TITLE_ID "/");
        char target[NP_PATH_MAX], temp[NP_PATH_MAX + 8], parent[NP_PATH_MAX];
        path_join(target, sizeof(target), app_dir(), inside);
        str_copy(parent, sizeof(parent), target);
        char *slash = strrchr(parent, '/');
        if (slash)
        {
            *slash = '\0';
            make_dirs(parent);
        }
        snprintf(temp, sizeof(temp), "%s.new", target);
        /* executable, as an FTP upload makes them: without it the PS5 won't start
         * the app ("Can't start the game or app"), which 2.0.0's updater did */
        ok = mz_zip_reader_extract_to_file(&archive, i, temp, 0) && chmod(temp, 0777) == 0 &&
             rename(temp, target) == 0;
        if (!ok)
            remove(temp);
        files += ok;
    }
    mz_zip_reader_end(&archive);
    remove(zip);
    if (!ok || files == 0)
    {
        fail(tr("Couldn't write the new files (is /data unlocked?)"));
        return 0;
    }
    snprintf(message, sizeof(message), tr("Dulcamara %s is installed: restart Dulcamara to use it"), version);
    np_log("update: installed %s (%d files)", version, files);
    SDL_AtomicSet(&state, UPDATE_INSTALLED);
    return 0;
}

void update_install(void)
{
    if (update_state() != UPDATE_AVAILABLE)
        return;
    SDL_AtomicSet(&state, UPDATE_INSTALLING);
    SDL_Thread *t = SDL_CreateThread(install_thread, "update", NULL);
    if (t)
        SDL_DetachThread(t);
    else
        fail("no thread");
}
