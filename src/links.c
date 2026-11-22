/*
 * Dulcamara - a game added from a direct link (links.h, docs/game-links.md): one link from start to end, the start's
 * cleanup, links.txt and its done file, and the background queue.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "links.h"
#include "link_internal.h"
#include "maintenance.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- shared by the link files */

bool link_fail(LinkResult *r, LinkError error, const char *fmt, ...)
{
    r->error = error;
    va_list arguments;
    va_start(arguments, fmt);
    vsnprintf(r->message, sizeof(r->message), fmt, arguments);
    va_end(arguments);
    return false;
}

void link_progress(const LinkObserver *o, LinkStage stage, uint64_t done, uint64_t total)
{
    if (o && o->progress)
        o->progress(o->context, stage, done, total);
}

bool link_cancelled(const LinkObserver *o)
{
    return o && o->cancelled && o->cancelled(o->context);
}

/* Removes path and all below it. A link is removed itself, never followed. */
bool link_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    DIR *d = opendir(path);
    if (!d)
        return false;
    bool ok = true;
    for (;;)
    {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e)
        {
            ok = ok && errno == 0;
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char child[MT_PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
        {
            ok = false;
            continue;
        }
        ok = link_remove_tree(child) && ok;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

const char *link_url_host(const char *url)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    for (size_t i = strcspn(host, "/?#"); i-- > 0;)
        if (host[i] == '@')
            return host + i + 1;
    return host;
}

/* url with any user:password@ taken out (what links-done.txt records) */
static void url_without_login(const char *url, char *out, size_t size)
{
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    snprintf(out, size, "%.*s%s", (int)(start - url), url, link_url_host(url));
}

void link_url_for_log(const char *url, char *out, size_t size)
{
    /* scheme://host/.../file: never a user:password@, nor the query, which may hold a token */
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *host = link_url_host(url);
    const size_t host_len = strcspn(host, "/?#");
    const char *path = host + host_len;
    const size_t path_len = strcspn(path, "?#");
    const char *last = path;
    for (const char *p = path; p < path + path_len; ++p)
        if (*p == '/')
            last = p;
    snprintf(out, size, "%.*s%.*s%s%.*s%s", (int)(start - url), url, (int)host_len, host,
             last > path ? "/..." : "", (int)(path + path_len - last), last, path[path_len] ? "?..." : "");
}

/* ---------------------------------------------------------------- one link */

bool link_error_permanent(LinkError e)
{
    return e == LINK_BAD_URL || e == LINK_HTTP || e == LINK_WEB_PAGE || e == LINK_FORMAT || e == LINK_UNSAFE ||
           e == LINK_DAMAGED;
}

bool link_url_valid(const char *url)
{
    const size_t n = url ? strlen(url) : 0;
    size_t scheme;
    if (n >= LINKS_URL_MAX)
        return false;
    if (!strncasecmp(url, "https://", 8))
        scheme = 8;
    else if (!strncasecmp(url, "http://", 7))
        scheme = 7;
    else
        return false;
    for (const unsigned char *c = (const unsigned char *)url; *c; ++c)
        if (*c <= 0x20 || *c == 0x7f)
            return false;
    const size_t host = strcspn(url + scheme, "/?#");
    return host > 0 && url[scheme] != '@' && url[scheme] != ':';
}

static void url_tag(const char *url, char out[17])
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *s = (const unsigned char *)url; *s; ++s)
        h = (h ^ *s) * 1099511628211ull;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* the staging folder, made when missing; something else in its place (a file, a link) is removed, not followed */
static bool staging_dir(const char *games, char *staging, size_t size)
{
    if (snprintf(staging, size, "%s/" LINKS_STAGING, games) >= (int)size)
        return false;
    struct stat st;
    if (!lstat(staging, &st) && !S_ISDIR(st.st_mode) && unlink(staging))
        return false;
    if (mkdir(games, 0755) && errno != EEXIST)
        return false;
    if (mkdir(staging, 0755) && errno != EEXIST)
        return false;
    return !lstat(staging, &st) && S_ISDIR(st.st_mode);
}

