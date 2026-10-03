# Liter8SpawnBridge — superseded

This experiment is **superseded by the integrated lhook implementation in**
[gru2007/Liter8-iPad8](https://github.com/gru2007/Liter8-iPad8).

Do not install the old v0.1/v0.2 SpawnBridge packages on a normal Liter8 boot.
Those builds were useful for tracing the iOS 26 xpcproxy spawn path, but they
added an unnecessary extra injection layer.

The integrated Liter8 implementation now uses ElleKit's own architecture:

```text
launchd
  └─ /usr/lib/lhook (PID 1 only)
       ├─ xpcproxy -> ElleKit pspawn.dylib
       │                └─ final process -> ElleKit libinjector/TweakLoader
       └─ direct launchd child -> TweakLoader
```

The PID 1 lhook no longer propagates itself into child processes.

ElleKit's normal tweak filters decide which tweak dylibs are loaded in the final
process, so Safari/SpringBoard/application tweaks do not require hard-coded
process entries in lhook.

Current implementation:

```text
gru2007/Liter8-iPad8/device/launchdhook/lhook.c
```

This repository is retained only as the diagnostic history that led to that fix.
