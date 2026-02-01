Alles gut 😄 verstanden — **wirklich alles in EINER einzigen README-Datei**, nichts ausgelagert, nichts referenziert.
Hier ist **die finale, vollständige README.md als eine Datei**.
👉 **Genau so speichern als:** `README.md`

---

```md
# LiveSpiff

LiveSpiff is a lightweight, Wayland-first speedrun timer for Linux, primarily targeting KDE Plasma.
It consists of a small D-Bus daemon (`livespiffd`) and a GTK4 frontend (`livespiff`).

> ⚠️ **PB (Personal Best) support is currently WIP (Work In Progress).**  
> PB data is stored locally, but the format and segment comparison logic may change in future versions.

---

## Features

### Timer
- Start / Split / Pause / Resume / Reset
- Millisecond precision timing
- States: `Idle`, `Running`, `Paused`, `Finished`

### Splits
- Custom split list (add, remove, rename)
- Splits displayed under the timer
- Current split is highlighted

### Segment timing
- Each split records a cumulative split time
- Segment time is calculated as:
```

segment_time[i] = split_time[i] - split_time[i - 1]

```

### Segment delta (vs PB) — WIP
- Each segment shows a delta compared to the PB segment
- **Delta convention:**
- **Negative value = slower**
- **Positive value = faster**

Example:
```

-2.50.231

````
This means you were **2.50.231 seconds slower** on that segment.

---

## Wayland & KDE Notes

- LiveSpiff does **not** grab global hotkeys directly on Wayland.
- Use **KDE Global Shortcuts** to bind hotkeys (recommended).
- Always-on-top overlays are compositor-managed:
  - Use **KDE Window Rules → “Keep Above Others”**

---

## Installation

LiveSpiff can be installed using the provided install script or built manually.

---

## Option 1: Install using the script (recommended)

```bash
chmod +x install.sh
./install.sh --all
````

This will:

* Install build & runtime dependencies (if supported by your distro)
* Build LiveSpiff
* Install binaries to `~/.local/bin`
* Create a desktop entry
* Enable a systemd **user** service for `livespiffd`

---

## Option 2: Manual build

### Dependencies

* meson
* ninja
* gcc or clang
* pkg-config / pkgconf
* glib-2.0
* json-glib-1.0
* gtk4
* qdbus6 (from qt6-tools / qt6-qttools)

### Build

```bash
meson setup build
meson compile -C build
```

---

## Running LiveSpiff

### Start the daemon (backend)

```bash
./build/livespiffd
```

or (if installed via script):

```bash
systemctl --user start livespiffd
```

### Start the GUI (frontend)

```bash
./build/livespiff
```

or:

```bash
livespiff
```

---

## KDE Global Shortcut Commands

Bind these commands in:
**System Settings → Shortcuts → Custom Shortcuts**

### Start / Split

```bash
qdbus6 com.livespiff.LiveSpiff /com/livespiff/LiveSpiff com.livespiff.LiveSpiff.Control.StartOrSplit
```

### Pause / Resume

```bash
qdbus6 com.livespiff.LiveSpiff /com/livespiff/LiveSpiff com.livespiff.LiveSpiff.Control.TogglePause
```

### Reset

```bash
qdbus6 com.livespiff.LiveSpiff /com/livespiff/LiveSpiff com.livespiff.LiveSpiff.Control.Reset
```

---

## Data & Config Locations

LiveSpiff follows the XDG Base Directory specification.

### UI config, splits & PB (WIP)

```
~/.config/livespiff/ui.ini
```

### Run file

```
~/.local/share/livespiff/runs/LiveSpiff_Run.json
```

---

## Roadmap

* Stable PB segment storage
* Segment delta vs last run
* Attempt history
* Full split table (segment time + delta + cumulative time)
* OBS-friendly text output
* Export / import runs
* Distribution packages (PKGBUILD, .deb)

---

## License

MIT

      
