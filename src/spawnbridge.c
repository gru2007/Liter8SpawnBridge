#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

typedef void (*MSHookFunction_t)(void *symbol, void *replace, void **result);

typedef int (*posix_spawn_fn)(pid_t *restrict,
                              const char *restrict,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *restrict,
                              char *const argv[restrict],
                              char *const envp[restrict]);

static posix_spawn_fn orig_posix_spawn = NULL;
static posix_spawn_fn orig_posix_spawnp = NULL;

static const char *kLogPath = "/var/tmp/Liter8SpawnBridge.log";
static const char *kInsertKey = "DYLD_INSERT_LIBRARIES=";
static const char *kRequiredPayloads[] = {
    "/usr/lib/lhook.dylib",
    "/var/jb/usr/lib/TweakLoader.dylib",
    NULL,
};

static const char *last_component(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
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

static int is_target(const char *path) {
    const char *name = last_component(path);
    return strcmp(name, "managedappdistributiond") == 0 ||
           strcmp(name, "appstorecomponentsd") == 0 ||
           strcmp(name, "installd") == 0;
}

static int string_has_path(const char *value, const char *path) {
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

static char *build_insert_value(const char *existing) {
    size_t cap = 256 + (existing ? strlen(existing) : 0);
    char *value = calloc(1, cap);
    if (!value) return NULL;

    size_t used = 0;
    if (existing && *existing) {
        size_t n = strlen(existing);
        memcpy(value, existing, n);
        used = n;
    }

    for (int i = 0; kRequiredPayloads[i]; i++) {
        const char *payload = kRequiredPayloads[i];
        if (existing && string_has_path(existing, payload)) continue;

        size_t plen = strlen(payload);
        size_t needed = used + (used ? 1 : 0) + plen + 1;
        if (needed > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < needed) new_cap *= 2;
            char *grown = realloc(value, new_cap);
            if (!grown) {
                free(value);
                return NULL;
            }
            value = grown;
            cap = new_cap;
        }
        if (used) value[used++] = ':';
        memcpy(value + used, payload, plen);
        used += plen;
        value[used] = '\0';
    }

    return value;
}

static char **env_with_bridge(char *const envp[], char **owned_entry) {
    char *const *source = envp ? envp : environ;
    size_t count = 0;
    while (source && source[count]) count++;

    const size_t key_len = strlen(kInsertKey);
    const char *existing = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(source[i], kInsertKey, key_len) == 0) {
            existing = source[i] + key_len;
            break;
        }
    }

    char *insert_value = build_insert_value(existing);
    if (!insert_value) return NULL;

    if (existing &&
        string_has_path(existing, kRequiredPayloads[0]) &&
        string_has_path(existing, kRequiredPayloads[1])) {
        free(insert_value);
        return NULL;
    }

    char *entry = NULL;
    if (asprintf(&entry, "%s%s", kInsertKey, insert_value) < 0) entry = NULL;
    free(insert_value);
    if (!entry) return NULL;

    char **out = calloc(count + 2, sizeof(char *));
    if (!out) {
        free(entry);
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(source[i], kInsertKey, key_len) == 0) continue;
        out[j++] = source[i];
    }
    out[j++] = entry;
    out[j] = NULL;
    *owned_entry = entry;
    return out;
}

static int bridge_spawn_common(posix_spawn_fn original,
                               pid_t *restrict pid,
                               const char *restrict path,
                               const posix_spawn_file_actions_t *actions,
                               const posix_spawnattr_t *restrict attr,
                               char *const argv[restrict],
                               char *const envp[restrict]) {
    if (!original) return ENOSYS;
    if (!is_target(path)) return original(pid, path, actions, attr, argv, envp);

    char *owned_entry = NULL;
    char **new_env = env_with_bridge(envp, &owned_entry);

    bridge_log("[SpawnBridge] pid=%d target=%s env=%s\n",
               getpid(), path ? path : "(null)", new_env ? "patched" : "already-ready");

    int rc = original(pid, path, actions, attr, argv, new_env ? new_env : envp);

    if (new_env) free(new_env);
    if (owned_entry) free(owned_entry);

    if (rc != 0) {
        bridge_log("[SpawnBridge] posix_spawn target=%s rc=%d\n",
                   path ? path : "(null)", rc);
    }
    return rc;
}

static int bridge_posix_spawn(pid_t *restrict pid,
                              const char *restrict path,
                              const posix_spawn_file_actions_t *actions,
                              const posix_spawnattr_t *restrict attr,
                              char *const argv[restrict],
                              char *const envp[restrict]) {
    return bridge_spawn_common(orig_posix_spawn, pid, path, actions, attr, argv, envp);
}

static int bridge_posix_spawnp(pid_t *restrict pid,
                               const char *restrict file,
                               const posix_spawn_file_actions_t *actions,
                               const posix_spawnattr_t *restrict attr,
                               char *const argv[restrict],
                               char *const envp[restrict]) {
    return bridge_spawn_common(orig_posix_spawnp, pid, file, actions, attr, argv, envp);
}

static int running_in_xpcproxy(void) {
    char path[4096] = {0};
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return 0;
    return strcmp(last_component(path), "xpcproxy") == 0;
}

__attribute__((constructor))
static void spawnbridge_init(void) {
    if (!running_in_xpcproxy()) return;

    void *substrate = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!substrate) {
        bridge_log("[SpawnBridge] libsubstrate dlopen failed: %s\n", dlerror());
        return;
    }

    MSHookFunction_t MSHookFunction = (MSHookFunction_t)dlsym(substrate, "MSHookFunction");
    if (!MSHookFunction) {
        bridge_log("[SpawnBridge] MSHookFunction unavailable\n");
        return;
    }

    void *spawn_sym = dlsym(RTLD_DEFAULT, "posix_spawn");
    void *spawnp_sym = dlsym(RTLD_DEFAULT, "posix_spawnp");

    if (spawn_sym) {
        MSHookFunction(spawn_sym, (void *)&bridge_posix_spawn, (void **)&orig_posix_spawn);
    }
    if (spawnp_sym) {
        MSHookFunction(spawnp_sym, (void *)&bridge_posix_spawnp, (void **)&orig_posix_spawnp);
    }

    bridge_log("[SpawnBridge] loaded pid=%d posix_spawn=%s posix_spawnp=%s\n",
               getpid(), spawn_sym ? "hooked" : "missing", spawnp_sym ? "hooked" : "missing");
}
