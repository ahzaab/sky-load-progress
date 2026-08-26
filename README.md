# Skyrim Load Progress

An SKSE/CommonLibSSE-NG proof of concept for measuring cell-load progress.

The current plugin writes loading epoch, queue enqueue/completion, aggregate progress, and fully-loaded milestones to `SkyrimLoadProgress.log`. It does not alter any menu or game state and does not schedule a recurring task.

The first runtime adapter hooks the exact atomic mutations of the three counters decoded in `RE::LOADED_CELL_DATA`. Address Library IDs and instruction offsets were verified in preserved, typed IDA databases for Skyrim 1.6.1170 and 1.7.99:

| Queue | Enqueue | Complete |
| --- | --- | --- |
| references | ID 19151 + `0x07` | ID 19152 + `0x0C` |
| critical references | ID 19155 + `0x07` | ID 19156 + `0x0C` |
| distant references | ID 19159 + `0x4E` | ID 19160 + `0x69` |

All six patched instructions are seven-byte `lock inc`/`lock dec` operations. The aggregator defines channels for the additional background-processing, task, and post-processing queues found in the engine diagnostic routine (Address Library ID 13065); those channels remain zero until their producer and completion mutations are identified with the same confidence.

Debug builds allocate a Windows console during plugin initialization and attach the stdout sink to CommonLib's logger after `SKSE::Init`. Trace messages are flushed immediately to both the console and the normal SKSE log file. Release builds remain file-only.

The proof of concept also hooks `LoadingMenu::AdvanceMovie` at vtable slot `0x05` for presentation only. The original engine update runs first; the post-update pass injects a native Scaleform movie clip named `_root.SkyrimLoadProgress` and scales its stamina-green fill from the event-driven aggregate. Queue state is never polled from this hook.

Because newly discovered work can increase the denominator during a load, the raw diagnostic fraction may decrease. The displayed bar uses a per-epoch monotonic high-water mark, stored in basis points, so late enqueue events cannot move it backward.
