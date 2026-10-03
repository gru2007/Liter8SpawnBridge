/*
 * Liter8SpawnBridge
 *
 * Loaded directly into xpcproxy by the scoped Liter8 lhook. ElleKit is NOT
 * loaded into xpcproxy. This dylib interposes posix_spawn/posix_spawnp at
 * process start and only modifies the environment for a tiny target allowlist.
 */

#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                      \
    __attribute__((used)) static struct {                                            \
        const void *replacement;                                                     \
        const void *replacee;                                                        \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {      \
        (const void *)(unsigned long)&_replacement,                                  \
        (const void *)(unsigned long)&_replacee                                      \
    };

static const char *kLogPath = "/var/tmp/Liter8SpawnBridge.log";
static const char *kInsertKey = "DYLD_INSERT_LIBRARIES=";

static const char *kTweakLoader = "/var/jb/usr/lib/TweakLoader.dylib";
static const char *kSystemHook  = "/usr/lib/systemhook.dylib";

static const char *last_component(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *payload_for_target(const char *path) {
    const char *name = last_component(path);

    /* Marketplace-related daemons: only ElleKit's loader is needed. */
    if (strcmp(name, "managedappdistributiond") == 0 ||
        strcmp(name, "appstorecomponentsd") == 0 ||
        strcmp(name, "installd") == 0)
        return kTweakLoader;

    /* Preserve Liter8's existing rootless icon read-extension helper, but do
     * NOT put ElleKit into iconservicesagent. */
    if (strcmp(name, "iconservicesagent") == 0)
        return kSystemHook;

    return NULL;
}

static int running_in_xpcproxy(void) {
    extern const char *getprogname(void);
    const char *name = getprogname();
    return name && strcmp(name, "xpcproxy") == 0;
}

static void bridge_log(const char *fmt, ...) {
    int fd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;

    va_list ap;
    va_start(ap, fmt);
    vdprintf(fd, fmt, ap);
    va_end(ap);
    close(fd);
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

static char **env_with_payload(char *const envp[],
                               const char *payload,
                               char **owned_entry) {
    if (!envp || !payload) return NULL;

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

    if (existing && value_has_path(existing, payload))
        return NULL;

    char *entry = NULL;
    if (existing && *existing) {
        if (asprintf(&entry, "%s%s:%s", kInsertKey, existing, payload) < 0)
            entry = NULL;
    } else {
        if (asprintf(&entry, "%s%s", kInsertKey, payload) < 0)
            entry = NULL;
    }
    if (!entry) return NULL;

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
    if (!running_in_xpcproxy())
        return real(pid, path, actions, attr, argv, envp);

    const char *payload = payload_for_target(path);
    if (!payload)
        return real(pid, path, actions, attr, argv, envp);

    if (access(payload, F_OK) != 0) {
        bridge_log("[SpawnBridge] target=%s skipped: payload missing (%s)\n",
                   path ? path : "(null)", payload);
        return real(pid, path, actions, attr, argv, envp);
    }

    char *owned_entry = NULL;
    char **newenv = env_with_payload(envp, payload, &owned_entry);

    bridge_log("[SpawnBridge] pid=%d target=%s payload=%s env=%s\n",
               getpid(),
               path ? path : "(null)",
               payload,
               newenv ? "patched" : "already-ready");

    int rc = real(pid, path, actions, attr, argv, newenv ? newenv : envp);

    free(owned_entry);
    free(newenv);

    if (rc != 0) {
        bridge_log("[SpawnBridge] target=%s rc=%d\n",
                   path ? path : "(null)", rc);
    }
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
    bridge_log("[SpawnBridge] loaded pid=%d direct-dyld-interpose\n", getpid());
}
