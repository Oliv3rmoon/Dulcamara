/*
 * Dulcamara - updates from Oliv3rmoon/Dulcamara's GitHub releases (update_release.h), as
 * Mandrake (and SwanStationPS5 before it) updates itself.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Checks api.github.com for the repository's latest release (not a draft or a pre-release), or,
 * with Settings > System > Test builds on, for its 20 most recent releases, test builds
 * (pre-releases) among them, so that testers needn't install each test build by hand;
 * update_release.c decides which is offered: a newer X.Y.Z tag carrying PPSA75483.zip and
 * SHA256SUMS, both this repository's own files, the newest such of the list with Test builds on
 * (never a draft). Either way, the same checks. Installing downloads the ZIP, checks it against
 * the SHA-256 that SHA256SUMS gives it, and writes the PPSA75483 folder inside it over the
 * installed app through verified staging and a durable rollback journal. The executable
 * is replaced last, and launch remains locked until the process restarts.
 */
#include "update.h"
#include "maintenance.h"
#include "update_storage.h"

#include "drives.h"
#include "app.h"
#include "i18n.h"
#include "insider.h"
#include "net.h"
#include "platform/platform.h"
#include "update_release.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

#define APP_DIR_DEFAULT "/data/homebrew/" NP_TITLE_ID

/* Workers own their request metadata. Only a mutex-protected value snapshot crosses
 * into UI; compatibility string accessors copy into main-thread-only buffers. */
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static UpdateStatus status;
typedef struct
{
    char version[32], zip_url[256], zip_sha256[65];
    uint64_t zip_size;
    bool test_build;
} Offer;
static Offer offer;
static char installation[MT_PATH_MAX];
static bool startup_pending;
static const char *app_dir(void);

bool update_init(const char *installed)
{
    startup_pending = true;
    return mt_canonical(installed ? installed : app_dir(), installation);
}
void update_poll(UpdateStatus *out)
{
    pthread_mutex_lock(&status_lock);
    *out = status;
    pthread_mutex_unlock(&status_lock);
}
int update_state(void)
{
    pthread_mutex_lock(&status_lock);
    if (status.state == UPDATE_AVAILABLE && status.test_build && !app.settings.update_test_builds)
        status.state = UPDATE_NONE;
    int state = status.state;
    pthread_mutex_unlock(&status_lock);
    return state;
}
const char *update_version(void)
{
    static char text[32];
    UpdateStatus s;
    update_poll(&s);
    str_copy(text, sizeof(text), s.version);
    return text;
}
const char *update_message(void)
{
    static char text[256];
    UpdateStatus s;
    update_poll(&s);
    str_copy(text, sizeof(text), s.message);
    return text;
}
bool update_test_build(void)
{
    UpdateStatus s;
    update_poll(&s);
    return s.test_build;
}
uint64_t update_size(void)
{
    UpdateStatus s;
    update_poll(&s);
    return s.size;
}
bool update_can_retry(void)
{
    UpdateStatus s;
    update_poll(&s);
    return s.state == UPDATE_FAILED && s.can_retry;
}
void update_progress(uint64_t *done, uint64_t *total)
{
    UpdateStatus s;
    update_poll(&s);
    *done = s.done;
    *total = s.total;
}
static void fail(const char *why)
{
    pthread_mutex_lock(&status_lock);
    str_copy(status.message, sizeof(status.message), why);
    status.state = UPDATE_FAILED;
    pthread_mutex_unlock(&status_lock);
    np_log("update: %s", why);
}
static void publish_offer(const char *version, const char *url, const char *sha, uint64_t bytes,
                          bool test)
{
    pthread_mutex_lock(&status_lock);
    str_copy(offer.version, sizeof(offer.version), version);
    str_copy(offer.zip_url, sizeof(offer.zip_url), url);
    str_copy(offer.zip_sha256, sizeof(offer.zip_sha256), sha);
    offer.zip_size = bytes;
    offer.test_build = test;
    status = (UpdateStatus){.state = UPDATE_AVAILABLE, .size = bytes, .test_build = test};
    str_copy(status.version, sizeof(status.version), version);
    pthread_mutex_unlock(&status_lock);
}
static void no_offer(void)
{
    pthread_mutex_lock(&status_lock);
    status.state = UPDATE_NONE;
    pthread_mutex_unlock(&status_lock);
}
bool update_recover_startup(void *unused, bool *restart, char *error, size_t size)
{
    (void)unused;
    bool ok = update_storage_recover(installation, restart, error, size);
    pthread_mutex_lock(&status_lock);
    startup_pending = !ok;
    if (*restart || !ok)
    {
        status = (UpdateStatus){.state = ok ? UPDATE_INSTALLED : UPDATE_FAILED,
                                .phase = UPDATE_RECOVERING};
        str_copy(status.message, sizeof(status.message),
                 ok ? "Update recovery finished. Close and reopen Dulcamara before playing."
                    : error);
    }
    pthread_mutex_unlock(&status_lock);
    return ok;
}

