/*
 * SwanStationPS5 disc daemon - a payload that stays resident (send it once to the console's ELF loader, port 9021).
 * A PS1 disc put in the USB CD drive while the app is closed starts SwanStationPS5, which finds the disc and asks
 * what to do with it (play it from the disc, copy it first, or start its game if it is installed already).
 *
 * The disc code is the app's own (src/discimport.c, src/cdscsi.c, src/disc.c), compiled in with a small SDL
 * replacement (payload/shim). The way it reaches the drive and starts the app follows the disc
 * daemon of PS5SX2 (ps5/disclaunch/disc_launch.c by swordpdf, GPL-3.0-or-later) and ps-disc-detect (Al-Azif).
 * While the app is open it looks after the drive itself, and the daemon does not touch it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "SwanStationPS5.h"
#include "cdscsi.h"
#include "disceta.h"
#include "discimport.h"
#include "drives.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define TITLE_ID "PPSA98510"
#define ROOT "/data/SwanStationPS5"
#define CONFIG ROOT "/SwanStationPS5.ini"
#define APP_ALIVE ROOT "/app-alive" /* the app writes the time in here every couple of seconds */
#define LAUNCH_MARK ROOT "/daemon-launch" /* tells the app that this daemon opened it, so it keeps quiet at first */
#define PID_FILE ROOT "/disc-daemon.pid"
#define LOG_FILE ROOT "/logs/disc-daemon.log"

enum
{
    kPollMs = 500,
    kSettleSec = 2,     /* a disc just put in: the system's own screen is on for a moment */
    kAliveWithin = 8,   /* seconds since the app's last sign of life */
    kAfterCloseSec = 5  /* after the app closes, the disc that is in is not news */
};

/* ------------------------------------------------------------------ texts: the app's five languages */

enum Msg
{
    M_READY,
    M_COUNT
};

/* en, fr, pt, es, ja - the same order as the app's language setting (src/i18n.h) */
static const char *const TEXT[M_COUNT][5] = {
    [M_READY] = {"SSPS5 ready...", "SSPS5 prêt...", "SSPS5 pronto...", "SSPS5 listo...", "SSPS5 準備完了..."},
};

static int language; /* the app's setting, read from its ini */

static void load_language(void)
{
    FILE *f = fopen(CONFIG, "r");
    char line[256];
    language = 0;
    while (f && fgets(line, sizeof(line), f))
        if (strncmp(line, "language=", 9) == 0)
        {
            int v = atoi(line + 9);
            language = v >= 0 && v < 5 ? v : 0;
        }
    if (f)
        fclose(f);
}

/* ------------------------------------------------------------------ notifications and log */

typedef struct
{
    char unused[45];
    char message[3075];
} notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size, int32_t blocking);

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_printf("[SwanStationPS5 disc] %s\n", buf);
    SwanStationPS5_log("daemon: %s", buf);
}

static void notify(enum Msg m, ...)
{
    static notify_req_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, m);
    vsnprintf(req.message, sizeof(req.message), TEXT[m][language], ap);
    va_end(ap);
    int rc = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    say("notification (rc %d): %s", rc, req.message);
}

/* ------------------------------------------------------------------ the app: alive, and started */

static bool stamp_fresh(const char *path, int within)
{
    FILE *f = fopen(path, "r");
    long long t = 0;
    if (!f)
        return false;
    int got = fscanf(f, "%lld", &t);
    fclose(f);
    long long now = (long long)time(NULL);
    return got == 1 && now >= t && now - t <= within;
}

static void stamp(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%lld\n", (long long)time(NULL));
        fclose(f);
    }
}

typedef struct
{
    uint32_t size;
    int user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint64_t check_flag;
} lnc_app_param_t;
typedef int (*LaunchApp_t)(const char *, const char **, lnc_app_param_t *);
typedef int (*UsrInit_t)(void *);
typedef int (*UsrFg_t)(int *);

static LaunchApp_t p_launch;
static UsrFg_t p_fguser;

