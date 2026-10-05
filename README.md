# Description

Skyrim Load Progress adds a progress meter to the loading screen. The meter uses the same artwork as the level progress bar and is placed directly below it.

The transition compositor retains a completed world image across cell loading and blends back into gameplay. Community Shaders is optional; its verified 1.8.4 integration preserves post-processed lighting and temporarily suppresses frame generation while the compositor owns the image.

This is still a proof of concept. The plugin currently tracks the reference, critical reference, distant reference, background, IO task, and post-processing work used while cells are loading. Optional diagnostics can write the queue activity and calculated progress to `SkyrimLoadProgress.log`.

## How it Works

* Hooks the enqueue and completion counters used by Skyrim's cell loading queues.
* Starts a new progress calculation when the Loading Menu opens.
* Combines the tracked queues into a completed count and total count.
* Adds a second level meter to the Loading Menu and updates it from `LoadingMenu::AdvanceMovie`.
* Keeps the displayed value from moving backward when Skyrim discovers more work during the load.

The progress meter does not poll the queues. The queue totals are updated by the hooks where Skyrim changes the counters. `LoadingMenu::AdvanceMovie` is only used to display the latest calculated value.

## Configuration

Transition settings are read from:

```text
Data/SKSE/Plugins/SkyrimLoadProgress.toml
```

The TOML file can control the loading meter with `progress_bar.mode`: `"all"` shows it during mod-owned and vanilla loads, `"custom_only"` restricts it to the mod's custom presentations, and `"disabled"` hides it everywhere. Existing configurations using `progress_bar.enabled` remain supported when `mode` is absent. The table also controls the meter's safe-zone position and width. Other settings control the blur shader, warm-cell fade timing, the default cold-cell transition, and ordered rules for cell editor IDs. Cell patterns are case-insensitive and support `*` and `?` wildcards. The first matching rule wins.

The `[transitions]` table independently controls the mod's presentation for fast travel and loading from a save. Set `fast_travel = false` or `load_from_save = false` to retain Skyrim's vanilla LoadingMenu and FaderMenu for that path, with no retained-frame transition. The loading progress meter follows `progress_bar.mode`. Door transitions keep their existing behavior.

Starting a new game uses a dedicated black loading presentation. When MQ101 closes Loading Menu,
the plugin hands presentation back to Skyrim's native FaderMenu without modifying its FaderData.
This preserves the scripted `FadeOutGame(false, true, 14.0, 15.0)` hold and fade while allowing
TitleSequence Menu to render at its normal higher UI depth.

Loading diagnostics are disabled by default. TOML-enabled diagnostics use `info` verbosity in Release and Debug builds; enabling them never lowers the Release logger threshold to `debug` or `trace`. When disabled, diagnostic-only timing, control inspection, counters and GPU readbacks are skipped. Set `debugging.loading` to write per-load and transition details. Set `debugging.verbose_queues` as well to include individual queue mutations and aggregate progress samples. The `debugging.loaded_entries` table can separately log normal object-reference work, references transferred between cells, and distant-reference work. Enabled entry categories include Form IDs and Editor IDs where available, plus an end-of-load tally. `debugging.capture_transition_textures` saves diagnostic GPU textures and can stall rendering; keep it disabled for normal play and smoothness tests. Startup messages, warnings, and errors are always logged. Older `[logging]` configurations remain supported; `[debugging]` takes precedence when present.

Cold transitions can use the retained frame with an optional blur, or blend to a fixed or captured dominant color. Each cold rule can override `fade_in_ms`, `hold_after_load_ms`, and `fade_out_ms`. Values omitted from a rule inherit from the global `[cold]` table. Warm transitions are global and do not use cell rules.

Settings are read once when Skyrim finishes loading game data. Restart the game after changing the file.

## Current Limitations

Skyrim also reports background processing, tasks, and post-processing work in its loading diagnostics. The plugin now tracks the same three values through their paired counter mutations, including the final IOManager priority queue used for post-processing.

The Loading Menu can also remain open after the tracked cell queues are finished. More loading stages may need to be added before the meter represents the entire load process.

## Debug Logging

Debug builds open a console and write trace messages to both the console and the SKSE log. Release builds only write to `SkyrimLoadProgress.log`.

The log contains:

* Loading Menu open and close events.
* Queue enqueue and completion activity.
* Completed, remaining, and total work.
* A 250 ms progress heartbeat with callback, queue-idle, and meter-idle timing.
* Cell fully loaded events.

## Installation

Install the plugin, configuration, and meter assets to:

```text
Data/SKSE/Plugins/SkyrimLoadProgress.dll
Data/SKSE/Plugins/SkyrimLoadProgress.toml
Data/Interface/SkyrimLoadProgress/LoadingProgressMeter.swf
Data/Interface/Exported/SkyrimLoadProgress/LoadingProgressMeter.swf
```

## Requirements

* [SKSE64](https://skse.silverlock.org/)
* [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
* [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)

## Build Dependencies

* [CMake](https://cmake.org/)
* [vcpkg](https://github.com/microsoft/vcpkg)
* [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (included as a pinned submodule)

## Build Instructions

Clone the repository with submodules, or initialize them after cloning:

```powershell
git submodule update --init --recursive
```

Use an x64 Visual Studio developer shell with CMake and Ninja available and
`VCPKG_ROOT` pointing to your vcpkg installation. For a release build:

```powershell
cmake --preset build-release-msvc
cmake --build --preset release-msvc
```

For a debug build with the console enabled:

```powershell
cmake --preset build-debug-msvc
cmake --build --preset debug-msvc
```

## Packaging a Release

Package the validated Release DLL and PDB from `build/release-msvc/` with the contents
of `dist/`. The archive root represents Skyrim's `Data` directory, with `SKSE/` and
`Interface/` at the top level so both MO2 and Vortex install the files without an extra `Data/Data`
layer. The GPL license and release notice are installed under
`SKSE/Plugins/SkyrimLoadProgress/`, where they cannot affect plugin loading. The archive also
includes the DLL, PDB, default TOML configuration, and both Interface movie paths required by the
plugin.

## License

Copyright (C) 2026 ahzaab.

Skyrim Load Progress is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version.

Skyrim Load Progress is distributed in the hope that it will be useful, but **without any warranty**;
without even the implied warranty of merchantability or fitness for a particular purpose. See the
[GNU General Public License](LICENSE.txt) for details.

The complete corresponding source code is available in this
[public GitHub repository](https://github.com/ahzaab/sky-load-progress).

## Reverse Engineering Notes

The current queue hooks were verified against Skyrim 1.5.97, 1.6.1170, 1.7.99, and 1.7.104.
Other runtimes supported by Address Library are attempted on a best-effort basis using
runtime-family offsets and hook-site validation rather than a fixed runtime whitelist.

| Queue | Enqueue | Complete |
| --- | --- | --- |
| References | ID 19151 + `0x07` | ID 19152 + `0x0C` |
| Critical references | ID 19155 + `0x07` | ID 19156 + `0x0C` |
| Distant references | ID 19159 + `0x4E` | ID 19160 + `0x69` |

Each hook validates the complete `lock inc` or `lock dec` instruction before installation. When a
runtime uses an instruction shorter than the branch needed by the hook, the installer safely extends
the patch across complete relocatable instructions and replays the full original sequence.