bool link_add(const char *games_in, const char *url, const LinkObserver *observer, LinkResult *r)
{
    memset(r, 0, sizeof(*r));
    if (!link_url_valid(url))
        return link_fail(r, LINK_BAD_URL,
                         "Not a link Dulcamara can download: a direct link starts with https:// or http://.");
    char games[MT_PATH_MAX], staging[MT_PATH_MAX], zip[MT_PATH_MAX], part[MT_PATH_MAX], tag[17], shown[160];
    if (!games_in || !mt_canonical(games_in, games) || strlen(games) + 2 >= NP_PATH_MAX)
        return link_fail(r, LINK_IO, "The games folder can't be used.");
    if (!maintenance_acquire(MAINT_LINKS))
        return link_fail(r, LINK_BUSY,
                         "Not now: a game is running, or a backup, restore or update is working (or Dulcamara must "
                         "restart after an update). Close the game or wait, then try again.");
    link_url_for_log(url, shown, sizeof(shown));
    url_tag(url, tag);
    bool ok = staging_dir(games, staging, sizeof(staging));
    if (!ok)
        link_fail(r, LINK_IO, "Couldn't make the download folder in the games folder (is /data unlocked?).");
    if (ok && (snprintf(zip, sizeof(zip), "%s/%s.zip", staging, tag) >= (int)sizeof(zip) ||
               snprintf(part, sizeof(part), "%s.part", zip) >= (int)sizeof(part)))
        ok = link_fail(r, LINK_IO, "The games folder's path is too long.");
    char name[256] = "";
    if (ok)
    {
        np_log("links: %s: downloading", shown);
        link_progress(observer, LINK_STAGE_DOWNLOAD, 0, 0);
        ok = link_download(url, zip, games, observer, r, name, sizeof(name));
        if (ok)
        {
            np_log("links: %s: %llu bytes; checking", shown, (unsigned long long)r->downloaded);
            ok = link_unpack(games, staging, zip, tag, name, observer, r);
            if (!ok && r->error == LINK_DAMAGED && r->resumed)
            {
                /* the file may have changed on the server between the tries: a passing failure, and the archive
                 * (removed below) is not gone on from */
                np_log("links: %s: %s (after going on from an earlier transfer)", shown, r->message);
                link_fail(r, LINK_NETWORK,
                          "The download was damaged (the file may have changed on the server while it was resumed). "
                          "It will be tried again from the start.");
            }
        }
        unlink(zip);
        /* what arrived stays only when a passing failure may go on from it (until the app closes) */
        struct stat st;
        if (!ok && (r->error != LINK_NETWORK && r->error != LINK_STALLED && r->error != LINK_CONNECT))
            unlink(part);
        else if (!lstat(part, &st) && st.st_size == 0)
            unlink(part);
        rmdir(staging); /* only when nothing is left in it */
    }
    if (ok)
        np_log("links: %s: added %s", shown, r->folder);
    else
        np_log("links: %s: %s", shown, r->message);
    maintenance_release(MAINT_LINKS, false);
    return ok;
}

bool links_install_shelf(const char *games_in, const char *asset, LinkResult *r)
{
    memset(r, 0, sizeof(*r));
    char games[MT_PATH_MAX], staging[MT_PATH_MAX];
    if (!mt_canonical(games_in, games) ||
        snprintf(r->folder, sizeof(r->folder), "%s/Dulcamara itch.io Shelf", games) >= (int)sizeof(r->folder))
        return link_fail(r, LINK_IO, "The games folder can't be used.");
    if (!maintenance_acquire(MAINT_LINKS))
        return link_fail(r, LINK_BUSY, "The library is busy. Try again after the current operation finishes.");

    struct stat st;
    bool ok;
    if (!lstat(r->folder, &st))
        ok = link_fail(r, LINK_IO, "Dulcamara itch.io Shelf already exists in your games folder.");
    else if (errno != ENOENT || !staging_dir(games, staging, sizeof(staging)))
        ok = link_fail(r, LINK_IO, "Couldn't create the shelf in the games folder.");
    else {
        ok = link_unpack(games, staging, asset, "bundled-shelf", "Dulcamara itch.io Shelf", NULL, r);
        rmdir(staging);
    }
    maintenance_release(MAINT_LINKS, false);
    return ok;
}

