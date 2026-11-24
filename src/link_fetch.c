/*
 * Dulcamara - a link's download (links.h): libcurl as the updater's net.c uses it, in a function of its own so the
 * updater's behaviour does not change. Redirects are followed (at most LINKS_MAX_REDIRECTS, http and https only), a
 * stall rather than a time limit ends it, a transfer that stops after data came goes on with a Range request, the
 * first bytes must be a ZIP's (a web page, RAR, 7-Zip, an installer... stop it at once), and the free space is
 * checked as soon as the size is known and every 16 MiB.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if !defined(__PROSPERO__)
#define _XOPEN_SOURCE 700
#endif
#define _DARWIN_C_SOURCE 1
#include "link_internal.h"
#include "maintenance.h"
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#if defined(__PROSPERO__)
#include <sys/mount.h>
#endif
#include <unistd.h>

#if defined(NP_HAVE_CURL)
#include <curl/curl.h>
#if defined(__PROSPERO__)
#include "console_curl.h"
#endif

#define HEAD_BYTES 512
#define SPACE_EVERY (16ull << 20)

typedef struct
{
    CURL *easy;
    const LinkObserver *observer;
    const char *space_root;
    int fd;
    uint64_t have;     /* in the .part file before this transfer */
    uint64_t received; /* this transfer */
    uint64_t total;    /* the whole file, once the server says (else 0) */
    uint64_t expected; /* the whole file as an earlier try of this download said (else 0) */
    long code;         /* the answer's HTTP status */
    uint64_t next_space;
    uint64_t reported;
    bool sized, stop, cancelled, no_space, write_error;
    bool range_bad; /* a 206 whose Content-Range does not go on from have, or of another or unknown total */
    int verdict; /* 0 not known yet, 1 a ZIP, -1 refused (refusal/why) */
    LinkError refusal;
    char why[256];
    unsigned char head[HEAD_BYTES];
    size_t head_length;
    char name[256]; /* Content-Disposition's file name */
} Fetch;

