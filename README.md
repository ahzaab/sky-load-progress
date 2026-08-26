# Skyrim Load Progress

An SKSE/CommonLibSSE-NG proof of concept for measuring cell-load progress.

The current plugin writes loading epoch, queue discovery/completion, aggregate progress, and fully-loaded milestones to `SkyrimLoadProgress.log`. It does not alter any menu or game state.

The first runtime adapter observes the three counters decoded in `RE::LOADED_CELL_DATA`. The aggregator already defines channels for the additional background-processing, task, and post-processing queues found in the engine; those channels remain zero until stable cross-runtime accessors are implemented.

