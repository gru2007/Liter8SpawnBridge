# Liter8 SpawnBridge

Universal two-stage tweak-injection router for **Xplo8E/Liter8 + ElleKit** on iOS/iPadOS 26/27.

## Why v0.3 exists

The first Liter8 experiments injected ElleKit's `TweakLoader.dylib` into almost every process. That is much broader than ElleKit's intended launch flow and can destabilize boot.

v0.3 keeps the heavy loader out of the propagation layer:

```text
launchd
  │
  │ /usr/lib/lhook (PID 1 only)
  │
  ├─ critical/recovery process ─────────────► untouched
  │
  ├─ direct ordinary target ────────────────► TweakLoader
  │
  └─ xpcproxy ──────────────────────────────► Liter8SpawnBridge only
                                                │
                                                │ SETEXEC / posix_spawn
                                                ▼
                                      final ordinary target
                                                │
                                                └─ TweakLoader
```

The Router removes itself from `DYLD_INSERT_LIBRARIES` before the final xpcproxy SETEXEC. It does **not** propagate into the target.

Once TweakLoader is inside the final process, ElleKit's own injector applies normal tweak filters such as:

- `Bundles`
- `Executables`
- `Classes`
- `CoreFoundationVersion`

So installing a new tweak does **not** require editing lhook or SpawnBridge.

## Examples

A tweak filtered for Safari loads in Safari automatically.

A tweak filtered for SpringBoard loads in SpringBoard automatically.

MarketplaceEnabler's `Executables = managedappdistributiond, appstorecomponentsd, installd` works through the same generic path.

Unrelated tweak dylibs are not loaded because ElleKit's injector rejects their filters.

## Safety blacklist

The routing layer keeps critical/recovery processes clean, including trust/security daemons, watchdog, SSH/dropbear, shell processes, WebKit helpers and BlastDoor.

The editable extra deny list remains:

```text
/var/jb/etc/lhook.deny
```

One executable basename per line.

## Artifacts

GitHub Actions builds:

- `Liter8SpawnBridge.dylib`
- `lhook-universal.dylib`
- `com.gru2007.liter8spawnbridge_<version>_iphoneos-arm64.deb`

The deb installs:

```text
/var/jb/usr/lib/Liter8SpawnBridge.dylib
/var/jb/usr/share/liter8spawnbridge/lhook-universal.dylib
```

It deliberately leaves injection disabled.

## Install package

On a normal boot:

```sh
dpkg -i com.gru2007.liter8spawnbridge_*_iphoneos-arm64.deb
```

## Replace the System-volume lhook

Boot Liter8 SSHRD, mount System + Data read-write:

```sh
mkdir -p /mnt1 /mnt2
mount_apfs /dev/disk1s1 /mnt1 2>/dev/null || true
mount_apfs /dev/disk1s2 /mnt2 2>/dev/null || true
mount -u -o rw /dev/disk1s1 2>/dev/null || true
mount -u -o rw /dev/disk1s2 2>/dev/null || true
```

Back up the currently working hook:

```sh
cp -p /mnt1/usr/lib/lhook /mnt1/usr/lib/lhook.pre-v03
```

Install the new one:

```sh
cp /mnt2/jb/usr/share/liter8spawnbridge/lhook-universal.dylib \
   /mnt1/usr/lib/lhook
chmod 0755 /mnt1/usr/lib/lhook
chown root:wheel /mnt1/usr/lib/lhook
sync
```

Then perform the normal Liter8 tethered boot.

## Enable only after the UI has booted

```sh
touch /var/jb/.lhook_enabled
```

Optional diagnostics:

```sh
touch /var/jb/.lhook_debug
```

Disable immediately:

```sh
rm -f /var/jb/.lhook_enabled /var/jb/.lhook_debug
```

## Diagnostics

PID 1 routing:

```sh
cat /var/jb/tmp/lhook.log
```

Typical lines:

```text
[lhook] router -> /usr/libexec/xpcproxy
[lhook] loader -> /System/Library/CoreServices/SpringBoard.app/SpringBoard
```

xpcproxy final-target routing:

```sh
cat /var/jb/tmp/Liter8SpawnBridge.log
```

Typical lines:

```text
[SpawnBridge] loaded pid=... universal-router
[SpawnBridge] pid=... target=/System/.../managedappdistributiond mode=TweakLoader
[SpawnBridge] pid=... target=/System/.../com.apple.WebKit.WebContent mode=clean/blacklisted
```

There should be no old-style cascade of `[lhook] loaded into pid ...` across unrelated processes because `lhook` no longer propagates itself.

## Recovery

From a normal SSH session:

```sh
rm -f /var/jb/.lhook_enabled /var/jb/.lhook_debug
```

From SSHRD:

```sh
rm -f /mnt2/jb/.lhook_enabled /mnt2/jb/.lhook_debug
cp -p /mnt1/usr/lib/lhook.pre-v03 /mnt1/usr/lib/lhook
sync
```

## Build

Requires macOS + Xcode:

```sh
./build.sh
```

Both dylibs are universal `arm64 + arm64e`.