/* test_builds_on: not 0 when Settings > System > Test builds was on as the check started */
static int check_thread(void *test_builds_on)
{
    const bool test_builds = (uintptr_t)test_builds_on != 0;
    char version[32] = "", zip_url[256] = "", zip_sha256[65] = "";
    uint64_t zip_size = 0;
    /* with Test builds on and an insider code, nightly's insider build first (insider.h); anything
     * else: GitHub */
    if (insider_first(test_builds, version, sizeof(version), zip_url, sizeof(zip_url), zip_sha256,
                      &zip_size))
    {
        publish_offer(version, zip_url, zip_sha256, zip_size, true);
        return 0;
    }
    char *body = NULL;
    size_t len = 0;
    /* releases/latest: the newest release that is neither a draft nor a pre-release. With Test
     * builds on, the 20 most recent releases, pre-releases among them, as a list. */
    int status =
        test_builds
            ? net_request(UPDATE_LIST_URL, NULL, NULL, NP_NAME "/" NP_VERSION, &body, &len)
            : net_request(UPDATE_LATEST_URL, NULL, NULL, NP_NAME "/" NP_VERSION, &body, &len);
    if (status == 404)
    {
        free(body);
        fail("No release found (the repository may be private)");
        return 0;
    }
    if (status != 200 || !body)
    {
        free(body);
        fail("Couldn't reach GitHub");
        return 0;
    }
    Release release;
    ReleaseVerdict verdict = test_builds
                                 ? release_evaluate_list(body, len, NP_VERSION, true, &release)
                                 : release_evaluate(body, len, NP_VERSION, &release);
    free(body);
    /* which was asked: "latest", or "releases (test builds on)" */
    np_log("update: " UPDATE_REPO " %s %s, this %s: %s",
           test_builds ? "releases (test builds on)" : "latest",
           release.version[0] ? release.version : "?", NP_VERSION, release_verdict_text(verdict));
    if (verdict != RELEASE_OFFER)
    {
        no_offer();
        return 0;
    }
    /* SHA256SUMS: the SHA-256 the download must have */
    status = net_request(release.sums_url, NULL, NULL, NP_NAME "/" NP_VERSION, &body, &len);
    if (status != 200 || !body)
    {
        free(body);
        fail("Couldn't reach GitHub");
        return 0;
    }
    char sha[65];
    int listed = release_sums_lookup(body, len, UPDATE_ZIP, sha);
    free(body);
    if (listed != 1 || (release.zip_digest[0] && strcmp(release.zip_digest, sha) != 0))
    {
        np_log("update: %s's " UPDATE_SUMS " %s; not offered", release.version,
               listed == 0  ? "doesn't list " UPDATE_ZIP
               : listed < 0 ? "has a malformed or repeated line for " UPDATE_ZIP
                            : "disagrees with GitHub's digest of " UPDATE_ZIP);
        no_offer();
        return 0;
    }
    str_copy(version, sizeof(version), release.version);
    str_copy(zip_url, sizeof(zip_url), release.zip_url);
    str_copy(zip_sha256, sizeof(zip_sha256), sha);
    zip_size = release.zip_size;
    np_log("update: %s%s available, " UPDATE_ZIP " sha256 %s", version,
           release.prerelease ? " (a test build)" : "", zip_sha256);
    publish_offer(version, zip_url, zip_sha256, zip_size, release.prerelease);
    return 0;
}

void update_check(void)
{
    update_state();
    pthread_mutex_lock(&status_lock);
    if (startup_pending || status.state == UPDATE_CHECKING || status.state == UPDATE_INSTALLING ||
        status.state == UPDATE_INSTALLED)
    {
        pthread_mutex_unlock(&status_lock);
        return;
    }
    status = (UpdateStatus){.state = UPDATE_CHECKING};
    pthread_mutex_unlock(&status_lock);
    SDL_Thread *t = SDL_CreateThread(check_thread, "update",
                                     (void *)(uintptr_t)app.settings.update_test_builds);
    if (t)
        SDL_DetachThread(t);
    else
        fail("Could not start update check");
}

