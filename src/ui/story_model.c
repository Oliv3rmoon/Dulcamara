/* Story mode's words and timing. SPDX-License-Identifier: GPL-3.0-or-later */
#include "story_model.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static size_t utf8_count(const char *s)
{
    size_t n = 0;
    for (; *s; ++s)
        n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

StorySlots story_slots_state(bool list_ok, const char *error)
{
    if (list_ok)
        return STORY_SLOTS_LISTED;
    return error && error[0] ? STORY_SLOTS_FAILED : STORY_SLOTS_LOOKING;
}

void story_line_say(StoryLine *line, const char *text, bool still)
{
    if (strcmp(line->text, text))
    {
        snprintf(line->text, sizeof(line->text), "%s", text);
        line->total = utf8_count(line->text);
        line->shown = 0;
    }
    if (still)
        story_line_finish(line);
}

void story_line_tick(StoryLine *line, float dt, bool still)
{
    if (still)
    {
        story_line_finish(line);
        return;
    }
    if (dt > 0)
        line->shown += dt * STORY_TYPE_RATE;
    if (line->shown > (float)line->total)
        line->shown = (float)line->total;
}

void story_line_finish(StoryLine *line)
{
    line->shown = (float)line->total;
}

bool story_line_done(const StoryLine *line)
{
    return line->shown >= (float)line->total;
}

size_t story_line_visible_bytes(const StoryLine *line)
{
    size_t chars = (size_t)line->shown, n = 0, i = 0;
    for (; line->text[i]; ++i)
        if (((unsigned char)line->text[i] & 0xc0) != 0x80 && n++ == chars)
            break;
    return i;
}

int story_choice_move(int current, int delta, int count)
{
    if (count <= 0)
        return 0;
    return ((current + delta) % count + count) % count;
}

void story_level(char *out, size_t size, uint64_t playtime_seconds, long long last_played)
{
    if (!playtime_seconds && !last_played)
    {
        snprintf(out, size, "New");
        return;
    }
    uint64_t level = (playtime_seconds + 1800) / 3600;
    snprintf(out, size, "Lv %llu", (unsigned long long)(level ? level : 1));
}

void story_duration(char *out, size_t size, uint64_t seconds)
{
    const uint64_t hours = seconds / 3600, minutes = seconds % 3600 / 60;
    if (hours)
        snprintf(out, size, "%llu h %llu m", (unsigned long long)hours, (unsigned long long)minutes);
    else if (minutes)
        snprintf(out, size, "%llu m", (unsigned long long)minutes);
    else
        snprintf(out, size, "under a minute");
}

static long long local_day(long long t)
{
    time_t s = (time_t)t;
    struct tm tm;
    if (!localtime_r(&s, &tm))
        return t / 86400;
    /* days since a fixed point, from the local calendar date */
    long long y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
}

void story_when(char *out, size_t size, long long when, long long now)
{
    const long long days = local_day(now) - local_day(when);
    if (days <= 0)
        snprintf(out, size, "today");
    else if (days == 1)
        snprintf(out, size, "yesterday");
    else if (days < 7)
        snprintf(out, size, "%lld days ago", days);
    else
    {
        time_t s = (time_t)when;
        struct tm tm;
        char date[32];
        if (localtime_r(&s, &tm) && strftime(date, sizeof(date), "%d %b %Y", &tm))
            snprintf(out, size, "on %s", date[0] == '0' ? date + 1 : date);
        else
            snprintf(out, size, "%lld days ago", days);
    }
}

void story_home_line(char *out, size_t size, const char *title, uint64_t playtime_seconds, long long last_played,
                     long long now)
{
    char time_text[48], when[48];
    story_duration(time_text, sizeof(time_text), playtime_seconds);
    if (last_played)
        story_when(when, sizeof(when), last_played, now);
    if (playtime_seconds >= 60 && last_played)
        snprintf(out, size, "\"%s.\" You are %s in, and you last played %s. What will you do?", title, time_text,
                 when);
    else if (playtime_seconds >= 60)
        snprintf(out, size, "\"%s.\" You are %s in. What will you do?", title, time_text);
    else if (last_played)
        snprintf(out, size, "\"%s.\" You last played it %s. What will you do?", title, when);
    else
        snprintf(out, size, "\"%s.\" You have not started this one yet. What will you do?", title);
}

void story_slot_stamp(char *out, size_t size, uint64_t created_ns, bool year)
{
    time_t t = (time_t)(created_ns / UINT64_C(1000000000));
    struct tm tm;
    char buf[48];
    if (!localtime_r(&t, &tm) ||
        !strftime(buf, sizeof(buf), year ? "%d %b %Y %H:%M:%S" : "%d %b \xC2\xB7 %H:%M:%S", &tm))
    {
        snprintf(out, size, "unknown date");
        return;
    }
    snprintf(out, size, "%s", buf[0] == '0' ? buf + 1 : buf);
}

bool story_chapter_card(bool game, bool stopping, bool after_stop, bool failed)
{
    return game && !stopping && !after_stop && !failed;
}

void story_chapter_lines(char *kicker, size_t kicker_size, char *where, size_t where_size, const char *engine,
                         uint64_t playtime_seconds, long long last_played, long long now)
{
    char time_text[48], when[48], upper[96];
    snprintf(upper, sizeof(upper), "%s", engine);
    for (char *c = upper; *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 'a' + 'A');
    story_duration(time_text, sizeof(time_text), playtime_seconds);
    if (last_played)
        story_when(when, sizeof(when), last_played, now);
    if (playtime_seconds >= 60)
    {
        char t[48];
        snprintf(t, sizeof(t), "%s", time_text);
        for (char *c = t; *c; ++c)
            if (*c >= 'a' && *c <= 'z')
                *c = (char)(*c - 'a' + 'A');
        snprintf(kicker, kicker_size, "%s \xC2\xB7 %s PLAYED", upper, t);
        if (last_played)
            snprintf(where, where_size, "You left off %s in. Last played %s.", time_text, when);
        else
            snprintf(where, where_size, "You left off %s in.", time_text);
    }
    else if (last_played)
    {
        snprintf(kicker, kicker_size, "%s \xC2\xB7 BACK AGAIN", upper);
        snprintf(where, where_size, "Last played %s.", when);
    }
    else
    {
        snprintf(kicker, kicker_size, "%s \xC2\xB7 FIRST TIME", upper);
        snprintf(where, where_size, "You have not played this one yet.");
    }
}

void story_plain(char *out, size_t size, const char *data)
{
    size_t n = 0;
    if (!size)
        return;
    for (; *data; ++data)
    {
        const char c = *data == TEXT_LITERAL ? '?' : *data;
        if (n + (c == '{' ? 2 : 1) >= size)
            break;
        if (c == '{')
            out[n++] = TEXT_LITERAL;
        out[n++] = c;
    }
    out[n] = 0;
}

size_t story_visible_cut(const char *text, size_t cut)
{
    for (size_t i = cut; i-- > 0;)
    {
        if (text[i] == '}' || (text[i] == '{' && i > 0 && text[i - 1] == TEXT_LITERAL))
            break;
        if (text[i] == '{')
            return i;
    }
    return cut;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

void story_not_shown_line(char *out, size_t size, const LibrarySkip *skipped, int count, const char *log_path)
{
    size_t used = (size_t)snprintf(out, size, count == 1 ? "1 folder was not shown." : "%d folders were not shown.",
                                   count);
    for (int i = 0; i < count && i < 2 && used < size; ++i)
    {
        const char *reason = skipped[i].reason;
        const bool phrase = !strncmp(reason, "a ", 2) || !strncmp(reason, "an ", 3);
        char name[300];
        story_plain(name, sizeof(name), base_name(skipped[i].path));
        used += (size_t)snprintf(out + used, size - used, phrase ? " \"%s\" is %s." : " \"%s\": %s.", name, reason);
    }
    if (used < size)
    {
        char log[600];
        story_plain(log, sizeof(log), log_path);
        snprintf(out + used, size - used, count > 2 ? " The others, with their reasons, are in %s."
                                                    : " Each one is in %s.",
                 log);
    }
}

static size_t next_char(const char *s, size_t i)
{
    if (!s[i])
        return i;
    if (s[i] == TEXT_LITERAL && s[i + 1] == '{')
        return i + 2; /* a marked brace is one character: a line never ends between them */
    ++i;
    while (((unsigned char)s[i] & 0xc0) == 0x80)
        ++i;
    return i;
}

int story_wrap(const char *text, float max_width, StoryMeasure measure, void *context, size_t *starts,
               size_t *lengths, int max_lines, bool *overflow)
{
    int lines = 0;
    size_t at = 0;
    *overflow = false;
    while (text[at] == ' ')
        ++at;
    while (text[at])
    {
        if (lines == max_lines)
        {
            *overflow = true;
            break;
        }
        /* the longest run of whole words that fits */
        size_t end = at, best = 0;
        while (text[end])
        {
            size_t word = end;
            while (text[word] == ' ')
                ++word;
            while (text[word] && text[word] != ' ')
                word = next_char(text, word);
            if (measure(text + at, word - at, context) > max_width)
                break;
            best = word - at;
            end = word;
            if (!text[end])
                break;
        }
        if (!best)
        {
            /* one word wider than the line: as many characters as fit, at least one */
            size_t cut = next_char(text, at);
            for (size_t next = next_char(text, cut);
                 text[cut] && text[cut] != ' ' && measure(text + at, next - at, context) <= max_width;
                 next = next_char(text, next))
                cut = next;
            best = cut - at;
        }
        starts[lines] = at;
        lengths[lines] = best;
        ++lines;
        at += best;
        while (text[at] == ' ')
            ++at;
    }
    return lines;
}

uint32_t story_tint(uint32_t c)
{
    const int r = (int)(c >> 16 & 0xff), g = (int)(c >> 8 & 0xff), b = (int)(c & 0xff);
    int hi = r > g ? r : g, lo = r < g ? r : g;
    hi = hi > b ? hi : b;
    lo = lo < b ? lo : b;
    /* dark, or grey (little saturation): Eclipse red */
    if (!(c >> 24) || hi < 80 || (hi - lo) * 100 < hi * 30)
        return STORY_RED;
    /* lifted so the brightest channel reads on black, the hue kept */
    const int scale = 230 * 256 / hi;
    const int rr = r * scale / 256, gg = g * scale / 256, bb = b * scale / 256;
    return 0xff000000u | (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
}
