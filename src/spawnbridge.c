/*
 * Liter8SpawnBridge - universal xpcproxy router for Liter8 + ElleKit.
 *
 * This image is inserted ONLY into xpcproxy by /usr/lib/lhook.
 * It never propagates itself into the final target.
 *
 * For the xpcproxy SETEXEC transition it:
 *   1. removes itself from DYLD_INSERT_LIBRARIES;
 *   2. leaves critical/blacklisted targets clean;
 *   3. inserts ElleKit TweakLoader into ordinary final targets;
 *   4. also inserts Liter8 systemhook for iconservicesagent.
 *
 * TweakLoader/libinjector then applies normal tweak Filter semantics, so adding a
 * new tweak does not require editing this file.
 */

#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                      \
    __attribute__((used)) static struct {                                            \
        const void *replacement;                                                     \
        const void *replacee;                                                        \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {      \
        (const void *)(unsigned long)&_replacement,                                  \
        (const void *)(unsigned long)&_replacee                                      \
    };

static const char *kRouter      = "/var/jb/usr/lib/Liter8SpawnBridge.dylib";
static const char *kTweakLoader = "/var/jb/usr/lib/TweakLoader.dylib";
static const char *kSystemHook  = "/usr/lib/systemhook.dylib";

static const char *kDebugFile = "/var/jb/.lhook_debug";
static const char *kLogPath   = "/var/jb/tmp/Liter8SpawnBridge.log";
static const char *kInsertKey = "DYLD_INSERT_LIBRARIES=";

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static const char *last_component(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return 1;
    }
    return 0;
}

static int running_in_xpcproxy(void) {
    extern const char *getprogname(void);
    const char *name = getprogname();
    return name && strcmp(name, "xpcproxy") == 0;
}

/* Mirrors the spirit of ElleKit's launch blacklist, with additional recovery/
 * trust processes that Liter8 must never touch. This is a loader blacklist, not
 * a tweak list: ordinary apps and daemons still use their own tweak Filters. */
static int blacklisted_target(const char *path) {
    const char *name = last_component(path);

    static const char *exact[] = {
        "launchd",
        "amfid",
        "trustd",
        "securityd",
        "configd",
        "notifyd",
        "logd",
        "opendirectoryd",
        "keybagd",
        "watchdogd",
        "mobile_assertion_agent",
        "mobile.usermanagerd",
        "dropbear",
        "sshd",
        "jailbreakd",
        "loader",
        "GSSCred",
        "sh",
        "bash",
        "zsh",
        NULL
    };

    for (int i = 0; exact[i]; i++)
        if (strcmp(name, exact[i]) == 0) return 1;

    /* ElleKit itself avoids WebKit/BlastDoor style helper processes. Safari.app
     * is NOT matched by these strings and still receives TweakLoader normally. */
    if (contains_ci(path, "webkit")) return 1;
    if (contains_ci(path, "blastdoor")) return 1;

    return 0;
}

static void bridge_log(const char *fmt, ...) {
    if (!file_exists(kDebugFile)) return;

    int fd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;

    va_list ap;
    va_start(ap, fmt);
    vdprintf(fd, fmt, ap);
    va_end(ap);
    close(fd);
}

static int same_path(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    return alen == blen && memcmp(a, b, blen) == 0;
}

static int managed_path(const char *start, size_t len) {
    return same_path(start, len, kRouter) ||
           same_path(start, len, kTweakLoader) ||
           same_path(start, len, kSystemHook);
}

/* Copy an existing DYLD_INSERT_LIBRARIES value while removing every path owned
 * by this injection stack. This is the key safety property: xpcproxy receives
 * the Router, but the final target never inherits the Router itself. */
static char *sanitize_existing(const char *existing) {
    if (!existing || !*existing) return strdup("");

    size_t cap = strlen(existing) + 1;
    char *out = calloc(1, cap);
    if (!out) return NULL;

    size_t used = 0;
    const char *p = existing;

    while (*p) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len > 0 && !managed_path(p, len)) {
            if (used) out[used++] = ':';
            memcpy(out + used, p, len);
            used += len;
            out[used] = '\0';
        }

        if (!end) break;
        p = end + 1;
    }

    return out;
}