/* Where the running app is stored. The update has to land there, whether it's in /data/homebrew or
 * on an extended or USB drive (/mnt/ext1/homebrew/PPSA75483 ...). The mount the app runs from
 * (/app0, or the ShadowMountPlus one) is matched to a folder on disk by its eboot.bin: same size
 * and modification time. */
static bool same_eboot(const char *dir, const struct stat *running)
{
    char file[NP_PATH_MAX];
    struct stat st;
    path_join(file, sizeof(file), dir, "eboot.bin");
    return stat(file, &st) == 0 && st.st_size == running->st_size &&
           st.st_mtime == running->st_mtime;
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
        if (have)
            str_copy(found, sizeof(found), running_dirs[i]);
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

static void on_progress(void *unused, uint64_t done, uint64_t total)
{
    (void)unused;
    pthread_mutex_lock(&status_lock);
    status.done = done;
    status.total = total;
    pthread_mutex_unlock(&status_lock);
}
static void phase(UpdatePhase value, const char *message)
{
    pthread_mutex_lock(&status_lock);
    status.phase = value;
    status.done = status.total = 0;
    str_copy(status.message, sizeof(status.message), message);
    pthread_mutex_unlock(&status_lock);
}
static void apply_progress(void *unused, MtPhase value, uint64_t done, uint64_t total)
{
    (void)unused;
    pthread_mutex_lock(&status_lock);
    status.phase = value == MT_APPLYING   ? UPDATE_APPLYING
                   : value == MT_ROLLBACK ? UPDATE_ROLLING_BACK
                                          : UPDATE_VERIFYING;
    status.done = done;
    status.total = total;
    pthread_mutex_unlock(&status_lock);
}

/* The cache's update files that aren't this version's go: update.zip and update.zip.part (0.2.12
 * and before), and update-<another version>.zip(.part), so that a download stopped half way is gone
 * on from only by its own version. */
static void forget_other_downloads(const char *dir, const char *keep)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, "update", 6) != 0 || !strstr(e->d_name, ".zip") ||
            !strncmp(e->d_name, keep, strlen(keep)))
            continue;
        char path[NP_PATH_MAX];
        path_join(path, sizeof(path), dir, e->d_name);
        if (remove(path) == 0)
            np_log("update: removed %s", path);
    }
    closedir(d);
}

static uint64_t size_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0 ? (uint64_t)st.st_size : 0;
}

#define DOWNLOAD_TRIES                                                                             \
    5 /* in a row, each going on from where the one before stopped, while each gets further */

