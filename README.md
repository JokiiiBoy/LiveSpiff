# LiveSpiff

LiveSpiff is a lightweight, Wayland-first speedrun timer for Linux.

It is designed specifically for modern Wayland desktops (KDE Plasma, GNOME, wlroots),
without relying on X11 hacks.

## Features

- Start / Split / Pause / Reset timer logic
- Wayland-safe global control via D-Bus
- KDE Global Shortcuts compatible
- Simple, fast, and extensible C codebase

## Architecture

- `livespiffd` runs as a background daemon
- Timer is controlled via D-Bus
- Desktop environment handles global shortcuts
- UI will be added later

## Build

### Dependencies
- glib-2.0
- gio-2.0
- meson
- gcc or clang

### Build steps

```bash
meson setup build
meson compile -C build
