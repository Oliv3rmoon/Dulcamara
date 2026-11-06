/*
 * SwanStationPS5 - minimal HTTPS download (libcurl).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PS5: PacBrew's libcurl plus the boilerplate's console_curl helpers (system
 * resolver, certificate list, non-blocking sockets). Desktop: system libcurl.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(NP_HAVE_CURL)
#include <curl/curl.h>
#if defined(__PROSPERO__)
#include "console_curl.h"
#endif

static size_t write_cb(void *data, size_t size, size_t count, void *user)
{
    return fwrite(data, size, count, (FILE *)user) * size;
}

bool net_available(void)
{
    static int state = -1;
    if (state < 0)
        state = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return state == 1;
}

typedef struct
{
    NetProgress progress;
    void *user;
    uint64_t offset; /* the bytes already in the .part file this transfer adds to */
} Progress;

static int progress_cb(void *user, curl_off_t total, curl_off_t done, curl_off_t up_total, curl_off_t up_done)
{
    (void)up_total;
    (void)up_done;
    const Progress *p = user;
    p->progress(p->user, p->offset + (done > 0 ? (uint64_t)done : 0), total > 0 ? p->offset + (uint64_t)total : 0);
    return 0;
}

static void say(char *why, size_t why_size, const char *text)
{
    if (why && why_size)
        snprintf(why, why_size, "%s", text);
}

static uint64_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0 ? (uint64_t)st.st_size : 0;
}

NetResult net_download(const char *url, const char *dest, NetProgress progress, void *user, char *why,
                       size_t why_size)
{
    say(why, why_size, "");
    if (!net_available())
    {
        say(why, why_size, "no network library");
        return NET_UNAVAILABLE;
    }
    char part[NP_PATH_MAX];
    snprintf(part, sizeof(part), "%s.part", dest);
    /* A second pass only to start over, when the server can't go on from where the .part file stops. */
    for (int pass = 0; pass < 2; ++pass)
    {
        const uint64_t have = file_size(part);
        FILE *f = fopen(part, have ? "ab" : "wb");
        if (!f)
        {
            np_log("net: can't write %s", part);
            say(why, why_size, "couldn't write the file");
            return NET_UNAVAILABLE;
        }
        CURL *easy = curl_easy_init();
        if (!easy)
        {
            fclose(f);
            say(why, why_size, "no network library");
            return NET_UNAVAILABLE;
        }
#if defined(__PROSPERO__)
        console_curl_setup(easy);
#else
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
#endif
        char error[CURL_ERROR_SIZE] = "";
        Progress p = {progress, user, have};
        curl_easy_setopt(easy, CURLOPT_URL, url);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 8L);
        /* No limit on the whole download, which can take many minutes on a slow connection: it is given up only when
         * it stays under NET_STALL_BYTES a second for NET_STALL_SECONDS. */
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, (long)NET_STALL_BYTES);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, (long)NET_STALL_SECONDS);
        curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L); /* an error page is never written into the file */
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, NP_NAME "/" NP_VERSION);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, f);
        if (have)
        {
            /* goes on from where the last try stopped (an HTTP Range request) */
            curl_easy_setopt(easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have);
            np_log("net: %s: going on from %llu bytes", url, (unsigned long long)have);
        }
        if (progress)
        {
            curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress_cb);
            curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &p);
            curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        }

        CURLcode rc = curl_easy_perform(easy);
        long status = 0;
        curl_off_t received = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &received);
        curl_easy_cleanup(easy);
        const bool written = fclose(f) == 0;

        if (rc == CURLE_OK && (status == 200 || status == 206) && written && rename(part, dest) == 0)
            return NET_OK;
        if (have && (rc == CURLE_RANGE_ERROR || status == 416))
        {
            /* the server sends the whole file or nothing from there: from the start */
            np_log("net: %s can't go on from %llu bytes (curl %d, http %ld): from the start", url,
                   (unsigned long long)have, (int)rc, status);
            remove(part);
            continue;
        }
        np_log("net: %s -> curl %d (%s), http %ld, %lld bytes received, %llu kept", url, (int)rc,
               error[0] ? error : curl_easy_strerror(rc), status, (long long)received,
               (unsigned long long)file_size(part));
        char text[96];
        if (rc == CURLE_OPERATION_TIMEDOUT && (received > 0 || have > 0))
            snprintf(text, sizeof(text), "the connection stalled (under %d KB/s for %d seconds)",
                     NET_STALL_BYTES / 1024, NET_STALL_SECONDS);
        else if (rc == CURLE_OPERATION_TIMEDOUT)
            snprintf(text, sizeof(text), "the server didn't answer in time");
        else if (rc == CURLE_HTTP_RETURNED_ERROR || (rc == CURLE_OK && status != 200 && status != 206))
            snprintf(text, sizeof(text), "the server answered %ld", status);
        else if (rc == CURLE_WRITE_ERROR || rc == CURLE_OK)
            snprintf(text, sizeof(text), "couldn't write the file (is the storage full?)");
        else
            snprintf(text, sizeof(text), "%s", curl_easy_strerror(rc));
        say(why, why_size, text);
        /* What arrived stays in the .part file for the next try to go on from, unless it can't be used: an answer
         * that isn't the file, or a file that couldn't be written. */
        if (rc == CURLE_HTTP_RETURNED_ERROR || rc == CURLE_WRITE_ERROR || rc == CURLE_OK || !written)
            remove(part);
        if (status == 404)
            return NET_NOT_FOUND;
        return rc == CURLE_OK || rc == CURLE_HTTP_RETURNED_ERROR ? NET_NOT_FOUND : NET_UNAVAILABLE;
    }
    say(why, why_size, "the server can't send the file from where it stopped");
    return NET_UNAVAILABLE;
}

