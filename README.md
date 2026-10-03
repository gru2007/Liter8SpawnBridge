# Liter8 SpawnBridge

Experimental **scoped** tweak-injection bridge for Xplo8E/Liter8 on iOS/iPadOS 26/27.

## v0.2 design

The original Liter8 lhook propagates its payloads broadly once `/var/jb/.lhook_enabled` exists. That is useful for generic tweak injection, but it is too aggressive for this Marketplace experiment and can destabilize boot.

v0.2 uses a much narrower chain:

```text
launchd
  │
  │ scoped lhook: ONLY xpcproxy
  ▼
xpcproxy
  │
  │ Liter8SpawnBridge.dylib (direct DYLD interpose, no ElleKit here)
  │
  ├─ unrelated target ───────────────► untouched
  │
  └─ managedappdistributiond
     appstorecomponentsd
     installd
          │
          │ add ONLY /var/jb/usr/lib/TweakLoader.dylib
          ▼
       target daemon
          │
          ▼
        ElleKit
          │
          ▼
    matching TweakInject tweaks
```

Only the three final daemons above receive ElleKit's TweakLoader.

## Artifacts

GitHub Actions builds:

- `Liter8SpawnBridge.dylib` — directly injected into xpcproxy.
- `lhook-scoped.dylib` — replacement for Liter8's broad `/usr/lib/lhook.dylib`.
- `com.gru2007.liter8spawnbridge_<version>_iphoneos-arm64.deb`

The deb installs:

```text
/var/jb/usr/lib/Liter8SpawnBridge.dylib
/var/jb/usr/share/liter8spawnbridge/lhook-scoped.dylib
```

It deliberately removes the old v0.1.x TweakInject copy and disables the old broad markers:

```text
/var/jb/.lhook_enabled
/var/jb/.lhook_debug
```

It does **not** enable scoped injection automatically.

## Install v0.2 package

On the normal boot:

```sh
dpkg -i com.gru2007.liter8spawnbridge_*_iphoneos-arm64.deb
```

Do not create the old `.lhook_enabled` marker.

## Install the scoped system lhook

Replacing `/usr/lib/lhook.dylib` requires Liter8 SSHRD because the System volume is read-only during normal boot.

Boot SSHRD, mount System + Data, then:

```sh
mkdir -p /mnt1 /mnt2
mount_apfs /dev/disk1s1 /mnt1 2>/dev/null || true
mount_apfs /dev/disk1s2 /mnt2 2>/dev/null || true
mount -u -o rw /dev/disk1s1 2>/dev/null || true
mount -u -o rw /dev/disk1s2 2>/dev/null || true

cp -p /mnt1/usr/lib/lhook.dylib /mnt1/usr/lib/lhook.dylib.pre-scoped
cp /mnt2/jb/usr/share/liter8spawnbridge/lhook-scoped.dylib /mnt1/usr/lib/lhook.dylib
chmod 0755 /mnt1/usr/lib/lhook.dylib
chown root:wheel /mnt1/usr/lib/lhook.dylib
sync
```

Then perform the normal Liter8 tethered boot.

## Enable after the device has booted

The scoped build intentionally uses a **new marker**:

```sh
touch /var/jb/.lhook_scoped_enabled
touch /var/jb/.lhook_scoped_debug
```

It ignores the old `.lhook_enabled` marker.

Restart only the Marketplace-related jobs:

```sh
launchctl kickstart -k user/foreground/com.apple.managedappdistributiond
launchctl kickstart -k user/foreground/com.apple.appstorecomponentsd
launchctl kickstart -k user/501/com.apple.mobile.installd
```

## Diagnostics

Scoped lhook:

```sh
cat /var/jb/tmp/lhook-scoped.log
```

Expected:

```text
[lhook-scoped] injecting /usr/libexec/xpcproxy
```

SpawnBridge:

```sh
cat /var/tmp/Liter8SpawnBridge.log
```

Expected for a target daemon:

```text
[SpawnBridge] loaded pid=... direct-dyld-interpose
[SpawnBridge] pid=... target=/System/.../managedappdistributiond env=patched
```

There should be no broad stream of `[lhook] loaded into pid ...` for unrelated apps and daemons.

## Disable / recovery

Normal boot:

```sh
rm -f /var/jb/.lhook_scoped_enabled /var/jb/.lhook_scoped_debug
```

If recovery is needed from SSHRD:

```sh
rm -f /mnt2/jb/.lhook_scoped_enabled /mnt2/jb/.lhook_scoped_debug
```

To restore the previous lhook:

```sh
cp -p /mnt1/usr/lib/lhook.dylib.pre-scoped /mnt1/usr/lib/lhook.dylib
sync
```

## Build locally

Requires macOS + Xcode:

```sh
./build.sh
```

Both dylibs are built universal `arm64 + arm64e`.

## Status

Experimental. The scope is intentionally limited to the MarketplaceKit test path; do not broaden it to system-wide injection until the scoped path is proven stable.
