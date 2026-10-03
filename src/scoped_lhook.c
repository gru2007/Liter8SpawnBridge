/*
 * scoped_lhook.c
 *
 * Safer Liter8 launchd hook for Marketplace testing.
 *
 * This dylib is still weak-loaded by /sbin/launchd, but unlike the original
 * broad propagation hook it only injects into /usr/libexec/xpcproxy.
 *
 * xpcproxy receives:
 *   /usr/lib/lhook
 *   /var/jb/usr/lib/Liter8SpawnBridge.dylib
 *
 * SpawnBridge then decides whether the final SETEXEC target is one of:
 *   managedappdistributiond, appstorecomponentsd, installd
 *
 * No TweakLoader/ElleKit is injected into unrelated launchd children.
 */

#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
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

static const char *kSelf = "/usr/lib/lhook";
static const char *kBridge = "/var/jb/usr/lib/Liter8SpawnBridge.dylib";

/* Deliberately different from the old broad .lhook_enabled marker. */
static const char *kEnableFile = "/var/jb/.lhook_scoped_enabled";
static const char *kDebugFile  = "/var/jb/.lhook_scoped_debug";
static const char *kLogFile    = "/var/jb/tmp/lhook-scoped.log";

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *last_component(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int is_allowed_spawn(const char *path) {
    return strcmp(last_component(path), "xpcproxy") == 0;
}

static void trace(const char *fmt, const char *arg) {
    if (!file_exists(kDebugFile)) return;
    FILE *f = fopen(kLogFile, "a");
    if (!f) return;
    fprintf(f, fmt, arg);
    fclose(f);
}

static int value_has_path(const char *value, const char *path) {
    if (!value || !path) return 0;
    size_t plen = strlen(path);
    const char *p = value;
    while ((p = strstr(p, path)) != NULL) {
        int left_ok = (p == value || p[-1] == ':');
        int right_ok = (p[plen] == '\0' || p[plen] == ':');
        if (left_ok && right_ok) return 1;
        p += plen;
    }
    return 0;
}

static const char kInsertKey[] = "DYLD_INSERT_LIBRARIES=";
#define INSERT_KEY_LEN (sizeof(kInsertKey) - 1)

static char **env_with_scoped_payload(char *const envp[], char **owned_entry) {
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

    if (existing &&
        value_has_path(existing, kSelf) &&
        value_has_path(existing, kBridge))
        return NULL;

    char *value = NULL;
    if (existing && *existing) {
        if (asprintf(&value, "%s", existing) < 0) value = NULL;
    } else {
        value = strdup("");
    }
    if (!value) return NULL;

    const char *required[] = { kSelf, kBridge, NULL };
    for (int i = 0; required[i]; i++) {
        if (value_has_path(value, required[i])) continue;

        char *next = NULL;
        if (*value) {
            if (asprintf(&next, "%s:%s", value, required[i]) < 0) next = NULL;
        } else {
            if (asprintf(&next, "%s", required[i]) < 0) next = NULL;
        }
        free(value);
        if (!next) return NULL;
        value = next;
    }

    char *entry = NULL;
    if (asprintf(&entry, "%s%s", kInsertKey, value) < 0) entry = NULL;
    free(value);
    if (!entry) return NULL;

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
    out[j++] = entry;
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
    if (!file_exists(kEnableFile) ||
        !is_allowed_spawn(path) ||
        !file_exists(kBridge))
        return real(pid, path, actions, attr, argv, envp);

    trace("[lhook-scoped] injecting %s\n", path ? path : "(null)");

    char *owned = NULL;
    char **newenv = env_with_scoped_payload(envp, &owned);
    if (!newenv)
        return real(pid, path, actions, attr, argv, envp);

    int rc = real(pid, path, actions, attr, argv, newenv);
    free(owned);
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
static void scoped_lhook_init(void) {
    if (getpid() != 1) return;
    int fd = open("/dev/console", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        static const char msg[] = "[lhook-scoped] loaded into launchd\n";
        (void)write(fd, msg, sizeof(msg) - 1);
        close(fd);
    }
}
