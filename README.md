# OpenRack

A minimal live plugin host in the spirit of Waves SuperRack, built with JUCE 8.

## Features
- Scan and host VST3 plugins (plus AudioUnit on macOS)
- Serial plugin chain: add, remove, reorder, bypass, open plugin windows
- Master gain
- Save/load racks (`.orack`) including plugin state
- Audio device selection

## Build
Requires CMake 3.22+ and a C++17 compiler. JUCE is fetched automatically.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Linux also needs the JUCE system dependencies (ALSA, X11, freetype, etc.).

## Using it
1. Click **Scan plugins** and scan your VST3 folders.
2. Double-click a plugin in the left list to add it to the rack.
3. Use Edit / Bypass / ^ / v / X on each rack row.
4. **Save rack** / **Load rack** store your setup.

## Roadmap (what SuperRack has that this doesn't yet)
- Multiple racks/snapshots with instant switching (SuperRack's "Songs/Snapshots")
- MIDI routing and instrument plugins, MIDI program-change/CC control of snapshots
- Parallel chains and per-slot input/output gain, meters, mute/solo
- Multi-channel / aggregate I/O routing
- Plugin sandboxing so a crashed plugin doesn't take down the app
- Latency reporting and compensation across parallel paths
