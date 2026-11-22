/* Production update jobs and prompt/consent. Network/draw boundaries are deterministic;
 * the installer, release evaluator, keyboard, insider storage and popup are real. */
/* The offered release: newer than NP_VERSION, set by tests/test_update_production.py from the release metadata. */
#ifndef OFFERED_VERSION
#error "build with -DOFFERED_VERSION=\"X.Y.Z\""
#endif
#include "app.h"
#include "update.h"
#include "update_release.h"
#include "maintenance.h"
#include "net.h"
#include "insider.h"
#include "ui/update_popup.h"
#include "ui/insider_prompt.h"
#include "ui/draw.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/sfx.h"
#include "platform/platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
App app;
Theme theme;
static const char *source_zip;
static char digest[65];
static uint64_t bytes;
static atomic_int downloads;
static float panel_y;
static uint32_t backdrop;
static int cleanup_fault, cleanup_failures, apply_fault, apply_failures;
static char transaction_phase[MT_PATH_MAX];
int update_test_unlink(const char *path)
{
    size_t n = strlen(path);
    if (cleanup_fault && n >= 4 && !strcmp(path + n - 4, ".old"))
    {
        /* cleanup() runs after the durable commit and its observer notification. */
        uint32_t phase[4];
        FILE *f = fopen(transaction_phase, "rb");
        assert(f && fread(phase, sizeof(phase), 1, f) == 1 && !fclose(f));
        assert(phase[0] == 0x45504853u && phase[1] == MT_COMMITTED &&
               phase[2] == ~phase[1] && !phase[3]);
        ++cleanup_failures;
        if (cleanup_fault == 1)
            cleanup_fault = 0;
        errno = EIO;
        return -1;
    }
    return unlink(path);
}
int update_test_rename(const char *a, const char *b)
{
    if (apply_fault && strstr(a, ".eclipse-stage") && strstr(b, "eboot.bin"))
    {
        apply_fault = 0;
        ++apply_failures;
        errno = EIO;
        return -1;
    }
    return rename(a, b);
}
uint64_t plat_ticks_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void plat_sleep_us(uint32_t us)
{
    struct timespec t = {us / 1000000, (long)(us % 1000000) * 1000};
    nanosleep(&t, NULL);
}
int plat_width(void)
{
    return 1920;
}
int plat_height(void)
{
    return 1080;
}
int app_nav_pad(void)
{
    return 0;
}
void app_toast(const char *s)
{
    (void)s;
}
void app_toast_for(const char *s, float seconds)
{
    (void)s;
    (void)seconds;
}
void app_save_settings(void)
{
}
void sfx_play(Sfx s)
{
    (void)s;
}
void text_language_changed(void)
{
}
void draw_rect(float x, float y, float w, float h, uint32_t c)
{
    if (x == 0 && y == 0 && w == 1920 && h == 1080)
        backdrop = c;
}
void draw_rrect(float x, float y, float w, float h, float r, uint32_t c)
{
    (void)x;
    (void)r;
    (void)c;
    if (w == 900 && h == 560)
        panel_y = y;
}
void draw_rrect_outline(float x, float y, float w, float h, float r, float t, uint32_t c)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)r;
    (void)t;
    (void)c;
}
void draw_glow(float x, float y, float w, float h, float s, uint32_t c)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)s;
    (void)c;
}
void draw_set_glyph_alpha(float a)
{
    (void)a;
}
float draw_hint(float x, float y, enum PadGlyph g, const char *s, float n, uint32_t c)
{
    (void)x;
    (void)y;
    (void)g;
    (void)s;
    (void)n;
    (void)c;
    return 1;
}
float draw_hint_combo(float x, float y, const int *g, int n, char sep, const char *s, float f,
                      uint32_t c)
{
    (void)x;
    (void)y;
    (void)g;
    (void)n;
    (void)sep;
    (void)s;
    (void)f;
    (void)c;
    return 1;
}
void icon_draw(int i, float x, float y, float s, uint32_t c)
{
    (void)i;
    (void)x;
    (void)y;
    (void)s;
    (void)c;
}
void text_draw(float x, float y, float s, int w, uint32_t c, int a, const char *t)
{
    (void)x;
    (void)y;
    (void)s;
    (void)w;
    (void)c;
    (void)a;
    (void)t;
}
void text_draw_fit(float x, float y, float s, int w, uint32_t c, int a, float max, const char *t)
{
    (void)max;
    text_draw(x, y, s, w, c, a, t);
}
uint32_t argb_alpha(uint32_t c, float a)
{
    return (c & 0xffffff) | ((uint32_t)((c >> 24) * a) << 24);
}
void anim_approach(float *v, float target, float dt, float speed)
{
    (void)dt;
    (void)speed;
    *v = target;
}
int net_request_within(const char *u, const char *p, const char *ct, const char *agent,
                       long seconds, bool follow, char **body, size_t *len)
{
    (void)u;
    (void)p;
    (void)ct;
    (void)agent;
    (void)seconds;
    (void)follow;
    *body = NULL;
    *len = 0;
    return 503;
}
int net_request(const char *url, const char *post, const char *ct, const char *agent, char **body,
                size_t *len)
{
    (void)post;
    (void)ct;
    (void)agent;
    char json[4096];
    if (strstr(url, "SHA256SUMS"))
        snprintf(json, sizeof(json), "%s  PPSA75483.zip\n", digest);
    else
        snprintf(json, sizeof(json),
                 "{\"tag_name\":\"" OFFERED_VERSION "\",\"draft\":false,\"prerelease\":false,\"assets\":[{"
                 "\"name\":\"PPSA75483.zip\",\"state\":\"uploaded\",\"size\":%llu,\"browser_"
                 "download_url\":\"https://github.com/Oliv3rmoon/Dulcamara/releases/download/" OFFERED_VERSION "/"
                 "PPSA75483.zip\"},{\"name\":\"SHA256SUMS\",\"state\":\"uploaded\",\"size\":100,"
                 "\"browser_download_url\":\"https://github.com/Oliv3rmoon/Dulcamara/releases/"
                 "download/" OFFERED_VERSION "/SHA256SUMS\"}]}",
                 (unsigned long long)bytes);
    *body = strdup(json);
    *len = strlen(json);
    return 200;
}
NetResult net_download(const char *url, const char *dest, NetProgress progress, void *ctx,
                       char *why, size_t size)
{
    (void)url;
    (void)why;
    (void)size;
    atomic_fetch_add(&downloads, 1);
    assert(mt_copy(source_zip, dest, NULL));
    progress(ctx, bytes, bytes);
    return NET_OK;
}
static void wait_state(int state)
{
    uint64_t until = plat_ticks_us() + 10000000;
    while (update_state() != state && plat_ticks_us() < until)
        plat_sleep_us(1000);
    assert(update_state() == state);
}
static void click(int button)
{
    uint32_t p = BIT(button);
    assert(update_popup_input(&p));
    assert(!p);
}
static void consent(void)
{
    char path[NP_PATH_MAX];
    path_join(path, sizeof(path), app.paths.root, "insider-code");
    insider_prompt_open();
    for (int i = 0; i < 8; ++i)
        assert(insider_prompt_input(BIT(BTN_CROSS)));
    assert(insider_prompt_input(BIT(BTN_CIRCLE)));
    assert(access(path, F_OK));
    insider_prompt_open();
    for (int i = 0; i < 8; ++i)
        assert(insider_prompt_input(BIT(BTN_CROSS)));
    assert(insider_prompt_input(BIT(BTN_START)));
    FILE *f = fopen(path, "rb");
    assert(f);
    char b[32] = {0};
    assert(fread(b, 1, sizeof(b), f) == 9);
    fclose(f);
    assert(!strcmp(b, "QQQQQQQQ\n"));
    insider_prompt_open();
    insider_prompt_input(BIT(BTN_START));
    assert(!insider_has_code());
    puts("PASS production insider cancel/save/explicit empty-code consent");
}
int main(int argc, char **argv)
{
    assert(argc == 5);
    cleanup_fault = !strcmp(argv[4], "cleanup-once") ? 1
                    : !strcmp(argv[4], "cleanup-always") ? 2 : 0;
    apply_fault = !strcmp(argv[4], "apply-write");
    assert(mt_path(transaction_phase, argv[3], ".eclipse-update/transaction/phase"));
    source_zip = argv[2];
    strcpy(app.paths.root, argv[1]);
    app.game = -1;
    app.dt = 0;
    app.settings.still_background = true;
    struct stat st;
    assert(!stat(source_zip, &st));
    bytes = st.st_size;
    assert(release_file_sha256(source_zip, digest));
    assert(update_init(argv[3]));
    maintenance_recovery_begin(MAINT_RECOVER_SAVES | MAINT_RECOVER_UPDATE);
    maintenance_recovered(true);
    assert(maintenance_acquire(MAINT_RECOVERY));
    bool restart = false;
    char error[256];
    assert(update_recover_startup(NULL, &restart, error, sizeof(error)));
    maintenance_recovery_finish(MAINT_RECOVER_UPDATE, true);
    maintenance_release(MAINT_RECOVERY, restart);
    consent();
    update_check();
    wait_state(UPDATE_AVAILABLE);
    assert(!update_test_build());
    assert(!strcmp(update_version(), OFFERED_VERSION));
    update_popup_open();
    update_popup_draw();
    assert(panel_y == 260 && backdrop == 0xb0000000u);
    assert(!atomic_load(&downloads));
    click(BTN_RIGHT);
    click(BTN_CROSS);
    assert(!update_popup_is_open() && !atomic_load(&downloads));
    assert(maintenance_launch());
    update_popup_open();
    click(BTN_CROSS);
    wait_state(UPDATE_INSTALLING);
    UpdateStatus s;
    uint64_t until = plat_ticks_us() + 10000000;
    do
    {
        update_poll(&s);
        assert(!strcmp(s.version, OFFERED_VERSION) && s.size == bytes);
        plat_sleep_us(1000);
    } while (s.phase != UPDATE_WAITING && plat_ticks_us() < until);
    assert(s.phase == UPDATE_WAITING && atomic_load(&downloads) == 1);
    assert(!maintenance_ready());
    click(BTN_CIRCLE);
    assert(!update_popup_is_open());
    maintenance_runtime(false, false, false);
    assert(maintenance_acquire(MAINT_RESTORE));
    plat_sleep_us(200000);
    assert(update_state() == UPDATE_INSTALLING);
    maintenance_release(MAINT_RESTORE, false);
    if (!strcmp(argv[4], "cleanup-always") || !strcmp(argv[4], "apply-write"))
    {
        wait_state(UPDATE_FAILED);
        update_poll(&s);
        if (!strcmp(argv[4], "apply-write"))
        {
            assert(apply_failures == 1 && !cleanup_failures);
            assert(strstr(s.message, "original installation restored. Try again"));
            assert(s.can_retry && update_can_retry() && maintenance_ready());
            assert(maintenance_launch());
            puts("PASS precommit rollback: UPDATE_FAILED, original restored, retry and launch available");
            return 0;
        }
        assert(cleanup_failures == 2);
        assert(strstr(s.message, "Update recovery pending"));
        assert(!strstr(s.message, "original installation restored") && !strstr(s.message, "Try again"));
        assert(!s.can_retry && !update_can_retry() && !maintenance_restart_ready());
        assert(!maintenance_launch());
        assert(maintenance_acquire(MAINT_RECOVERY));
        assert(!update_recover_startup(NULL, &restart, error, sizeof(error)) && restart);
        maintenance_recovery_finish(MAINT_RECOVER_UPDATE, false);
        maintenance_release(MAINT_RECOVERY, restart);
        assert(cleanup_failures == 3 && update_state() == UPDATE_FAILED);
        assert(!maintenance_launch() && !maintenance_restart_ready());
        cleanup_fault = 0;
        assert(maintenance_acquire(MAINT_RECOVERY));
        assert(update_recover_startup(NULL, &restart, error, sizeof(error)) && restart);
        maintenance_recovery_finish(MAINT_RECOVER_UPDATE, true);
        maintenance_release(MAINT_RECOVERY, restart);
        update_poll(&s);
        assert(s.state == UPDATE_INSTALLED && !s.can_retry);
        assert(strstr(s.message, "Close and reopen"));
        assert(!maintenance_launch() && maintenance_restart_ready());
        puts("PASS committed persistent cleanup: pending UPDATE_FAILED, retry recovery still locked, "
             "successful recovery UPDATE_INSTALLED/restart required");
        return 0;
    }
    wait_state(UPDATE_INSTALLED);
    update_poll(&s);
    assert(!s.can_retry && !update_can_retry());
    assert(strstr(s.message, "is installed: restart Dulcamara"));
    assert(!strstr(s.message, "original installation restored"));
    assert(cleanup_failures == (!strcmp(argv[4], "cleanup-once") ? 1 : 0));
    assert(!maintenance_launch() && maintenance_restart_ready());
    if (cleanup_failures)
        puts("PASS committed transient cleanup: UPDATE_INSTALLED, restart required, launch locked");
    puts("PASS ordinary offer of the next release (" OFFERED_VERSION "), Later/consent, reduced-motion first frame, "
         "independent download, runtime/restore exclusion, install/restart");
    return 0;
}
