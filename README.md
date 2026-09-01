# Description

Skyrim Load Progress adds a progress meter to the loading screen. The meter uses the same artwork as the level progress bar and is placed directly below it.

> **Experimental branch:** `seamless-loading-experiment` keeps Skyrim's loading, fader, and mist update loops running but disables their presentation. This hides both Scaleform movies and suppresses MistMenu's native mist, background, and load-screen NIF rendering. It also disables form-backed image-space modifiers, including their cross-fades. The last completed frame is kept in a GPU texture and presented while the Loading Menu is open. Expect a frozen image followed by normal pop-in when rendering resumes.

This is still a proof of concept. The plugin currently tracks the reference, critical reference, and distant reference queues used while cells are loading. Optional diagnostics can write the queue activity and calculated progress to `SkyrimLoadProgress.log`.

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

The TOML file can disable the loading meter or control its safe-zone position and width. It also controls the blur shader, warm-cell fade timing, the default cold-cell transition, and ordered rules for cell editor IDs. Cell patterns are case-insensitive and support `*` and `?` wildcards. The first matching rule wins.

Starting a new game uses a dedicated black loading presentation. When MQ101 closes Loading Menu,
the plugin hands presentation back to Skyrim's native FaderMenu without modifying its FaderData.
This preserves the scripted `FadeOutGame(false, true, 14.0, 15.0)` hold and fade while allowing
TitleSequence Menu to render at its normal higher UI depth.

Loading diagnostics are disabled by default. Set `logging.loading` to write per-load and transition details. Set `logging.verbose_queues` as well to include individual queue mutations and aggregate progress samples. The `logging.loaded_entries` table can separately log normal object-reference work, references transferred between cells, and distant-reference work. Enabled entry categories include Form IDs and Editor IDs where available, plus an end-of-load tally. Startup messages, warnings, and errors are always logged.

Cold transitions can use the retained frame with an optional blur, or blend to a fixed or captured dominant color. Each cold rule can override `fade_in_ms`, `hold_after_load_ms`, and `fade_out_ms`. Values omitted from a rule inherit from the global `[cold]` table. Warm transitions are global and do not use cell rules.

Settings are read once when Skyrim finishes loading game data. Restart the game after changing the file.

## Current Limitations

Skyrim also reports background processing, task, and post-processing work in its loading diagnostics. Those queues are not included yet because their enqueue and completion points have not been identified with enough confidence.

The Loading Menu can also remain open after the tracked cell queues are finished. More loading stages may need to be added before the meter represents the entire load process.

## Debug Logging

Debug builds open a console and write trace messages to both the console and the SKSE log. Release builds only write to `SkyrimLoadProgress.log`.

The log contains:

* Loading Menu open and close events.
* Queue enqueue and completion activity.
* Completed, remaining, and total work.
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

For a release build:

```powershell
./build.ps1
```

For a debug build with the console enabled:

```powershell
./build-debug.ps1
```

## Packaging a Release

Build, validate, and create a Nexus-ready ZIP with a top-level `Data` directory:

```powershell
./Scripts/BuildRelease.ps1 -VsDevCmd J:\dev\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
```

The version defaults to `PROJECT_VERSION` from `CMakeLists.txt`. The archive is written to
`release/<version>/` and contains the GPL license, DLL, PDB, default TOML configuration, and both
Interface movie paths required by the plugin. Use `-SkipBuild` to package an existing validated
Release build.

## License

Skyrim Load Progress is licensed under the [GNU General Public License version 3 or
later](COPYING), matching CommonLibSSE-NG. See `COPYING` for the complete license terms.

## Reverse Engineering Notes

The current queue hooks were verified against Skyrim 1.6.1170, 1.7.99, and 1.7.104.
Other runtimes supported by Address Library are attempted on a best-effort basis using
runtime-family offsets and hook-site validation rather than a fixed runtime whitelist.

| Queue | Enqueue | Complete |
| --- | --- | --- |
| References | ID 19151 + `0x07` | ID 19152 + `0x0C` |
| Critical references | ID 19155 + `0x07` | ID 19156 + `0x0C` |
| Distant references | ID 19159 + `0x4E` | ID 19160 + `0x69` |

Each hook replaces a seven-byte `lock inc` or `lock dec` instruction with a CommonLib context hook.
