/*
 * capslock-daemon — while Caps Lock is on, stop the Mac from sleeping.
 *
 * Detection: IOHIDGetModifierLockState() on IOHIDSystem, polled every 250 ms.
 * This needs no TCC permission at all. A CGEventTap would be event-driven, but
 * it inserts itself into the path of every system-wide input event and requires
 * Input Monitoring; for reading one lock bit that is a bad trade. The IORegistry
 * property HIDCapsLockState looks like a third option but was measured NOT to
 * follow the real state — it is a stale cache.
 *
 * Effect: the SleepDisabled system power setting, the same one that
 * `pmset -a disablesleep` writes. Unlike an IOPMAssertion (what `caffeinate`
 * takes), this still holds with the lid closed, which is the whole point.
 * It requires root, so this is meant to run as a LaunchDaemon.
 *
 * Steady-state cost: one IOKit call per 250 ms. No event tap, no app bundle,
 * no menu bar, no CFRunLoop.
 *
 * Caveat: Caps Lock is genuinely on while this is armed, so typing produces
 * capitals. Suppressing that is exactly the part that would need a CGEventTap
 * and Accessibility, and is deliberately not done here.
 */
#include <AvailabilityMacros.h>
#include <AvailabilityVersions.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <CoreFoundation/CoreFoundation.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* kIOMainPortDefault arrived in the macOS 12 SDK. nixpkgs 25.05 pins an older
 * apple-sdk, where only the kIOMasterPortDefault spelling exists — so building
 * against the local Xcode SDK succeeds while the Nix build fails. Both names
 * denote the same default port. */
#ifdef MAC_OS_VERSION_12_0
#  define MAIN_PORT kIOMainPortDefault
#else
#  define MAIN_PORT kIOMasterPortDefault
#endif

#define POLL_NS (250 * 1000 * 1000L)

extern char **environ;

/* IOPMSetSystemPowerSetting is what pmset itself calls, but Apple declares it
 * in IOPMLibPrivate.h and does not ship that header in the SDK. The symbol is
 * exported and verified working, so call it directly and skip a process spawn —
 * with a pmset fallback so a future macOS dropping the export degrades to slow
 * instead of silently doing nothing. */
extern IOReturn IOPMSetSystemPowerSetting(CFStringRef key, CFTypeRef value)
    __attribute__((weak_import));

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int s) { (void)s; stop_requested = 1; }

static void log_line(const char *msg) {
    time_t t = time(NULL);
    struct tm tm;
    char buf[32];
    localtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    printf("%s capslock-daemon: %s\n", buf, msg);
    fflush(stdout);
}

/* Read SleepDisabled back from IOPMrootDomain. Necessary because `pmset` exits
 * 0 even when it refuses the write for lack of privilege — its exit status
 * cannot be trusted as a success signal. */
static int read_sleep_disabled(bool *out) {
    io_service_t root = IOServiceGetMatchingService(MAIN_PORT,
                                                    IOServiceMatching("IOPMrootDomain"));
    if (!root) return -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(root, CFSTR("SleepDisabled"),
                                                  kCFAllocatorDefault, 0);
    IOObjectRelease(root);
    if (!v) return -1;
    int rc = -1;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        *out = CFBooleanGetValue((CFBooleanRef)v);
        rc = 0;
    }
    CFRelease(v);
    return rc;
}

static void set_via_pmset(bool disabled) {
    char *const argv[] = { "/usr/bin/pmset", "-a", "disablesleep",
                           (char *)(disabled ? "1" : "0"), NULL };
    pid_t pid;
    if (posix_spawn(&pid, "/usr/bin/pmset", NULL, NULL, argv, environ) != 0) return;
    int st;
    waitpid(pid, &st, 0);
}

/* Returns 0 only if the setting actually reads back as requested. */
static int set_sleep_disabled(bool disabled) {
    bool ok = false;

    if (IOPMSetSystemPowerSetting) {
        if (IOPMSetSystemPowerSetting(CFSTR("SleepDisabled"),
                                      disabled ? kCFBooleanTrue : kCFBooleanFalse)
            == kIOReturnSuccess)
            ok = true;
    }
    if (!ok) set_via_pmset(disabled);

    /* The write lands in the IORegistry asynchronously, so an immediate
     * read-back can still show the old value — reading once produced false
     * "did not take" reports when Caps Lock was toggled rapidly. Give it a
     * short settle window instead of judging on the first sample. */
    struct timespec settle = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000L };
    bool actual = !disabled;
    int readable = -1;
    for (int i = 0; i < 15; i++) {                 /* up to ~300 ms */
        readable = read_sleep_disabled(&actual);
        if (readable == 0 && actual == disabled) return 0;
        nanosleep(&settle, NULL);
    }

    char m[96];
    if (readable != 0)
        snprintf(m, sizeof m, "could not read SleepDisabled back");
    else
        snprintf(m, sizeof m, "SleepDisabled did not take: wanted %d, is %d", disabled, actual);
    log_line(m);
    return -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (geteuid() != 0) {
        log_line("fatal: must run as root (SleepDisabled is a privileged setting)");
        return 1;
    }

    io_service_t svc = IOServiceGetMatchingService(MAIN_PORT,
                                                    IOServiceMatching(kIOHIDSystemClass));
    if (!svc) { log_line("fatal: IOHIDSystem service not found"); return 1; }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), kIOHIDParamConnectType, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        char m[80];
        snprintf(m, sizeof m, "fatal: IOServiceOpen(IOHIDSystem) failed: 0x%08x", kr);
        log_line(m);
        return 1;
    }

    /* Start from a known state, then reconcile with whatever Caps Lock is doing
     * right now. Without this, a crash-restart could leave SleepDisabled stuck
     * on long after Caps Lock went off. */
    if (set_sleep_disabled(false) != 0) {
        log_line("fatal: cannot write SleepDisabled");
        IOServiceClose(conn);
        return 1;
    }

    log_line("started (sleep allowed)");

    bool applied = false, first = true;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = POLL_NS };

    while (!stop_requested) {
        bool caps = false;
        if (IOHIDGetModifierLockState(conn, kIOHIDCapsLockState, &caps) != KERN_SUCCESS) {
            log_line("IOHIDGetModifierLockState failed; exiting for launchd to restart");
            break;
        }
        if (first || caps != applied) {
            if (set_sleep_disabled(caps) == 0) {
                applied = caps;
                log_line(caps ? "Caps Lock ON  -> sleep DISABLED"
                              : "Caps Lock OFF -> sleep allowed");
            }
            first = false;
        }
        nanosleep(&ts, NULL);
    }

    /* Never leave the machine unable to sleep just because we went away. */
    set_sleep_disabled(false);
    log_line("stopped (sleep allowed)");
    IOServiceClose(conn);
    return 0;
}