static uint64_t free_bytes(const char *path)
{
#if defined(__PROSPERO__)
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct statfs fs;
    memset(&fs, 0, sizeof(fs));
    extern int link_fetch_ps5_fstatfs(int, struct statfs *) __asm__("_fstatfs");
    int rc = link_fetch_ps5_fstatfs(fd, &fs);
    close(fd);
    return rc == 0 && fs.f_bavail > 0 ? (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize : 0;
#else
    struct statvfs s;
    if (statvfs(path, &s))
        return 0;
    return (uint64_t)s.f_bavail * (s.f_frsize ? s.f_frsize : s.f_bsize);
#endif
}

static bool space_for(Fetch *f, uint64_t bytes)
{
    if (mt_space(f->space_root, bytes + LINKS_RESERVE))
        return true;
    f->no_space = true;
    snprintf(f->why, sizeof(f->why),
             "Not enough free space: the download needs %.0f MB more and %.0f MB are free. Free some space and try "
             "again.",
             (double)(bytes + LINKS_RESERVE) / 1048576.0, (double)free_bytes(f->space_root) / 1048576.0);
    return false;
}

static void refuse(Fetch *f, LinkError e, const char *why)
{
    f->verdict = -1;
    f->refusal = e;
    snprintf(f->why, sizeof(f->why), "%s", why);
}

/* What the first bytes are. final: no more are coming. */
static void decide(Fetch *f, bool final)
{
    const unsigned char *h = f->head;
    const size_t n = f->head_length;
    if (n >= 4 && !memcmp(h, "PK\003\004", 4))
    {
        f->verdict = 1;
        return;
    }
    size_t i = n >= 3 && !memcmp(h, "\xEF\xBB\xBF", 3) ? 3 : 0;
    while (i < n && (h[i] == ' ' || h[i] == '\t' || h[i] == '\r' || h[i] == '\n'))
        ++i;
    const char *type = NULL;
    if (f->easy)
        curl_easy_getinfo(f->easy, CURLINFO_CONTENT_TYPE, &type);
    const bool html_type = type && (!strncasecmp(type, "text/html", 9) || !strncasecmp(type, "application/xhtml", 17));
    if ((i < n && h[i] == '<') || (html_type && n >= 4))
    {
        refuse(f, LINK_WEB_PAGE,
               "The link opened a web page, not a file. Open it on a phone or PC and copy the page's direct "
               "download link (the one that starts the .zip download itself).");
        return;
    }
    if (n < 8 && !final)
        return;
    static const struct
    {
        const char *magic;
        size_t length;
        LinkError error;
        const char *why;
    } KNOWN[] = {
        {"PK\005\006", 4, LINK_FORMAT, "The ZIP is empty: there is no game in it."},
        {"Rar!\x1a\x07", 6, LINK_FORMAT,
         "This is a RAR archive. Dulcamara opens only ZIP: extract it on a PC and copy the game's folder to games/ "
         "over FTP, or use a .zip link."},
        {"7z\xBC\xAF\x27\x1C", 6, LINK_FORMAT,
         "This is a 7-Zip archive. Dulcamara opens only ZIP: extract it on a PC and copy the game's folder to games/ "
         "over FTP, or use a .zip link."},
        {"\x1f\x8b", 2, LINK_FORMAT,
         "This is a gzip file (.gz, .tar.gz). Dulcamara opens only ZIP: extract it on a PC and copy the game's "
         "folder to games/ over FTP, or use a .zip link."},
        {"\xFD" "7zXZ", 5, LINK_FORMAT,
         "This is an xz file (.xz, .tar.xz). Dulcamara opens only ZIP: extract it on a PC and copy the game's "
         "folder to games/ over FTP, or use a .zip link."},
        {"BZh", 3, LINK_FORMAT,
         "This is a bzip2 file (.bz2, .tar.bz2). Dulcamara opens only ZIP: extract it on a PC and copy the game's "
         "folder to games/ over FTP, or use a .zip link."},
        {"\x28\xB5\x2F\xFD", 4, LINK_FORMAT,
         "This is a zstd file (.zst). Dulcamara opens only ZIP: extract it on a PC and copy the game's folder to "
         "games/ over FTP, or use a .zip link."},
        {"MZ", 2, LINK_FORMAT,
         "This is a Windows program (an installer or a self-extracting archive). Dulcamara can't run it: install or "
         "extract the game on a PC and copy its folder to games/ over FTP."},
        {"%PDF", 4, LINK_FORMAT, "This is a PDF document, not a game's ZIP."},
    };
    for (size_t k = 0; k < sizeof(KNOWN) / sizeof(KNOWN[0]); ++k)
        if (n >= KNOWN[k].length && !memcmp(h, KNOWN[k].magic, KNOWN[k].length))
        {
            refuse(f, KNOWN[k].error, KNOWN[k].why);
            return;
        }
    if (n < HEAD_BYTES && !final)
        return;
    if (n >= 262 && !memcmp(h + 257, "ustar", 5))
        refuse(f, LINK_FORMAT,
               "This is a tar archive. Dulcamara opens only ZIP: extract it on a PC and copy the game's folder to "
               "games/ over FTP, or use a .zip link.");
    else
        refuse(f, LINK_FORMAT, "The file is not a ZIP archive, the only kind Dulcamara opens.");
}

static const char *find_ci(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; ++hay)
        if (!strncasecmp(hay, needle, n))
            return hay;
    return NULL;
}

/* the value of p's %XY, or -1 unless both X and Y are hex digits (a trailing "%" or "%X" stays as it is) */
static int percent_byte(const char *p)
{
    static const char hex[] = "0123456789abcdef0123456789ABCDEF";
    if (p[0] != '%' || !p[1] || !p[2])
        return -1;
    const char *x = strchr(hex, p[1]), *y = strchr(hex, p[2]);
    if (!x || !y)
        return -1;
    return (int)((x - hex) & 15) << 4 | (int)((y - hex) & 15);
}

