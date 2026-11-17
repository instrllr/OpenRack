# OpenRack

A live plugin host in the spirit of Waves SuperRack, built with JUCE 8. It runs two ways:

- **Standalone app** - opens your audio interface and runs the rack on it.
- **Plugin (VST3 / AU)** - load OpenRack on a mixer channel in FL Studio, Ableton Live (VST3) or Logic Pro (AU) and host your effects inside it.

## Features
- Scan and host effect plugins (VST3, plus AudioUnit on macOS); instruments are hidden
- Serial plugin chain: add, remove, reorder, bypass, open plugin windows (inline when running in a DAW)
- **Drag and drop**: drag plugins from the list or the hot strip into the rack at an exact position, drag rack rows to reorder, or drop a `.vst3` / `.component` file on the window
- **Hot plugins**: star a plugin to pin it, and your most-used plugins appear automatically in a one-click strip under the toolbar and in the **Hot plugins** menu
- Input and output meters with peak hold and clip lights, master gain, latency reporting to the DAW
- Search bar, Bypass all, Clear rack, tooltips
- Save/load racks (`.orack`) including plugin state; the rack is also saved inside your DAW project

## Build
Requires CMake 3.22+ and a C++17 compiler. JUCE is fetched automatically.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Linux also needs the JUCE system dependencies (ALSA, X11, freetype, etc.).
GitHub Actions builds all three platforms and runs auval / pluginval on the plugin.

## Using it
1. Click **Scan plugins** (or drop a plugin file on the window).
2. Double-click a plugin, press Enter in the search box, or drag it into the rack.
3. Click the star beside plugins you reach for often to pin them as hot plugins.
4. Use Edit / Bypass / ^ / v / X on each rack row, or drag the row to reorder.

## Roadmap
- Snapshots with instant switching
- MIDI routing and instrument plugins
- Parallel chains and per-slot gain
- Plugin sandboxing

# DISCLAIMER

This software is made entirely by vibecoding.
if this is not your cup of tea, you do not have to use it.
i am simply making this mostly for myself but it is available to anyone who wants to use this software for their live events and do not want to pay for Waves SuperRack Performer.