typedef struct
{
    Offer offer;
    char root[NP_PATH_MAX], installed[MT_PATH_MAX];
} InstallJob;
static int install_thread(void *arg)
{
    InstallJob job = *(InstallJob *)arg;
    free(arg);
    const char *version = job.offer.version, *zip_sha256 = job.offer.zip_sha256;
    char *zip_url = job.offer.zip_url;
    const uint64_t zip_size = job.offer.zip_size;
    char dir[NP_PATH_MAX], name[64], zip[NP_PATH_MAX], part[NP_PATH_MAX + 8];
    path_join(dir, sizeof(dir), job.root, "cache");
    make_dirs(dir);
    snprintf(name, sizeof(name), "update-%s.zip", version);
    path_join(zip, sizeof(zip), dir, name);
    snprintf(part, sizeof(part), "%s.part", zip);
    forget_other_downloads(dir, name);
    insider_refresh_url(version, zip_sha256, zip_url, sizeof(job.offer.zip_url));
    char shown[160];
    np_log("update: downloading %s (%llu bytes) to %s",
           insider_loggable(zip_url, shown, sizeof(shown)), (unsigned long long)zip_size, zip);
    const uint64_t started = plat_ticks_us();
    const uint64_t had = size_of(part);
    char why[96] = "";
    NetResult result = NET_UNAVAILABLE;
    /* Include the remaining ZIP bytes before downloading. Staging later independently
     * checks the actual installation filesystem against fully expanded sizes. */
    if (zip_size > had && !mt_space(dir, zip_size - had))
    {
        fail("Not enough space for the update download");
        return 0;
    }
    for (int attempt = 1; attempt <= DOWNLOAD_TRIES; ++attempt)
    {
        const uint64_t before = size_of(part);
        result = net_download(zip_url, zip, on_progress, NULL, why, sizeof(why));
        const uint64_t after = size_of(part);
        if (result != NET_UNAVAILABLE || after <= before || attempt == DOWNLOAD_TRIES)
            break;
        /* the connection dropped or stalled after some of it came: go on from there, after a breath
         */
        np_log("update: the download stopped at %llu of %llu bytes (%s); going on (try %d of %d)",
               (unsigned long long)after, (unsigned long long)zip_size, why, attempt + 1,
               DOWNLOAD_TRIES);
        plat_sleep_us(2000000);
    }
    if (result != NET_OK)
    {
        char text[160];
        snprintf(text, sizeof(text), "The download failed: %s", why[0] ? why : "no reason given");
        fail(text);
        return 0;
    }
    const double took = (double)(plat_ticks_us() - started) / 1e6;
    const uint64_t fetched = zip_size > had ? zip_size - had : 0;
    np_log("update: downloaded in %.1f s (%.2f MB/s%s)", took,
           took > 0 ? (double)fetched / 1048576.0 / took : 0.0,
           had ? ", going on from an earlier try" : "");
    /* nothing is unpacked unless the ZIP is the one SHA256SUMS names */
    char got[65] = "";
    if (!insider_size_ok(zip_url, zip, zip_size) || !release_file_sha256(zip, got) ||
        strcmp(got, zip_sha256) != 0)
    {
        np_log("update: " UPDATE_ZIP " sha256 %s, " UPDATE_SUMS " says %s", got, zip_sha256);
        remove(zip);
        fail("The download doesn't match its SHA256SUMS");
        return 0;
    }
    phase(UPDATE_WAITING, "Downloaded. Waiting for the game and save maintenance to finish.");
    MtObserver observer = {.progress = apply_progress};
    char error[256];
    bool ok;
    for (;;)
    {
        while (!maintenance_ready())
            plat_sleep_us(100000);
        phase(UPDATE_VERIFYING, "Verifying and staging the complete update.");
        ok = update_storage_apply(zip, job.installed, version, &observer, error, sizeof(error));
        if (ok || strcmp(error, "Waiting for game and save maintenance to finish"))
            break;
        phase(UPDATE_WAITING, "Downloaded. Waiting for the game and save maintenance to finish.");
    }
    if (!ok)
    {
        /* Invalid ZIPs are redownloadable. Durable journals are preserved by storage.
         * If recovery/restart is required, offer an accurate close/reopen instruction. */
        remove(zip);
        pthread_mutex_lock(&status_lock);
        status.can_retry = maintenance_ready();
        if (!status.can_retry)
            startup_pending = true;
        pthread_mutex_unlock(&status_lock);
        fail(error);
        return 0;
    }
    remove(zip);
    pthread_mutex_lock(&status_lock);
    snprintf(status.message, sizeof(status.message),
             "Dulcamara %s is installed: restart Dulcamara to use it", version);
    status.state = UPDATE_INSTALLED;
    status.can_retry = false;
    pthread_mutex_unlock(&status_lock);
    np_log("update: installed %s transactionally", version);
    return 0;
}

void update_install(void)
{
    update_state();
    pthread_mutex_lock(&status_lock);
    if (startup_pending ||
        (status.state != UPDATE_AVAILABLE && !(status.state == UPDATE_FAILED && status.can_retry)))
    {
        pthread_mutex_unlock(&status_lock);
        return;
    }
    InstallJob *job = calloc(1, sizeof(*job));
    if (!job)
    {
        pthread_mutex_unlock(&status_lock);
        fail("Not enough memory to start update");
        return;
    }
    job->offer = offer;
    str_copy(job->root, sizeof(job->root), app.paths.root);
    str_copy(job->installed, sizeof(job->installed), installation);
    status.can_retry = true;
    status.done = 0;
    status.total = offer.zip_size;
    status.phase = UPDATE_DOWNLOADING;
    status.state = UPDATE_INSTALLING;
    status.message[0] = 0;
    pthread_mutex_unlock(&status_lock);
    SDL_Thread *t = SDL_CreateThread(install_thread, "update", job);
    if (t)
        SDL_DetachThread(t);
    else
    {
        free(job);
        fail("Could not start update download");
    }
}