static size_t on_header(char *data, size_t size, size_t count, void *user)
{
    Fetch *f = user;
    const size_t n = size * count;
    if (n >= 5 && !strncasecmp(data, "HTTP/", 5))
    {
        f->name[0] = '\0'; /* a new answer (after a redirect): its own name, if any */
        char line[64];
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), data);
        const char *space = strchr(line, ' ');
        f->code = space ? strtol(space + 1, NULL, 10) : 0;
    }
    if (f->have && f->code == 206 && n > 14 && !strncasecmp(data, "Content-Range:", 14))
    {
        /* bytes <have>-<last>/<total>, the total the same as before when it was known: else from the start */
        char line[256];
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), data);
        unsigned long long first, last, total;
        if (sscanf(line + 14, " bytes %llu-%llu/%llu", &first, &last, &total) != 3 || first != f->have ||
            last < first || total <= last || (f->expected && total != f->expected))
        {
            f->range_bad = true;
            return 0; /* the transfer ends here, before any of its bytes are kept */
        }
    }
    if (n > 20 && !strncasecmp(data, "Content-Disposition:", 20))
    {
        char line[1024];
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), data);
        line[strcspn(line, "\r\n")] = '\0';
        /* filename*=UTF-8''name%20here (preferred), else filename="name" or filename=name */
        const char *star = find_ci(line, "filename*=");
        const char *plain = find_ci(line, "filename=");
        char *out = f->name;
        size_t o = 0;
        if (star)
        {
            const char *p = strstr(star, "''");
            p = p ? p + 2 : star + 10;
            for (; *p && *p != ';' && o + 1 < sizeof(f->name); ++p)
            {
                const int v = percent_byte(p);
                if (v >= 0)
                {
                    out[o++] = (char)v;
                    p += 2;
                }
                else
                    out[o++] = *p;
            }
        }
        else if (plain)
        {
            const char *p = plain + 9;
            const bool quoted = *p == '"';
            p += quoted;
            for (; *p && (quoted ? *p != '"' : (*p != ';' && *p != ' ')) && o + 1 < sizeof(f->name); ++p)
                out[o++] = *p;
        }
        out[o] = '\0';
    }
    return n;
}

static size_t on_data(char *data, size_t size, size_t count, void *user)
{
    Fetch *f = user;
    const size_t n = size * count;
    if (f->stop)
        return 0;
    if (link_cancelled(f->observer))
    {
        f->cancelled = f->stop = true;
        return 0;
    }
    if (!f->sized)
    {
        f->sized = true;
        curl_off_t length = -1;
        curl_easy_getinfo(f->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        if (length > 0)
            f->total = f->have + (uint64_t)length;
        if (f->total && !space_for(f, f->total - f->have))
        {
            f->stop = true;
            return 0;
        }
    }
    if (f->verdict == 0)
    {
        const size_t take = n < HEAD_BYTES - f->head_length ? n : HEAD_BYTES - f->head_length;
        memcpy(f->head + f->head_length, data, take);
        f->head_length += take;
        decide(f, false);
        if (f->verdict < 0)
        {
            f->stop = true;
            return 0;
        }
    }
    for (size_t at = 0; at < n;)
    {
        const ssize_t w = write(f->fd, data + at, n - at);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
        {
            if (errno == ENOSPC || errno == EDQUOT || errno == EFBIG)
            {
                f->no_space = true;
                snprintf(f->why, sizeof(f->why),
                         "The storage is full: the download stopped after %.0f MB. Free some space and try again.",
                         (double)(f->have + f->received + at) / 1048576.0);
            }
            else
                f->write_error = true;
            f->stop = true;
            return 0;
        }
        at += (size_t)w;
    }
    f->received += n;
    const uint64_t done = f->have + f->received;
    if (done >= f->next_space)
    {
        f->next_space = done + SPACE_EVERY;
        if (!space_for(f, f->total > done ? f->total - done : 0))
        {
            f->stop = true;
            return 0;
        }
    }
    return n;
}

static int on_progress(void *user, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
    (void)total;
    (void)up_total;
    (void)up_now;
    Fetch *f = user;
    if (link_cancelled(f->observer))
    {
        f->cancelled = true;
        return 1;
    }
    const uint64_t done = f->have + (now > 0 ? (uint64_t)now : 0);
    if (done != f->reported || !f->reported)
    {
        f->reported = done ? done : 1;
        link_progress(f->observer, LINK_STAGE_DOWNLOAD, done, f->total);
    }
    return 0;
}

static uint64_t size_of(const char *path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_size > 0 ? (uint64_t)st.st_size : 0;
}

/* The name a file has in the URL: its last part, without the query, percent-decoded. */
static void name_from_url(const char *url, char *out, size_t size)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    const char *path = host + strcspn(host, "/?#");
    const size_t path_length = strcspn(path, "?#");
    const char *last = path;
    for (const char *p = path; p < path + path_length; ++p)
        if (*p == '/')
            last = p + 1;
    size_t o = 0;
    for (const char *p = last; p < path + path_length && o + 1 < size; ++p)
    {
        const int v = p + 2 < path + path_length ? percent_byte(p) : -1;
        if (v >= 0)
        {
            out[o++] = (char)v;
            p += 2;
        }
        else
            out[o++] = *p;
    }
    out[o] = '\0';
}

