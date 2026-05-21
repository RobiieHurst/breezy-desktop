# Breezy Desktop Hyprland Backend

This is the Hyprland compositor plugin backend for Breezy Desktop.

Current status: early scaffold. The plugin loads, verifies Hyprland plugin API compatibility, checks `/dev/shm/breezy_desktop_imu` for XR pose data, discovers Hyprland monitors/headless outputs, and posts load/unload notifications. XR rendering will be added incrementally.

## Build

```bash
make -C hyprland
```

## Load Manually

Run from inside a Hyprland session:

```bash
hyprctl plugin load "$PWD/hyprland/build/libbreezy-hyprland.so"
```

Unload with:

```bash
hyprctl plugin unload "$PWD/hyprland/build/libbreezy-hyprland.so"
```

## Runtime Controls

Recenter the anchored XR desktop with:

```bash
hyprctl breezy-recenter
```

Or bind it in `hyprland.conf`:

```ini
bind = SUPER SHIFT, B, breezy_recenter
```

## Tuning

The plugin exposes tuning values through Hyprland config. These defaults match the current tested behavior:

```ini
plugin:breezy:screen_scale = 0.95
plugin:breezy:smoothing_alpha = 0.45
plugin:breezy:max_step_ratio = 0.35
plugin:breezy:deadzone = 0.01
plugin:breezy:movement_limit = 6.0
```

Reload Hyprland config after changing values.

## Fallback Mirroring

Native texture compositing is preferred. If needed for debugging, you can use Hyprland's monitor mirroring to show the Breezy virtual output on the headset display:

```bash
hyprland/bin/breezy_hyprland_mirror_virtual
```

Undo mirroring with:

```bash
hyprland/bin/breezy_hyprland_mirror_virtual --undo
```

## Install With hyprpm

From the repository root:

```bash
hyprpm add "$PWD"
hyprpm enable breezy-desktop
hyprpm reload
```

If Hyprland was updated, rebuild/reload the plugin. Hyprland plugins must be built against matching Hyprland headers.
