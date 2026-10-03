/*
 * Liter8 universal launchd router.
 *
 * Loaded only into PID 1 as /usr/lib/lhook.
 *
 * It NEVER propagates itself.
 *
 * launchd -> xpcproxy:
 *   inject only /var/jb/usr/lib/Liter8SpawnBridge.dylib
 *
 * launchd -> direct ordinary target (not xpcproxy):
 *   inject only /var/jb/usr/lib/TweakLoader.dylib
 *   (+ /usr/lib/systemhook.dylib for iconservicesagent)
 *
 * Critical/recovery processes stay clean.
 */

#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

static const char *kEnableFile = "/var/jb/.lhook_enabled";
static const char *kDebugFile  = "/var/jb/.lhook_debug";
static const char *kLogFile    = "/var/jb/tmp/lhook.log";
static const char *kDenyFile   = "/var/jb/etc/lhook.deny";

static const char kInsertKey[] = "DYLD_INSERT_LIBRARIES=";
#define INSERT_KEY_LEN (sizeof(kInsertKey) - 1)

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
    if (!nlen) return 1;

    for (const char *p = haystack; *p; p++)
        if (strncasecmp(p, needle, nlen) == 0)
            return 1;

    return 0;
}

static int name_in(const char *const *list, const char *name) {
    for (int i = 0; list[i]; i++)
        if (strcmp(list[i], name) == 0)
            return 1;
    return 0;
}

static const char *const kHardDeny[] = {
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

static int denied_by_file(const char *name) {
    FILE *f = fopen(kDenyFile, "r");
    if (!f) return 0;

    char line[256];
    int denied = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        char *end = p + strlen(p);
        while (end > p &&
               (end[-1] == '\n' || end[-1] == '\r' ||
                end[-1] == ' ' || end[-1] == '\t'))
            end--;
        *end = '\0';

        if (strcmp(p, name) == 0) {
            denied = 1;
            break;
        }
    }

    fclose(f);
    return denied;
}

static int denied(const char *path) {
    const char *name = last_component(path);

    if (name_in(kHardDeny, name)) return 1;
    if (denied_by_file(name)) return 1;

    if (contains_ci(path, "webkit")) return 1;
    if (contains_ci(path, "blastdoor")) return 1;

    return 0;
}

static void trace(const char *fmt, const char *arg) {
    if (!file_exists(kDebugFile)) return;

    FILE *f = fopen(kLogFile, "a");
    if (!f) return;
    fprintf(f, fmt, arg ? arg : "(null)");
    fclose(f);
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

static char *sanitize_existing(const char *existing) {
    if (!existing || !*existing) return strdup("");

    char *out = calloc(1, strlen(existing) + 1);
    if (!out) return NULL;

    size_t used = 0;
    const char *p = existing;

    while (*p) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len && !managed_path(p, len)) {
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

    char *next = NULL;

    if (*value && **value) {
        if (asprintf(&next, "%s:%s", *value, path) < 0)
            next = NULL;
    } else {
        if (asprintf(&next, "%s", path) < 0)
            next = NULL;
    }

    if (!next) return 0;
    free(*value);
    *value = next;
    return 1;
}

static char **build_env(char *const envp[],
                        const char *path,
                        char **owned_entry) {
    if (!envp) return NULL;

    size_t count = 0;
    while (envp[count]) count++;

    const char *existing = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(envp[i], kInsertKey, INSERT_KEY_LEN) == 0) {
            existing = envp[i] + INSERT_KEY_LEN;
            break;
        }
    }

    char *value = sanitize_existing(existing);
    if (!value) return NULL;

    const char *name = last_component(path);

    if (strcmp(name, "xpcproxy") == 0) {
        if (!append_path(&value, kRouter)) {
            free(value);
            return NULL;
        }
    } else if (!denied(path)) {
        if (!append_path(&value, kTweakLoader)) {
            free(value);
            return NULL;
        }

        if (strcmp(name, "iconservicesagent") == 0) {
            if (!append_path(&value, kSystemHook)) {
                free(value);
                return NULL;
            }
        }
    }

    char *entry = NULL;
    if (*value) {
        if (asprintf(&entry, "%s%s", kInsertKey, value) < 0)
            entry = NULL;
    }
    free(value);

    char **out = calloc(count + 2, sizeof(char *));
    if (!out) {
        free(entry);
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(envp[i], kInsertKey, INSERT_KEY_LEN) == 0) continue;
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
    if (!file_exists(kEnableFile))
        return real(pid, path, actions, attr, argv, envp);

    const char *name = last_component(path);

    /* Critical targets are never touched. xpcproxy is special: it gets only
     * the tiny Router, never TweakLoader. */
    if (strcmp(name, "xpcproxy") != 0 && denied(path))
        return real(pid, path, actions, attr, argv, envp);

    if (strcmp(name, "xpcproxy") == 0)
        trace("[lhook] router -> %s\n", path);
    else
        trace("[lhook] loader -> %s\n", path);

    char *owned_entry = NULL;
    char **newenv = build_env(envp, path, &owned_entry);

    if (!newenv)
        return real(pid, path, actions, attr, argv, envp);

    int rc = real(pid, path, actions, attr, argv, newenv);

    free(owned_entry);
    free(newenv);

    return rc;
}

static int my_posix_spawn(pid_t *pid,
                          const char *path,
                          const posix_spawn_file_actions_t *actions,
                          const posix_spawnattr_t *attr,
                          char *const argv[],
                          char *const envp[]) {
    return spawn_common(posix_spawn, pid, path, actions, attr, argv, envp);
}

static int my_posix_spawnp(pid_t *pid,
                           const char *path,
                           const posix_spawn_file_actions_t *actions,
                           const posix_spawnattr_t *attr,
                           char *const argv[],
                           char *const envp[]) {
    return spawn_common(posix_spawnp, pid, path, actions, attr, argv, envp);
}

DYLD_INTERPOSE(my_posix_spawn, posix_spawn)
DYLD_INTERPOSE(my_posix_spawnp, posix_spawnp)

__attribute__((constructor))
static void lhook_init(void) {
    if (getpid() != 1) return;

    int fd = open("/dev/console", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        static const char msg[] = "[lhook] universal router loaded in launchd\n";
        (void)write(fd, msg, sizeof(msg) - 1);
        close(fd);
    }
}
