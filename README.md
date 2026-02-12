# Smooth Scroll Daemon

Smooth scroll daemon for Linux VMs running under UTM, QEMU, or any SPICE-based hypervisor on macOS.

Transforms the choppy, jerky scrolling you get inside a Linux VM into buttery-smooth, macOS-like trackpad scrolling — with inertial deceleration, precise low-speed control, and proper hi-res scroll event support.

## The Problem

When you run a Linux VM (Ubuntu, Fedora, Arch, etc.) via UTM or QEMU on a MacBook, the host trackpad produces smooth, pixel-perfect scroll events. But by the time they reach the Linux guest, SPICE/QEMU has quantized them into discrete `REL_WHEEL ±1` events — each representing a full "line" of scrolling (120 hi-res units). The result:

- **Choppy scrolling** — content jumps in large steps instead of gliding smoothly
- **Way too fast** — a light flick sends content flying off the screen
- **No fine control** — impossible to scroll precisely by a few pixels
- **No inertia** — scrolling stops dead the instant your fingers lift

This is a [well-known problem](https://github.com/utmapp/UTM/issues/2400) that affects every Linux desktop environment under SPICE/QEMU, and none of the standard workarounds solve it properly.

## Why Existing Workarounds Fail

| Approach | Problem |
|---|---|
| **GNOME scroll speed settings** | [Don't exist](https://discourse.gnome.org/t/adding-scroll-speed-setting-in-gnome/25893). GNOME has no scroll sensitivity control — only touchpad pointer speed. Merge requests have been pending for 3+ years. |
| **imwheel** | X11 only. Doesn't work on Wayland. Difficult to configure. Multiplies discrete events, so low values make slow scrolling dead. |
| **libinput scroll-factor** | Requires compiling a [deprecated library](https://github.com/ptrsr/libinput-config). Simple linear multiplier — setting it low enough for fast scroll makes slow scroll unresponsive. |
| **Touchpad size hack** (`hwdb`) | Changes virtual touchpad dimensions to affect scroll speed, but [also breaks cursor tracking](https://ubuntuhandbook.org/index.php/2023/05/adjust-touchpad-scrolling-ubuntu/) — "Don't do it on production machine!" Doesn't work in Chromium/Electron apps. |
| **Guest OS settings** | [Minimal effect](https://github.com/utmapp/UTM/issues/2400) — the problem is at the evdev layer, below where desktop settings operate. |

**The fundamental issue**: all these approaches use a linear multiplier. A single fixed multiplier creates an unsolvable trade-off — set it low enough (0.08) for reasonable fast-scroll speed, and slow precise scrolling becomes completely dead.

## How Smooth Scroll Daemon Solves It

This daemon operates at the raw evdev layer, below libinput and the desktop environment. It works with any DE (GNOME, KDE, XFCE, etc.) on both X11 and Wayland.

### Non-Linear Velocity Curve

Instead of a linear multiplier, smooth-scroll uses input-rate-aware dampening:

- **Slow, precise scrolling** (1-5 events/sec) — scale factor near 1.0, full responsiveness
- **Normal scrolling** (5-30 events/sec) — gradual dampening via sqrt interpolation
- **Fast flick scrolling** (30+ events/sec) — heavy dampening (configurable, default 0.3x)

This means a single slow tick gives you fine pixel-level control, while a rapid flick doesn't send content flying.

### Inertial Deceleration

When you lift your fingers, content continues to glide with smooth exponential decay — matching macOS trackpad feel:

- **250 Hz emission rate** — smooth enough to eliminate visible stutter
- **Sub-pixel accumulation** — fractional hi-res units carry over between ticks, preventing micro-jitter
- **Absolute timer scheduling** — prevents drift accumulation for consistent frame timing
- **Configurable friction** — tune the deceleration half-life (default matches macOS feel)

### Proper Hi-Res Scroll Protocol

Emits both `REL_WHEEL_HI_RES` (for modern apps that support fine-grained scrolling) and `REL_WHEEL` at 120-unit boundaries (for compatibility with Firefox, Electron, older X11 toolkits). Same for horizontal axis.

### Zero-Latency Forwarding

Non-scroll events (pointer motion, button clicks, etc.) are forwarded immediately with no buffering. Only scroll events enter the smoothing pipeline.

## Quick Start

### Dependencies

```bash
# Ubuntu / Debian
sudo apt install build-essential libevdev-dev pkg-config

# Fedora
sudo dnf install gcc libevdev-devel pkg-config

# Arch
sudo pacman -S base-devel libevdev pkgconf
```

### Build & Run

```bash
git clone https://github.com/stronautt/smooth-scroll-daemon.git
cd smooth-scroll-daemon
make

# Run with auto-detection (finds SPICE/QEMU/VirtIO device automatically)
sudo ./smooth-scroll

# Run with explicit device path
sudo ./smooth-scroll /dev/input/event5
```

### Install as a Service

```bash
sudo make install
sudo systemctl enable --now smooth-scroll
```

#### To uninstall:

```bash
sudo systemctl disable --now smooth-scroll
sudo make uninstall
```

## Usage

```
Usage: smooth-scroll [OPTIONS] [DEVICE_PATH]

Options:
  -f, --friction FLOAT       Per-tick friction factor, 0.01-0.2 (default: 0.08)
                             Lower = longer glide, higher = stops faster.
  -t, --tick-ms INT          Timer tick interval in ms (default: 4)
      --low-rate FLOAT       No dampening below this events/sec (default: 5.0)
      --high-rate FLOAT      Max dampening above this events/sec (default: 30.0)
      --min-scale FLOAT      Scale factor at high input rate (default: 0.30)
      --stop-threshold FLOAT Velocity below which scrolling stops (default: 0.5)
  -m, --multiplier FLOAT     Global scroll distance multiplier (default: 0.5)
                             Lower = less scroll per gesture.
  -v, --verbose              Print debug info about intercepted/emitted events
  -h, --help                 Show this help
```

If no `DEVICE_PATH` is given, the daemon scans `/dev/input/event*` for a device whose name contains "spice", "qemu", or "virtio" (case-insensitive) with `REL_WHEEL` capability.

## Tuning Guide

The defaults are tuned for a macOS-like feel on a MacBook trackpad through UTM. Here's how to adjust:

### Scroll Distance Feels Too Large / Too Small

```bash
# Less scroll per gesture (finer control)
sudo ./smooth-scroll -m 0.3

# More scroll per gesture
sudo ./smooth-scroll -m 0.8

# Full 1:1 passthrough (no distance scaling)
sudo ./smooth-scroll -m 1.0
```

### Scrolling Stops Too Abruptly / Glides Too Long

```bash
# Longer glide after lifting fingers (lower friction)
sudo ./smooth-scroll -f 0.04

# Stops faster (higher friction)
sudo ./smooth-scroll -f 0.15

# Almost no inertia (very high friction)
sudo ./smooth-scroll -f 0.2
```

### Fast Scrolling Is Still Too Fast / Too Slow

```bash
# Heavier dampening on fast flicks
sudo ./smooth-scroll --min-scale 0.1

# Lighter dampening (faster flick = more scroll)
sudo ./smooth-scroll --min-scale 0.5

# Narrower speed range before dampening kicks in
sudo ./smooth-scroll --low-rate 3 --high-rate 20
```

### Debugging

Run with `-v` to see every intercepted and emitted event:

```bash
sudo ./smooth-scroll -v
```

Output shows input rate, scale factor, velocity, and emitted hi-res values — useful for finding the right tuning parameters.

### Identifying Your Device

If auto-detection doesn't find the right device:

```bash
# List all input devices
cat /proc/bus/input/devices

# Or use evtest to identify the scroll device
sudo evtest
```

Look for a device with "SPICE", "QEMU", or "VirtIO" in the name that has `REL_WHEEL` in its capabilities.

## How It Works

```
┌──────────────────────────────────────────────────────────────────┐
│  Host macOS trackpad                                             │
│  (smooth, pixel-perfect scroll events)                           │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  SPICE / QEMU                                                    │
│  (quantizes to discrete REL_WHEEL ±1, each = 120 hi-res units)   │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  smooth-scroll daemon                                            │
│                                                                  │
│  1. EVIOCGRAB source device (exclusive access)                   │
│  2. Forward non-scroll events immediately (zero latency)         │
│  3. Intercept scroll events:                                     │
│     - Track input rate (events/sec over 300ms window)            │
│     - Apply non-linear dampening based on rate                   │
│     - Scale by global multiplier                                 │
│     - Add to velocity accumulator                                │
│  4. 250 Hz timer emits smooth output:                            │
│     - Exponential friction decay (velocity *= 1-friction)        │
│     - Sub-pixel accumulation (no rounding jitter)                │
│     - Emit REL_WHEEL_HI_RES + REL_WHEEL at 120-unit boundaries   │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  uinput virtual device                                           │
│  → libinput → GNOME/KDE/XFCE → applications                      │
│  (smooth, fine-grained scroll events on both axes)               │
└──────────────────────────────────────────────────────────────────┘
```

### Architecture

- **Single-threaded** — `epoll` event loop monitoring two fds (source device + timerfd)
- **Single C file** — ~1000 lines, no external dependencies beyond `libevdev` and `libm`
- **Raw evdev layer** — works below libinput, compatible with any desktop environment and display server
- **Absolute timer scheduling** — `TFD_TIMER_ABSTIME` prevents drift accumulation

## Known Limitations

- **Horizontal scrolling** depends on SPICE/QEMU forwarding `REL_HWHEEL` events from the host. If the SPICE client (UTM) doesn't translate horizontal trackpad gestures, they won't reach the daemon. Vertical scrolling always works.
- **Very slow trackpad movement** may not generate any `REL_WHEEL` events from SPICE (the delta is too small to cross the quantization threshold). The daemon guarantees at least ±1 hi-res unit of output for every event that does arrive.
- **Requires root** for `EVIOCGRAB` (exclusive device grab) and `/dev/uinput` access.

## Requirements

- Linux kernel with evdev and uinput support (standard on all modern distros)
- `libevdev` development headers
- C99 compiler (GCC or Clang)
- Root access (for evdev grab and uinput)