typedef struct
{
    char *data;
    size_t length, capacity;
} Buffer;

static size_t buffer_cb(void *data, size_t size, size_t count, void *user)
{
    Buffer *b = user;
    size_t n = size * count;
    if (b->length + n + 1 > b->capacity)
    {
        size_t cap = (b->length + n + 1) * 2;
        char *grown = realloc(b->data, cap);
        if (!grown)
            return 0;
        b->data = grown;
        b->capacity = cap;
    }
    memcpy(b->data + b->length, data, n);
    b->length += n;
    b->data[b->length] = '\0';
    return n;
}

int net_request(const char *url, const char *post_data, const char *content_type,
                const char *user_agent, char **body, size_t *body_length)
{
    *body = NULL;
    *body_length = 0;
    if (!net_available())
        return -1;
    CURL *easy = curl_easy_init();
    if (!easy)
        return -1;
#if defined(__PROSPERO__)
    console_curl_setup(easy);
#else
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
#endif
    Buffer b = {0};
    struct curl_slist *headers = NULL;
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent ? user_agent : NP_NAME "/" NP_VERSION);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, buffer_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &b);
    if (post_data)
    {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, post_data);
        if (content_type)
        {
            char header[160];
            snprintf(header, sizeof(header), "Content-Type: %s", content_type);
            headers = curl_slist_append(headers, header);
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
        }
    }
    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(easy);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK)
    {
        np_log("net: %s -> curl %d", url, (int)rc);
        free(b.data);
        return -1;
    }
    *body = b.data;
    *body_length = b.length;
    return (int)status;
}

#else

bool net_available(void)
{
    return false;
}

NetResult net_download(const char *url, const char *dest, NetProgress progress, void *user, char *why,
                       size_t why_size)
{
    (void)url;
    (void)dest;
    (void)progress;
    (void)user;
    if (why && why_size)
        snprintf(why, why_size, "%s", "built without a network library");
    return NET_UNAVAILABLE;
}

int net_request(const char *url, const char *post_data, const char *content_type,
                const char *user_agent, char **body, size_t *body_length)
{
    (void)url;
    (void)post_data;
    (void)content_type;
    (void)user_agent;
    *body = NULL;
    *body_length = 0;
    return -1;
}

#endif