static int append_path(char **value, const char *path) {
    if (!path || !file_exists(path)) return 1;

    char *old = *value;
    char *next = NULL;

    if (old && *old) {
        if (asprintf(&next, "%s:%s", old, path) < 0) next = NULL;
    } else {
        if (asprintf(&next, "%s", path) < 0) next = NULL;
    }

    if (!next) return 0;
    free(old);
    *value = next;
    return 1;
}

static char **build_final_env(char *const envp[],
                              const char *path,
                              char **owned_entry) {
    if (!envp) return NULL;

    size_t count = 0;
    while (envp[count]) count++;

    const size_t key_len = strlen(kInsertKey);
    const char *existing = NULL;

    for (size_t i = 0; i < count; i++) {
        if (strncmp(envp[i], kInsertKey, key_len) == 0) {
            existing = envp[i] + key_len;
            break;
        }
    }

    char *value = sanitize_existing(existing);
    if (!value) return NULL;

    if (!blacklisted_target(path)) {
        if (!append_path(&value, kTweakLoader)) {
            free(value);
            return NULL;
        }

        if (strcmp(last_component(path), "iconservicesagent") == 0) {
            if (!append_path(&value, kSystemHook)) {
                free(value);
                return NULL;
            }
        }
    }

    /* Always rebuild the environment, even for blacklisted targets, because the
     * xpcproxy environment contains kRouter and it must not survive SETEXEC. */
    char *entry = NULL;
    if (*value) {
        if (asprintf(&entry, "%s%s", kInsertKey, value) < 0) entry = NULL;
    }
    free(value);

    char **out = calloc(count + 2, sizeof(char *));
    if (!out) {
        free(entry);
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(envp[i], kInsertKey, key_len) == 0) continue;
        out[j++] = envp[i];
    }

    if (entry) out[j++] = entry;
    out[j] = NULL;

    *owned_entry = entry;
    return out;
}

static int spawn_common(int (*real)(pid_t *, const char *,
                                    const posix_spawn_file_actions_t *,
                                    const posix_spawnattr_t *,
                                    char *const [], char *const []),
                        pid_t *pid,
                        const char *path,
                        const posix_spawn_file_actions_t *actions,
                        const posix_spawnattr_t *attr,
                        char *const argv[],
                        char *const envp[]) {
    if (!running_in_xpcproxy())
        return real(pid, path, actions, attr, argv, envp);

    char *owned_entry = NULL;
    char **newenv = build_final_env(envp, path, &owned_entry);

    if (!newenv) {
        bridge_log("[SpawnBridge] target=%s env rebuild failed; passthrough\n",
                   path ? path : "(null)");
        return real(pid, path, actions, attr, argv, envp);
    }

    bridge_log("[SpawnBridge] pid=%d target=%s mode=%s\n",
               getpid(),
               path ? path : "(null)",
               blacklisted_target(path) ? "clean/blacklisted" : "TweakLoader");

    int rc = real(pid, path, actions, attr, argv, newenv);

    free(owned_entry);
    free(newenv);

    if (rc != 0)
        bridge_log("[SpawnBridge] target=%s rc=%d\n",
                   path ? path : "(null)", rc);

    return rc;
}

static int bridge_posix_spawn(pid_t *pid,
                              const char *path,
                              const posix_spawn_file_actions_t *actions,
                              const posix_spawnattr_t *attr,
                              char *const argv[],
                              char *const envp[]) {
    return spawn_common(posix_spawn, pid, path, actions, attr, argv, envp);
}

static int bridge_posix_spawnp(pid_t *pid,
                               const char *path,
                               const posix_spawn_file_actions_t *actions,
                               const posix_spawnattr_t *attr,
                               char *const argv[],
                               char *const envp[]) {
    return spawn_common(posix_spawnp, pid, path, actions, attr, argv, envp);
}

DYLD_INTERPOSE(bridge_posix_spawn, posix_spawn)
DYLD_INTERPOSE(bridge_posix_spawnp, posix_spawnp)

__attribute__((constructor))
static void spawnbridge_init(void) {
    if (!running_in_xpcproxy()) return;
    bridge_log("[SpawnBridge] loaded pid=%d universal-router\n", getpid());
}