bool links_cleanup(const char *games)
{
    char staging[MT_PATH_MAX];
    if (snprintf(staging, sizeof(staging), "%s/" LINKS_STAGING, games) >= (int)sizeof(staging))
        return false;
    struct stat st;
    if (lstat(staging, &st))
        return errno == ENOENT;
    np_log("links: removing what an interrupted download left in %s", staging);
    return link_remove_tree(staging);
}

int links_find_game(const Library *lib, const char *folder, char *reason, size_t reason_size)
{
    const size_t n = strlen(folder);
    for (int i = 0; i < lib->count; ++i)
    {
        const char *at = lib->games[i].location;
        if (!strncmp(at, folder, n) && (at[n] == '\0' || at[n] == '/'))
            return i;
    }
    for (int i = 0; i < lib->skipped_count; ++i)
    {
        const char *at = lib->skipped[i].path;
        if (!strncmp(at, folder, n) && (at[n] == '\0' || at[n] == '/'))
        {
            snprintf(reason, reason_size, "%s", lib->skipped[i].reason);
            return -1;
        }
    }
    snprintf(reason, reason_size, "no RPG Maker 2000/2003 or Ren'Py game was found in it");
    return -1;
}

/* ---------------------------------------------------------------- links.txt */

/* path's bytes, at most limit; regular_only: a link in path's place is not followed, and only a file is read */
static char *read_small(const char *path, size_t limit, size_t *length, bool *too_big, bool regular_only)
{
    *length = 0;
    *too_big = false;
    const int fd = open(path, O_RDONLY | (regular_only ? O_NOFOLLOW : 0));
    struct stat st;
    if (fd >= 0 && regular_only && (fstat(fd, &st) || !S_ISREG(st.st_mode)))
    {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    FILE *f = fd >= 0 ? fdopen(fd, "rb") : NULL;
    if (!f)
    {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    char *data = malloc(limit + 1);
    size_t n = data ? fread(data, 1, limit + 1, f) : 0;
    const bool failed = !data || ferror(f);
    fclose(f);
    if (failed)
    {
        free(data);
        return NULL;
    }
    if (n > limit)
    {
        *too_big = true;
        free(data);
        return NULL;
    }
    data[n] = '\0';
    *length = n;
    return data;
}

/* each line, trimmed of spaces, tabs and a CR, into line (NUL-terminated in place); returns the next line */
static char *next_line(char *p, char **line)
{
    char *end = strchr(p, '\n');
    char *next = end ? end + 1 : p + strlen(p);
    if (end)
        *end = '\0';
    while (*p == ' ' || *p == '\t')
        ++p;
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t'))
        p[--n] = '\0';
    *line = p;
    return next;
}

static bool done_has(const char *done, const char *url)
{
    if (!done)
        return false;
    const size_t n = strlen(url);
    for (const char *p = done; (p = strstr(p, url)); p += n)
        if (p > done && p[-1] == '\t' && (p[n] == '\n' || p[n] == '\r' || p[n] == '\0'))
            return true;
    return false;
}

int links_pending(const char *links_file, const char *done_file, char (*urls)[LINKS_URL_MAX], int max,
                  int *invalid, char *why, size_t why_size)
{
    if (invalid)
        *invalid = 0;
    if (why_size)
        why[0] = '\0';
    size_t length;
    bool too_big;
    char *text = read_small(links_file, LINKS_FILE_MAX, &length, &too_big, false);
    if (!text)
    {
        if (why_size)
            snprintf(why, why_size, too_big ? "links.txt is larger than 256 KiB"
                                    : errno == ENOENT ? "there is no links.txt"
                                                      : "links.txt can't be read");
        return -1;
    }
    size_t done_length;
    bool done_big;
    char *done = read_small(done_file, 16u * 1024 * 1024, &done_length, &done_big, false);
    int count = 0;
    char *p = text;
    if (!strncmp(p, "\xEF\xBB\xBF", 3))
        p += 3;
    while (*p && count < max)
    {
        char *line;
        p = next_line(p, &line);
        if (!line[0] || line[0] == '#')
            continue;
        if (!link_url_valid(line))
        {
            if (invalid)
                ++*invalid;
            continue;
        }
        char recorded[LINKS_URL_MAX];
        url_without_login(line, recorded, sizeof(recorded));
        bool seen = done_has(done, line) || done_has(done, recorded);
        for (int i = 0; i < count && !seen; ++i)
            seen = !strcmp(urls[i], line);
        if (!seen)
            snprintf(urls[count++], LINKS_URL_MAX, "%s", line);
    }
    free(done);
    free(text);
    return count;
}

bool links_mark(const char *done_file, const char *url_in, const char *outcome)
{
    char path[MT_PATH_MAX], url[LINKS_URL_MAX];
    url_without_login(url_in, url, sizeof(url));
    /* its folder made canonical, never the file itself: a link in its place is refused, not followed (and the
     * rename below would replace the link, never its target) */
    const char *slash = strrchr(done_file, '/');
    char folder[MT_PATH_MAX];
    if (!slash || !slash[1] || snprintf(folder, sizeof(folder), "%.*s", (int)(slash - done_file), done_file) < 0 ||
        !mt_canonical(folder[0] ? folder : "/", path) ||
        strlen(path) + strlen(slash) >= sizeof(path))
        return false;
    strcat(path, slash);
    struct stat st;
    const bool exists = !lstat(path, &st);
    if (exists ? !S_ISREG(st.st_mode) : errno != ENOENT)
    {
        np_log("links: %s is not a regular file (a link or a folder?) or can't be checked: not changed", path);
        return false;
    }
    size_t length = 0;
    bool too_big = false;
    char *old = exists ? read_small(path, 16u * 1024 * 1024, &length, &too_big, true) : NULL;
    if (exists && !old)
    {
        /* a done file that is there but can't be read is kept as it is: its history is not lost */
        np_log("links: %s can't be read (%s): not changed", path, too_big ? "over 16 MiB" : strerror(errno));
        return false;
    }
    const size_t add = strlen(outcome) + strlen(url) + 3;
    char *data = malloc(length + add + 1);
    if (!data)
    {
        free(old);
        return false;
    }
    if (length)
        memcpy(data, old, length);
    if (length && data[length - 1] != '\n')
        data[length++] = '\n';
    const int n = snprintf(data + length, add + 1, "%s\t%s\n", outcome, url);
    free(old);
    const bool ok = n > 0 && mt_atomic(path, data, length + (size_t)n);
    free(data);
    return ok;
}

/* ---------------------------------------------------------------- the background queue */

typedef struct
{
    char games[MT_PATH_MAX];
    char done[MT_PATH_MAX];
    int count;
    char (*urls)[LINKS_URL_MAX];
} Job;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static LinksStatus status;
static bool cancel_requested, worker_running;

static bool job_cancelled(void *context)
{
    (void)context;
    pthread_mutex_lock(&lock);
    const bool c = cancel_requested;
    pthread_mutex_unlock(&lock);
    return c;
}

static void job_progress(void *context, LinkStage stage, uint64_t done, uint64_t total)
{
    (void)context;
    pthread_mutex_lock(&lock);
    status.stage = stage;
    status.done = done;
    status.total = total;
    pthread_mutex_unlock(&lock);
}

static void *job_main(void *argument)
{
    Job *job = argument;
    const LinkObserver observer = {job_cancelled, job_progress, NULL};
    bool cancelled = false;
    for (int i = 0; i < job->count && !cancelled; ++i)
    {
        pthread_mutex_lock(&lock);
        status.index = i;
        status.stage = LINK_STAGE_DOWNLOAD;
        status.done = status.total = 0;
        cancelled = cancel_requested;
        pthread_mutex_unlock(&lock);
        if (cancelled)
            break;
        LinkResult *r = malloc(sizeof(*r));
        if (!r)
            break;
        link_add(job->games, job->urls[i], &observer, r);
        cancelled = r->error == LINK_CANCELLED;
        if (job->done[0] && (r->error == LINK_OK || link_error_permanent(r->error)) &&
            !links_mark(job->done, job->urls[i], r->error == LINK_OK ? "added" : "failed"))
            np_log("links: couldn't record the link in %s", job->done);
        pthread_mutex_lock(&lock);
        if (i < LINKS_RESULTS_MAX)
            status.results[i] = *r;
        if (r->error == LINK_OK)
        {
            ++status.added;
            str_copy(status.last_folder, sizeof(status.last_folder), r->folder);
        }
        else if (!cancelled)
            ++status.failed;
        pthread_mutex_unlock(&lock);
        free(r);
    }
    pthread_mutex_lock(&lock);
    status.state = cancelled ? LINKS_CANCELLED : LINKS_FINISHED;
    worker_running = false;
    pthread_mutex_unlock(&lock);
    free(job->urls);
    free(job);
    return NULL;
}

bool links_start(const char *games, const char *done_file, const char (*urls)[LINKS_URL_MAX], int count)
{
    if (count <= 0 || count > LINKS_PER_RUN)
        return false;
    Job *job = calloc(1, sizeof(*job));
    char(*copy)[LINKS_URL_MAX] = calloc((size_t)count, LINKS_URL_MAX);
    if (!job || !copy)
    {
        free(job);
        free(copy);
        return false;
    }
    str_copy(job->games, sizeof(job->games), games);
    str_copy(job->done, sizeof(job->done), done_file ? done_file : "");
    memcpy(copy, urls, (size_t)count * LINKS_URL_MAX);
    job->urls = copy;
    job->count = count;
    pthread_mutex_lock(&lock);
    if (worker_running || status.state == LINKS_RUNNING)
    {
        pthread_mutex_unlock(&lock);
        free(copy);
        free(job);
        return false;
    }
    memset(&status, 0, sizeof(status));
    status.state = LINKS_RUNNING;
    status.count = count;
    cancel_requested = false;
    worker_running = true;
    pthread_attr_t attributes;
    pthread_t thread;
    bool started = pthread_attr_init(&attributes) == 0;
    if (started)
    {
        pthread_attr_setstacksize(&attributes, 1024 * 1024); /* libcurl, TLS and miniz: more than a small default */
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&thread, &attributes, job_main, job) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (!started)
    {
        memset(&status, 0, sizeof(status));
        worker_running = false;
    }
    pthread_mutex_unlock(&lock);
    if (!started)
    {
        free(copy);
        free(job);
    }
    return started;
}

void links_poll(LinksStatus *out)
{
    pthread_mutex_lock(&lock);
    *out = status;
    pthread_mutex_unlock(&lock);
}

void links_cancel(void)
{
    pthread_mutex_lock(&lock);
    if (status.state == LINKS_RUNNING)
        cancel_requested = true;
    pthread_mutex_unlock(&lock);
}

void links_acknowledge(void)
{
    pthread_mutex_lock(&lock);
    if (!worker_running && (status.state == LINKS_FINISHED || status.state == LINKS_CANCELLED))
        memset(&status, 0, sizeof(status));
    pthread_mutex_unlock(&lock);
}
