# Liter8 SpawnBridge

Experimental compatibility bridge for **Xplo8E/Liter8** tweak injection on iOS/iPadOS 26/27.

## Why

On the tested iPadOS 26 build, Liter8 successfully injects `lhook.dylib` and ElleKit's `TweakLoader.dylib` into `/usr/libexec/xpcproxy`, but the injected environment is lost when that `xpcproxy` instance performs the final `posix_spawn(..., POSIX_SPAWN_SETEXEC, ...)` into selected launchd services.

Observed example:

```text
[lhook] injecting /usr/libexec/xpcproxy
[lhook] loaded into pid 10041
# PID 10041 later becomes managedappdistributiond,
# but there is no second lhook load for the final executable.
```

`xpcproxy` on this build imports `_posix_spawn` directly. SpawnBridge therefore loads as an ElleKit tweak **inside xpcproxy** and uses `MSHookFunction` to hook `posix_spawn`/`posix_spawnp` at runtime rather than relying on dyld `__interpose`.

For a small allowlist of targets it adds back:

```text
/usr/lib/lhook.dylib
/var/jb/usr/lib/TweakLoader.dylib
```

to `DYLD_INSERT_LIBRARIES` before the final spawn/SETEXEC transition.

## Target allowlist

The initial build only modifies launches of:

- `managedappdistributiond`
- `appstorecomponentsd`
- `installd`

This is deliberate. Do not make this system-wide until the approach is proven stable.

## Requirements

- Liter8 with `/usr/lib/lhook.dylib`
- rootless bootstrap at `/var/jb`
- ElleKit with `/var/jb/usr/lib/TweakLoader.dylib`
- `/var/jb/.lhook_enabled`

## Install

Use the `.deb` produced by GitHub Actions:

```sh
dpkg -i com.gru2007.liter8spawnbridge_*_iphoneos-arm64.deb
```

Then restart the target services:

```sh
launchctl kickstart -k user/foreground/com.apple.managedappdistributiond
launchctl kickstart -k user/foreground/com.apple.appstorecomponentsd
launchctl kickstart -k user/501/com.apple.mobile.installd
```

For MarketplaceEnabler testing, retry the MarketplaceKit installation from Safari after the first two daemons have restarted.

## Diagnostics

SpawnBridge writes:

```text
/var/tmp/Liter8SpawnBridge.log
```

Expected output:

```text
[SpawnBridge] loaded pid=... posix_spawn=hooked posix_spawnp=hooked
[SpawnBridge] pid=... target=/System/.../managedappdistributiond env=patched
```

Then Liter8's own log should finally show the target executable being injected, not just `xpcproxy`:

```text
[lhook] injecting .../managedappdistributiond
[lhook] loaded into pid ...
```

## Build locally

Requires macOS + Xcode:

```sh
./build.sh
```

Artifacts appear in `dist/`.

## GitHub Actions

Every push/PR builds a universal `arm64 + arm64e` dylib. The workflow also packages a rootless `.deb`.

A tag such as `v0.1.0` automatically creates a GitHub Release. Alternatively run **Build SpawnBridge → Run workflow** and provide `release_tag=v0.1.0`.

## Recovery

If system-process behavior becomes unstable, remove/disable the tweak from SSH or a recovery ramdisk:

```sh
rm -f /var/jb/usr/lib/TweakInject/Liter8SpawnBridge.dylib
rm -f /var/jb/usr/lib/TweakInject/Liter8SpawnBridge.plist
```

Or globally disable Liter8 tweak propagation:

```sh
rm -f /var/jb/.lhook_enabled
```

This project is experimental and intentionally targets only the three daemons above.
