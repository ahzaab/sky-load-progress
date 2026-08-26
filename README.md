# Description

Skyrim Load Progress adds a progress meter to the loading screen. The meter uses the same artwork as the level progress bar and is placed directly below it.

> **Experimental branch:** `codex/seamless-loading-experiment` keeps Skyrim's loading, fader, and mist update loops running but disables their presentation. This hides both Scaleform movies and suppresses MistMenu's native mist, background, and load-screen NIF rendering. It also disables form-backed image-space modifiers, including their cross-fades. The loading loop calls Skyrim's world renderer once per `LoadingMenu::AdvanceMovie` update. Expect missing geometry, frozen frames, pop-in, missing gameplay effects, and other visual problems.

This is still a proof of concept. The plugin currently tracks the reference, critical reference, and distant reference queues used while cells are loading. It writes the queue activity and calculated progress to `SkyrimLoadProgress.log`.

## How it Works

* Hooks the enqueue and completion counters used by Skyrim's cell loading queues.
* Starts a new progress calculation when the Loading Menu opens.
* Combines the tracked queues into a completed count and total count.
* Adds a second level meter to the Loading Menu and updates it from `LoadingMenu::AdvanceMovie`.
* Keeps the displayed value from moving backward when Skyrim discovers more work during the load.

The progress meter does not poll the queues. The queue totals are updated by the hooks where Skyrim changes the counters. `LoadingMenu::AdvanceMovie` is only used to display the latest calculated value.

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

Install the DLL to:

```text
Data/SKSE/Plugins/SkyrimLoadProgress.dll
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

## Reverse Engineering Notes

The current queue hooks were verified against Skyrim 1.6.1170 and 1.7.99.

| Queue | Enqueue | Complete |
| --- | --- | --- |
| References | ID 19151 + `0x07` | ID 19152 + `0x0C` |
| Critical references | ID 19155 + `0x07` | ID 19156 + `0x0C` |
| Distant references | ID 19159 + `0x4E` | ID 19160 + `0x69` |

Each hook replaces a seven-byte `lock inc` or `lock dec` instruction with a CommonLib context hook.
