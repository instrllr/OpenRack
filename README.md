# OpenRack

A live plugin host in the spirit of Waves SuperRack, built with JUCE 8. It runs two ways:

- **Standalone app** - opens your audio interface and runs the rack on it.
- **Plugin (VST3 / AU)** - load OpenRack on a mixer channel in FL Studio, Ableton Live (VST3) or Logic Pro (AU) and host your effects inside it.

## Features
- Scan and host effect plugins (VST3, plus AudioUnit on macOS); instruments are hidden
- **Up to 64 stereo racks** (like SuperRack Performer), each with its own plugin chain, fader, mute, solo and meters. A rack is patched from any stereo **input pair** to any stereo **output pair** (standalone: your interface's channels, up to 128 in / 128 out; plugin: the DAW's 64 input and 64 output buses)
- **Patch bay**: a console-style 64 x 64 stereo (128 x 128 channel) crosspoint matrix (**Patch** button): input sockets to rack inputs and rack outputs to output sockets - click a crosspoint to patch, click again to remove; Patch 1:1 and Clear all for quick starts
- **Direct I/O**: a third patch-bay view routes any input socket straight to any output socket with no rack in between (for passing channels through, e.g. on a SoundGrid setup). Saved with the show and snapshots
- **MIDI hardware control** (**MIDI** button): MIDI learn for faders, mute and solo buttons, master fader and snapshot buttons; map faders 1-N to racks 1-N in one pass; bank switching for more racks than faders (any strip count, so 8-fader boards like the AKAI MIDImix and bigger surfaces both work); soft takeover; LED and motorized-fader feedback to the controller's MIDI output; a Mackie Control preset. Mappings are stored per computer
- **Channel EQ in every rack** (not a plugin in the chain): high-pass, low shelf, two peaking bands and a high shelf ahead of the plugins, with a response-curve editor (drag bands, scroll for Q, double-click to flatten). Saved with the rack and snapshots
- **Rack names and group tabs** (a vertical strip at the bottom-left): double-click a rack to name it; group racks under tabs (right-click a rack > Move to group, the + tab, or **Add racks to group...** to tick any existing racks into the open tab) so a big session stays easy to scan. Groups are saved with the session
- Serial plugin chain per rack: add, remove, reorder, bypass, open plugin windows (inline when running in a DAW)
- **Drag and drop**: drag plugins from the list or the hot strip into the rack at an exact position, drag rack rows to reorder, or drop a `.vst3` / `.component` file on the window
- **Hot plugins**: star a plugin to pin it, and your most-used plugins appear automatically in a one-click strip under the toolbar and in the **Hot plugins** menu
- Input and output meters with peak hold and clip lights, master gain, latency reporting to the DAW
- Search bar, Bypass all, Clear rack, tooltips
- **Shows library**: **Save show** stores the whole show (all racks, patching, EQ, snapshots, plugin state) as a `.oshow` in the app's shows library; the **Shows** button opens a full-size library view to open, rename, delete, import or export shows (old `.orack` files still open). The show is also saved inside your DAW project
- **64 stereo input and output buses** (VST3): switch them on in your DAW, trim each input from the **Inputs** view, and patch racks to them. FL Studio can send mixer tracks to any input; Logic's AU exposes the main input plus its one side-chain and a single output
- **Snapshots**: 8 stored sessions (all racks, patching and faders). Same plugins = instant recall; otherwise plugins reload. They are also the plugin's 8 programs, so a DAW can automate them
- **Isolated plugins (experimental, standalone app)**: tick **Isolate plugins** and every plugin you load afterwards runs in its own helper process. If a plugin crashes or hangs OpenRack keeps running, the slot passes audio through, and the plugin is restarted automatically with its last state
- **Crash protection**: new plugins are test-loaded in a separate process first (standalone app), plugins that crash while loading get blocked, and the standalone app offers to restore your last rack after an unexpected exit

## Build
Requires CMake 3.22+ and a C++17 compiler. JUCE is fetched automatically.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Linux also needs the JUCE system dependencies (ALSA, X11, freetype, etc.).
GitHub Actions builds all three platforms and runs auval / pluginval on the plugin.

## Using it
1. Standalone: open **Audio settings** and enable your interface's channels. Add racks on the right, then pick each rack's **In** and **Out** pair.
2. Click **Scan plugins** (or drop a plugin file on the window).
3. Double-click a plugin, press Enter in the search box, or drag it into the rack.
4. Click the star beside plugins you reach for often to pin them as hot plugins.
5. Use Edit / Bypass / ^ / v / X on each rack row, or drag the row to reorder.

## Roadmap
- MIDI routing and instrument plugins
- MIDI / OSC control of snapshots, faders and bypass
- Parallel chains and per-slot gain
- Out-of-process hosting for the VST3 / AU plugin versions (today it is standalone-app only) and per-rack instead of per-plugin processes

# DISCLAIMER

This software is vibecoded.
if this is not your cup of tea, you do not have to use it.
i am simply making this mostly for myself but it is available to anyone who wants to use this software for their live events and do not want to pay for Waves SuperRack Performer.

- **MIDI activity light** in the status bar (green flash on incoming MIDI, accent ring when a binding fires; click to open MIDI setup).
- **Global tempo** (20-500 BPM) in the toolbar: tap tempo (also bindable to a MIDI button), slider or +/- buttons. Hosted plugins receive it as host tempo (standalone app).