static void host_of(const char *url, char *out, size_t size)
{
    const char *host = link_url_host(url); /* never a user:password@ */
    snprintf(out, size, "%.*s", (int)strcspn(host, "/?#"), host);
}

bool link_download(const char *url, const char *dest, const char *space_root, const LinkObserver *observer,
                   LinkResult *r, char *name, size_t name_size)
{
    char part[MT_PATH_MAX], host[256], shown[160];
    if (name && name_size)
        name[0] = '\0';
    host_of(url, host, sizeof(host));
    link_url_for_log(url, shown, sizeof(shown));
    if (snprintf(part, sizeof(part), "%s.part", dest) >= (int)sizeof(part))
        return link_fail(r, LINK_IO, "The download's path is too long.");
    if (!net_available())
        return link_fail(r, LINK_NETWORK, "No network library in this build.");
    bool restarted = false;
    uint64_t expected = 0; /* the file's size, once a try has said it */
    int resumes = 0;
    for (;;)
    {
        Fetch f;
        memset(&f, 0, sizeof(f));
        f.observer = observer;
        f.space_root = space_root;
        f.expected = expected;
        f.have = size_of(part);
        if (f.have)
        {
            /* an earlier try's bytes: still a ZIP's start, else from the start */
            int fd = open(part, O_RDONLY | O_NOFOLLOW);
            const ssize_t got = fd >= 0 ? read(fd, f.head, sizeof(f.head)) : -1;
            if (fd >= 0)
                close(fd);
            f.head_length = got > 0 ? (size_t)got : 0;
            decide(&f, true);
            if (f.verdict != 1)
            {
                unlink(part);
                memset(&f, 0, sizeof(f));
                f.observer = observer;
                f.space_root = space_root;
                f.expected = expected = 0;
            }
        }
        if (!space_for(&f, 0))
            return link_fail(r, LINK_NO_SPACE, "%s", f.why);
        f.next_space = f.have + SPACE_EVERY;
        f.fd = open(part, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
        if (f.fd < 0)
            return link_fail(r, LINK_IO, "Couldn't write the download in the games folder (is /data unlocked?).");
        f.easy = curl_easy_init();
        if (!f.easy)
        {
            close(f.fd);
            return link_fail(r, LINK_NETWORK, "The network library couldn't start.");
        }
#if defined(__PROSPERO__)
        console_curl_setup(f.easy);
#else
        curl_easy_setopt(f.easy, CURLOPT_NOSIGNAL, 1L);
#endif
        char error[CURL_ERROR_SIZE] = "";
        curl_easy_setopt(f.easy, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(f.easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(f.easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(f.easy, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(f.easy, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(f.easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(f.easy, CURLOPT_MAXREDIRS, (long)LINKS_MAX_REDIRECTS);
        curl_easy_setopt(f.easy, CURLOPT_CONNECTTIMEOUT, 15L);
        /* no limit on the whole download: given up only when it stays under LINKS_STALL_BYTES a second */
        curl_easy_setopt(f.easy, CURLOPT_LOW_SPEED_LIMIT, (long)LINKS_STALL_BYTES);
        curl_easy_setopt(f.easy, CURLOPT_LOW_SPEED_TIME, (long)LINKS_STALL_SECONDS);
        curl_easy_setopt(f.easy, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(f.easy, CURLOPT_FAILONERROR, 1L); /* an error page never lands in the file */
        curl_easy_setopt(f.easy, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(f.easy, CURLOPT_USERAGENT, NP_NAME "/" NP_VERSION);
        curl_easy_setopt(f.easy, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(f.easy, CURLOPT_HEADERDATA, &f);
        curl_easy_setopt(f.easy, CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(f.easy, CURLOPT_WRITEDATA, &f);
        curl_easy_setopt(f.easy, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(f.easy, CURLOPT_XFERINFODATA, &f);
        curl_easy_setopt(f.easy, CURLOPT_NOPROGRESS, 0L);
        if (f.have)
        {
            curl_easy_setopt(f.easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)f.have);
            np_log("links: %s: going on from %llu bytes", shown, (unsigned long long)f.have);
        }
        const CURLcode rc = curl_easy_perform(f.easy);
        long status = 0;
        curl_easy_getinfo(f.easy, CURLINFO_RESPONSE_CODE, &status);
        if (rc == CURLE_OK && f.verdict == 0)
            decide(&f, true); /* fewer than HEAD_BYTES came */
        if (name && name_size)
        {
            const char *effective = NULL;
            curl_easy_getinfo(f.easy, CURLINFO_EFFECTIVE_URL, &effective);
            if (f.name[0])
                snprintf(name, name_size, "%s", f.name);
            else if (!name[0])
                name_from_url(effective ? effective : url, name, name_size);
        }
        curl_easy_cleanup(f.easy);
        f.easy = NULL;
        if (fsync(f.fd) && errno != ENOSYS && errno != EINVAL && errno != ENOTSUP && rc == CURLE_OK)
            f.write_error = true;
        if (close(f.fd) && rc == CURLE_OK)
            f.write_error = true;
        const uint64_t done = f.have + f.received;
        link_progress(observer, LINK_STAGE_DOWNLOAD, done, f.total);

        if (f.total && !expected)
            expected = f.total;
        if (f.cancelled)
            return link_fail(r, LINK_CANCELLED, "Cancelled.");
        if (f.range_bad)
        {
            /* the server would not go on from the bytes kept (or the file changed): the whole file again */
            np_log("links: %s: the answer doesn't go on from %llu bytes of the same file: %s", shown,
                   (unsigned long long)f.have, restarted ? "given up" : "from the start");
            unlink(part);
            if (restarted)
                return link_fail(r, LINK_NETWORK,
                                 "The server couldn't go on with the download from where it stopped. Try again: it "
                                 "starts over.");
            restarted = true;
            expected = 0;
            continue;
        }
        if (f.no_space)
            return link_fail(r, LINK_NO_SPACE, "%s", f.why);
        if (f.verdict < 0)
            return link_fail(r, f.refusal, "%s", f.why);
        if (f.write_error)
            return link_fail(r, LINK_IO, "Couldn't write the download (is the storage full or read-only?).");
        if (rc == CURLE_OK && (status == 200 || status == 206) && f.verdict == 1)
        {
            if (rename(part, dest))
                return link_fail(r, LINK_IO, "Couldn't keep the download.");
            r->downloaded = done;
            r->resumed = f.have > 0;
            return true;
        }
        if (rc == CURLE_OK)
            return link_fail(r, LINK_HTTP, "The server answered HTTP %ld, not the file.", status);
        if (f.have && (rc == CURLE_RANGE_ERROR || status == 416) && !restarted)
        {
            /* the server can't go on from there: the whole file again */
            np_log("links: %s can't go on from %llu bytes (curl %d, http %ld): from the start", shown,
                   (unsigned long long)f.have, (int)rc, status);
            unlink(part);
            restarted = true;
            expected = 0;
            continue;
        }
        np_log("links: %s -> curl %d (%s), http %ld, %llu bytes this try, %llu kept", shown, (int)rc,
               error[0] ? error : curl_easy_strerror(rc), status, (unsigned long long)f.received,
               (unsigned long long)size_of(part));
        const bool dropped = rc == CURLE_PARTIAL_FILE || rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
                             rc == CURLE_GOT_NOTHING || rc == CURLE_OPERATION_TIMEDOUT;
        if (dropped && f.received > 0 && resumes < LINKS_RESUMES)
        {
            ++resumes;
            continue; /* it was going: on from where it stopped */
        }
        switch (rc)
        {
        case CURLE_COULDNT_RESOLVE_HOST:
            return link_fail(r, LINK_CONNECT, "Can't find the server %s: check the link and the console's network.",
                             host);
        case CURLE_COULDNT_CONNECT:
            return link_fail(r, LINK_CONNECT, "Can't connect to %s: check the network; the server may be down.",
                             host);
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_USE_SSL_FAILED:
            return link_fail(r, LINK_TLS,
                             "The secure connection (TLS) to %s failed: the console's date may be wrong, or the "
                             "server's certificate isn't trusted.",
                             host);
        case CURLE_TOO_MANY_REDIRECTS:
            return link_fail(r, LINK_HTTP, "Too many redirects (more than %d): the link goes round in circles.",
                             LINKS_MAX_REDIRECTS);
        case CURLE_UNSUPPORTED_PROTOCOL:
            return link_fail(r, LINK_BAD_URL, "The link led to something other than http:// or https://.");
        case CURLE_OPERATION_TIMEDOUT:
            if (!status)
                return link_fail(r, LINK_CONNECT, "%s didn't answer: check the network.", host);
            return link_fail(r, LINK_STALLED,
                             "The download stalled (under 1 KB/s for %d seconds). Try again: it goes on from where "
                             "it stopped.",
                             LINKS_STALL_SECONDS);
        case CURLE_HTTP_RETURNED_ERROR:
            if (status == 404 || status == 410)
                return link_fail(r, LINK_HTTP, "Not found on the server (HTTP %ld): check the link.", status);
            if (status == 401 || status == 403)
                return link_fail(r, LINK_HTTP,
                                 "The server refused the download (HTTP %ld): the link may have expired or need a "
                                 "login, which Dulcamara can't do.",
                                 status);
            if (status >= 500 || status == 429)
                return link_fail(r, LINK_NETWORK, "The server is busy or failing (HTTP %ld): try again later.",
                                 status);
            return link_fail(r, LINK_HTTP, "The server answered HTTP %ld.", status);
        default:
            return link_fail(r, LINK_NETWORK, "The download failed: %s. Try again: it goes on from where it stopped.",
                             curl_easy_strerror(rc));
        }
    }
}

#else

bool link_download(const char *url, const char *dest, const char *space_root, const LinkObserver *observer,
                   LinkResult *r, char *name, size_t name_size)
{
    (void)url;
    (void)dest;
    (void)space_root;
    (void)observer;
    if (name && name_size)
        name[0] = '\0';
    return link_fail(r, LINK_NETWORK, "This build has no network library.");
}

#endif