static void load_launcher(void)
{
    void *h = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (h)
        p_launch = (LaunchApp_t)dlsym(h, "sceLncUtilLaunchApp");
    void *u = dlopen("libSceUserService.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (u)
    {
        UsrInit_t init = (UsrInit_t)dlsym(u, "sceUserServiceInitialize");
        p_fguser = (UsrFg_t)dlsym(u, "sceUserServiceGetForegroundUser");
        if (init)
            say("sceUserServiceInitialize = 0x%08x", (unsigned)init(NULL));
    }
    say("launcher: %s", p_launch ? "sceLncUtilLaunchApp found" : "sceLncUtilLaunchApp NOT found");
}

/* Starts the app the way its tile does. Never fork + LoadExec: it panicked the kernel for PS5SX2. */
static void launch_app(void)
{
    if (!p_launch)
        return;
    int user = -1;
    if (p_fguser)
        p_fguser(&user);
    lnc_app_param_t prm;
    memset(&prm, 0, sizeof(prm));
    prm.size = sizeof(prm);
    prm.user_id = user;
    stamp(LAUNCH_MARK);
    int rc = p_launch(TITLE_ID, NULL, &prm);
    say("sceLncUtilLaunchApp(" TITLE_ID ", user %d) = 0x%08x", user, (unsigned)rc);
}

/* ------------------------------------------------------------------ one daemon at a time */

static void take_over(void)
{
    FILE *f = fopen(PID_FILE, "r");
    int old = 0;
    if (f)
    {
        if (fscanf(f, "%d", &old) == 1 && old > 0 && old != (int)getpid())
        {
            /* only a process that is still this daemon: the comm of the process is the loader's */
            int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, old};
            static unsigned char buf[8192];
            size_t len = sizeof(buf);
            memset(buf, 0, sizeof(buf));
            if (sysctl(mib, 4, buf, &len, NULL, 0) == 0 && len > 0)
            {
                const struct kinfo_proc *kp = (const struct kinfo_proc *)buf;
                if (kp->ki_pid == old && (strstr(kp->ki_comm, "payload") || strstr(kp->ki_comm, "disc") ||
                                          strstr(kp->ki_comm, "Disc")))
                {
                    say("the previous daemon (pid %d, %s) is stopped", old, kp->ki_comm);
                    kill((pid_t)old, SIGKILL);
                }
            }
        }
        fclose(f);
    }
    f = fopen(PID_FILE, "w");
    if (f)
    {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

static bool superseded(void)
{
    FILE *f = fopen(PID_FILE, "r");
    int p = 0;
    if (!f)
        return false;
    int got = fscanf(f, "%d", &p);
    fclose(f);
    return got == 1 && p > 0 && p != (int)getpid();
}

/* ------------------------------------------------------------------ a disc */

static void handle_disc(const DiscId *id)
{
    say("a disc: '%s' (%s)", id->title, id->key);
    sleep(kSettleSec); /* the system's own screen for a disc it does not know, before anything of ours */
    say("the app is opened: it finds the disc in the drive and asks what to do with it");
    launch_app();
}

int main(void)
{
    /* /dev/cd* and notifications, whatever the loader gave us */
    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    kernel_set_ucred_authid(getpid(), 0x4800000000010003ull);
    kernel_set_ucred_caps(getpid(), caps);
    signal(SIGCHLD, SIG_IGN);
    make_dirs(ROOT "/logs");
    SwanStationPS5_log_open(LOG_FILE);
    say("disc daemon, pid %d", (int)getpid());
    take_over();
    load_language();
    load_launcher();
    notify(M_READY);

    discwatch_set_active(true);
    bool was_open = false;
    time_t closed_at = 0;
    for (;;)
    {
        usleep(kPollMs * 1000);
        if (superseded())
        {
            say("another daemon took over: stopping");
            break;
        }
        const bool app_open = stamp_fresh(APP_ALIVE, kAliveWithin);
        discwatch_pause(app_open); /* the app watches the drive itself */
        DiscId id;
        if (app_open)
        {
            /* whatever was seen just before the app opened is the app's business: if it were kept, it would be acted
             * on the moment the app is closed, and the app would open again by itself */
            discwatch_take(&id);
            was_open = true;
            continue;
        }
        if (was_open)
        {
            was_open = false;
            closed_at = time(NULL);
        }
        /* a disc already in when the app is closed is not news: only one put in, or changed, after that is. The first
         * few seconds after the app closes, what the watcher reports (it looks at the drive again after a pause) is
         * the same disc. */
        if (closed_at && time(NULL) - closed_at < kAfterCloseSec)
        {
            discwatch_take(&id);
            continue;
        }
        if (discwatch_take(&id))
            handle_disc(&id);
    }
    return 0;
}
