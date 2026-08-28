// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"
#include "LoadingProgress.h"
#include "Settings.h"

namespace load_progress
{
    // Returns the singleton that owns loading aggregation and event handling.
    LoadingProgress& LoadingProgress::GetSingleton()
    {
        static LoadingProgress singleton;
        return singleton;
    }

    // Sums the directly observed work that is still queued across all pools.
    std::uint64_t LoadingProgress::GetLiveRemaining()
    {
        std::uint64_t remaining = 0;
        for (const auto& queue : liveRemaining) {
            remaining += queue.load(std::memory_order_relaxed);
        }
        return remaining;
    }

    // Writes a numeric member to an ActionScript object.
    void LoadingProgress::SetNumber(RE::GFxValue& a_object, const char* a_name, double a_value)
    {
        if (!a_name || !a_object.IsObject()) {
            return;
        }

        RE::GFxValue value;
        value.SetNumber(a_value);
        a_object.SetMember(a_name, value);
    }

    // Reads a numeric member from an ActionScript object.
    bool LoadingProgress::GetNumber(const RE::GFxValue& a_object, const char* a_name, double& a_value)
    {
        if (!a_name || !a_object.IsObject()) {
            return false;
        }

        RE::GFxValue value;
        if (!a_object.GetMember(a_name, &value) || !value.IsNumber()) {
            return false;
        }
        a_value = value.GetNumber();
        return true;
    }

    // Converts one root-movie point into the meter container's local coordinate space.
    bool LoadingProgress::ConvertGlobalPointToLocal(
        RE::GFxMovieView* a_view, RE::GFxValue& a_parent, double& a_x, double& a_y)
    {
        if (!a_view || !a_parent.IsObject()) {
            return false;
        }

        RE::GFxValue point;
        a_view->CreateObject(&point);
        if (!point.IsObject()) {
            return false;
        }
        SetNumber(point, "x", a_x);
        SetNumber(point, "y", a_y);

        RE::GFxValue ignored;
        if (!a_parent.Invoke("globalToLocal", &ignored, &point, 1) ||
            !GetNumber(point, "x", a_x) || !GetNumber(point, "y", a_y)) {
            return false;
        }

        return true;
    }

    // Converts one coordinate-space point into Stage-global coordinates.
    bool LoadingProgress::ConvertLocalPointToGlobal(
        RE::GFxMovieView* a_view, RE::GFxValue& a_coordinateSpace, double& a_x, double& a_y)
    {
        if (!a_view || !a_coordinateSpace.IsObject()) {
            return false;
        }

        RE::GFxValue point;
        a_view->CreateObject(&point);
        if (!point.IsObject()) {
            return false;
        }
        SetNumber(point, "x", a_x);
        SetNumber(point, "y", a_y);

        RE::GFxValue ignored;
        if (!a_coordinateSpace.Invoke("localToGlobal", &ignored, &point, 1) ||
            !GetNumber(point, "x", a_x) || !GetNumber(point, "y", a_y)) {
            return false;
        }

        return true;
    }

    // Reads a movie clip's visual bounds in the requested coordinate space.
    bool LoadingProgress::GetClipBounds(
        RE::GFxValue& a_clip, RE::GFxValue& a_coordinateSpace, std::array<double, 4>& a_bounds)
    {
        if (!a_clip.IsObject() || !a_coordinateSpace.IsObject()) {
            return false;
        }

        RE::GFxValue bounds;
        if (!a_clip.Invoke("getBounds", &bounds, &a_coordinateSpace, 1) || !bounds.IsObject()) {
            return false;
        }

        return GetNumber(bounds, "xMin", a_bounds[0]) && GetNumber(bounds, "yMin", a_bounds[1]) &&
               GetNumber(bounds, "xMax", a_bounds[2]) && GetNumber(bounds, "yMax", a_bounds[3]);
    }

    // Reads a clip's bounds and explicitly converts the result from root-local to Stage-global space.
    bool LoadingProgress::GetGlobalClipBounds(
        RE::GFxMovieView*      a_view,
        RE::GFxValue&          a_clip,
        RE::GFxValue&          a_root,
        std::array<double, 4>& a_bounds)
    {
        if (!a_view || !a_clip.IsObject() || !a_root.IsObject()) {
            return false;
        }

        if (!GetClipBounds(a_clip, a_root, a_bounds)) {
            return false;
        }

        double left = a_bounds[0];
        double top = a_bounds[1];
        double right = a_bounds[2];
        double bottom = a_bounds[3];
        if (!ConvertLocalPointToGlobal(a_view, a_root, left, top) ||
            !ConvertLocalPointToGlobal(a_view, a_root, right, bottom)) {
            return false;
        }

        a_bounds = { std::min(left, right), std::min(top, bottom),
            std::max(left, right), std::max(top, bottom) };
        return true;
    }

    // Converts the frame's target width into the scale needed by the animated center section.
    double LoadingProgress::CalculateMeterXScale(
        double a_originalXScale, double a_originalFrameWidth, double a_targetFrameWidth)
    {
        // Character 5 spans -2200 through 2209 twips. Its scaling grid spans -1800 through 1800,
        // leaving 809 twips across the two fixed end-cap regions.
        constexpr double authoredFrameWidth = 4409.0;
        constexpr double authoredStretchWidth = 3600.0;
        constexpr double minimumStretchWidth = 1.0;

        const auto fixedWidthRatio = (authoredFrameWidth - authoredStretchWidth) / authoredFrameWidth;
        const auto fixedCapWidth = a_originalFrameWidth * fixedWidthRatio;
        const auto originalStretchWidth = a_originalFrameWidth - fixedCapWidth;
        const auto targetStretchWidth = std::max(minimumStretchWidth, a_targetFrameWidth - fixedCapWidth);

        return a_originalXScale * targetStretchWidth / originalStretchWidth;
    }

    // Sizes and positions the standalone meter inside LoadingMenu's safe rectangle.
    bool LoadingProgress::ApplyProgressBarLayout(
        RE::GFxMovieView* a_view,
        RE::GFxValue&     a_parent,
        RE::GFxValue&     a_layoutClip,
        RE::GFxValue&     a_meter,
        RE::GFxValue&     a_frame,
        RE::GFxValue&     a_boundsClip)
    {
        if (!a_view || !a_parent.IsObject() || !a_layoutClip.IsObject() || !a_meter.IsObject() ||
            !a_frame.IsObject() || !a_boundsClip.IsObject()) {
            return false;
        }

        RE::GFxValue root;
        if (!a_view->GetVariable(&root, "_root") || !root.IsObject()) {
            return false;
        }

        // GFx reports the safe rectangle in root-movie coordinates, not Stage-global coordinates.
        const auto safeRect = a_view->GetSafeRect();
        double     safeLeft = safeRect.left;
        double     safeTop = safeRect.top;
        double     safeRight = safeRect.right;
        double     safeBottom = safeRect.bottom;
        if (!ConvertLocalPointToGlobal(a_view, root, safeLeft, safeTop) ||
            !ConvertLocalPointToGlobal(a_view, root, safeRight, safeBottom)) {
            return false;
        }

        const auto safeMinimumX = std::min(safeLeft, safeRight);
        const auto safeMinimumY = std::min(safeTop, safeBottom);
        const auto safeMaximumX = std::max(safeLeft, safeRight);
        const auto safeMaximumY = std::max(safeTop, safeBottom);
        const auto safeWidth = safeMaximumX - safeMinimumX;
        const auto safeHeight = safeMaximumY - safeMinimumY;
        double     originalMeterXScale = 100.0;
        double     originalFrameWidth = 0.0;
        double     originalBoundsWidth = 0.0;
        if (safeWidth <= 0.0 || safeHeight <= 0.0 ||
            !GetNumber(a_meter, "_xscale", originalMeterXScale) ||
            !GetNumber(a_frame, "_width", originalFrameWidth) ||
            !GetNumber(a_boundsClip, "_width", originalBoundsWidth) || originalMeterXScale <= 0.0 ||
            originalFrameWidth <= 0.0 || originalBoundsWidth <= 0.0) {
            return false;
        }

        const auto& layout = Settings::GetSingleton().GetProgressBar();
        auto        frameWidth = originalFrameWidth * layout.widthPercent / 100.0;
        auto        boundsWidth = originalBoundsWidth * layout.widthPercent / 100.0;

        // Frame_mc owns the scaling grid. Bounds_mc mirrors the requested visible width for layout.
        SetNumber(a_layoutClip, "_xscale", 100.0);
        SetNumber(a_frame, "_width", frameWidth);
        SetNumber(a_boundsClip, "_width", boundsWidth);

        // Measure globally because the meter's parent can carry its own translation and scale.
        std::array<double, 4> globalBounds{};
        if (!GetGlobalClipBounds(a_view, a_boundsClip, root, globalBounds)) {
            return false;
        }

        auto globalMeterWidth = globalBounds[2] - globalBounds[0];
        auto globalMeterHeight = globalBounds[3] - globalBounds[1];
        if (globalMeterWidth <= 0.0 || globalMeterHeight <= 0.0) {
            return false;
        }

        // Bounds_mc is invisible and mirrors the visible frame width, giving layout a stable measurement
        // while Meter_mc changes frames.
        if (globalMeterWidth > safeWidth) {
            const auto safeScale = safeWidth / globalMeterWidth;
            frameWidth *= safeScale;
            boundsWidth *= safeScale;
            SetNumber(a_frame, "_width", frameWidth);
            SetNumber(a_boundsClip, "_width", boundsWidth);

            if (!GetGlobalClipBounds(a_view, a_boundsClip, root, globalBounds)) {
                return false;
            }
            globalMeterWidth = globalBounds[2] - globalBounds[0];
            globalMeterHeight = globalBounds[3] - globalBounds[1];
        }

        // Only the center of Frame_mc stretches. Scale the animated fill by that center's change rather
        // than the frame's overall percentage so it continues to meet the fixed end caps.
        const auto meterXScale =
            CalculateMeterXScale(originalMeterXScale, originalFrameWidth, frameWidth);
        SetNumber(a_meter, "_xscale", meterXScale);

        const auto targetGlobalLeft = safeMinimumX +
                                      std::max(0.0, safeWidth - globalMeterWidth) * layout.xPercent / 100.0;
        const auto targetGlobalTop = safeMinimumY +
                                     std::max(0.0, safeHeight - globalMeterHeight) * layout.yPercent / 100.0;

        double targetLocalLeft = targetGlobalLeft;
        double targetLocalTop = targetGlobalTop;
        double boundsLocalLeft = globalBounds[0];
        double boundsLocalTop = globalBounds[1];
        if (!ConvertGlobalPointToLocal(a_view, a_parent, targetLocalLeft, targetLocalTop) ||
            !ConvertGlobalPointToLocal(a_view, a_parent, boundsLocalLeft, boundsLocalTop)) {
            return false;
        }

        double currentX = 0.0;
        double currentY = 0.0;
        if (!GetNumber(a_layoutClip, "_x", currentX) || !GetNumber(a_layoutClip, "_y", currentY)) {
            return false;
        }

        const auto requestedLocalX = currentX + targetLocalLeft - boundsLocalLeft;
        const auto requestedLocalY = currentY + targetLocalTop - boundsLocalTop;

        // The external movie owns layout while its Meter_mc child changes timeline frames.
        SetNumber(a_layoutClip, "_x", requestedLocalX);
        SetNumber(a_layoutClip, "_y", requestedLocalY);

        if (!GetGlobalClipBounds(a_view, a_boundsClip, root, globalBounds)) {
            return false;
        }

        double appliedX = 0.0;
        double appliedY = 0.0;
        if (!GetNumber(a_layoutClip, "_x", appliedX) || !GetNumber(a_layoutClip, "_y", appliedY)) {
            return false;
        }

        logger::info(
            "positioned loading meter: x={:.1f}% y={:.1f}% width={:.1f}% "
            "localPosition=({:.1f}, {:.1f})->({:.1f}, {:.1f}) "
            "globalSafe=({:.1f}, {:.1f})-({:.1f}, {:.1f}) globalBounds=({:.1f}, {:.1f})-({:.1f}, {:.1f})",
            layout.xPercent, layout.yPercent, layout.widthPercent, currentX, currentY, appliedX, appliedY,
            safeMinimumX, safeMinimumY, safeMaximumX, safeMaximumY, globalBounds[0], globalBounds[1],
            globalBounds[2], globalBounds[3]);
        return true;
    }

    // Loads and initializes the standalone progress-meter movie.
    bool LoadingProgress::CreateProgressBar(RE::GFxMovieView* a_view)
    {
        if (!a_view) {
            return false;
        }

        RE::GFxValue root;
        if (!a_view->GetVariable(&root, "_root") || !root.IsObject()) {
            return false;
        }

        RE::GFxValue container;
        if (!root.GetMember("SkyrimLoadProgress", &container) || !container.IsObject()) {
            RE::GFxValue depth;
            if (!root.Invoke("getNextHighestDepth", &depth, nullptr, 0) || !depth.IsNumber()) {
                logger::warn("could not obtain a depth for the standalone loading meter");
                return false;
            }

            RE::GFxValue createArguments[2];
            createArguments[0].SetString("SkyrimLoadProgress");
            createArguments[1] = depth;
            if (!root.Invoke("createEmptyMovieClip", &container, createArguments, 2) ||
                !container.IsObject()) {
                logger::warn("could not create the standalone loading meter container");
                return false;
            }

            RE::GFxValue moviePath;
            moviePath.SetString("SkyrimLoadProgress/LoadingProgressMeter.swf");
            RE::GFxValue ignored;
            if (!container.Invoke("loadMovie", &ignored, &moviePath, 1)) {
                logger::warn("could not request the standalone loading meter movie");
                return false;
            }

            logger::info("requested SkyrimLoadProgress/LoadingProgressMeter.swf");
            return false;
        }

        RE::GFxValue progressBar;
        RE::GFxValue meterFrame;
        RE::GFxValue layoutBounds;
        if (!container.GetMember("Meter_mc", &progressBar) || !progressBar.IsObject() ||
            !container.GetMember("Frame_mc", &meterFrame) || !meterFrame.IsObject() ||
            !container.GetMember("Bounds_mc", &layoutBounds) || !layoutBounds.IsObject()) {
            return false;
        }

        double emptyFrame = 0.0;
        if (GetNumber(progressBar, "_slpEmptyFrame", emptyFrame)) {
            return true;
        }

        double totalFrames = 0.0;
        if (!GetNumber(progressBar, "_totalframes", totalFrames) || totalFrames <= 1.0) {
            return false;
        }

        RE::GFxValue ignored;
        RE::GFxValue frameArgument;
        frameArgument.SetString("Empty");
        if (!progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1) ||
            !GetNumber(progressBar, "_currentframe", emptyFrame)) {
            return false;
        }
        frameArgument.SetString("Full");
        if (!progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1)) {
            return false;
        }
        double fullFrame = emptyFrame;
        if (!GetNumber(progressBar, "_currentframe", fullFrame) || fullFrame == emptyFrame) {
            return false;
        }
        // Some loading menu skins place Full before Empty on the timeline.
        SetNumber(progressBar, "_slpEmptyFrame", emptyFrame);
        SetNumber(progressBar, "_slpFullFrame", fullFrame);

        // Use a stable frame when measuring bounds; the external root remains independent of the meter timeline.
        frameArgument.SetString("Empty");
        if (!progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1)) {
            return false;
        }
        if (!ApplyProgressBarLayout(a_view, root, container, progressBar, meterFrame, layoutBounds)) {
            logger::warn("could not position the standalone loading meter");
            return false;
        }

        logger::info("initialized standalone loading meter; frames empty={:.0f} full={:.0f}",
            emptyFrame, fullFrame);
        return true;
    }

    // Maps a percentage onto the external meter's labeled timeline frames.
    bool LoadingProgress::SetMeterPercent(RE::GFxValue& a_meter, double a_percent)
    {
        double emptyFrame = 0.0;
        double fullFrame = 0.0;
        if (!GetNumber(a_meter, "_slpEmptyFrame", emptyFrame) ||
            !GetNumber(a_meter, "_slpFullFrame", fullFrame) || fullFrame == emptyFrame) {
            return false;
        }

        const auto   percent = std::clamp(a_percent, 0.0, 100.0);
        const auto   frame = std::floor(emptyFrame + (fullFrame - emptyFrame) * percent / 100.0);
        RE::GFxValue ignored;
        RE::GFxValue argument;
        argument.SetNumber(frame);
        return a_meter.Invoke("gotoAndStop", &ignored, &argument, 1);
    }

    // Creates the progress meter on demand and advances it monotonically.
    void LoadingProgress::UpdateProgressBar(RE::IMenu* a_menu)
    {
        if (!a_menu || !a_menu->uiMovie) {
            return;
        }

        RE::GFxValue progressBar;
        if (!a_menu->uiMovie->GetVariable(
                &progressBar, "_root.SkyrimLoadProgress.Meter_mc") ||
            !progressBar.IsObject()) {
            if (!CreateProgressBar(a_menu->uiMovie.get())) {
                return;
            }
            if (!a_menu->uiMovie->GetVariable(
                    &progressBar, "_root.SkyrimLoadProgress.Meter_mc") ||
                !progressBar.IsObject()) {
                return;
            }
        }

        double emptyFrame = 0.0;
        if (!GetNumber(progressBar, "_slpEmptyFrame", emptyFrame) &&
            !CreateProgressBar(a_menu->uiMovie.get())) {
            return;
        }

        const auto basisPoints = displayedBasisPoints.load(std::memory_order_acquire);
        if (!SetMeterPercent(progressBar, static_cast<double>(basisPoints) / 100.0)) {
            static bool warned = false;
            if (!warned) {
                logger::warn("external loading meter has invalid Empty/Full frame labels");
                warned = true;
            }
        }
    }

    // Applies queue deltas outside the engine's queue-mutation call paths.
    void LoadingProgress::DrainQueueMutations()
    {
        if (!epochActive.load(std::memory_order_acquire)) {
            return;
        }

        std::scoped_lock lock(stateLock);
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto enqueued = pendingEnqueued[i].exchange(0, std::memory_order_acq_rel);
            const auto completed = pendingCompleted[i].exchange(0, std::memory_order_acq_rel);
            const auto queue = static_cast<Queue>(i);

            if (enqueued != 0) {
                aggregator.Enqueue(queue, enqueued);
            }
            if (completed != 0) {
                aggregator.Complete(queue, completed);
            }
        }
        LogProgress(aggregator.Current());
    }

    // Updates the progress widget and hides Scaleform for warm transitions.
    void LoadingProgress::LoadingMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
    {
        if (originalAdvanceMovie) {
            originalAdvanceMovie(a_menu, a_interval, a_currentTime);
        }

        if (!hooksEnabled.load(std::memory_order_acquire)) {
            return;
        }

        try {
            DrainQueueMutations();

            if (a_menu && a_menu->uiMovie) {
                const bool seamless = CellTransitioner::IsSeamless();
                a_menu->uiMovie->SetBackgroundAlpha(0.0F);
                a_menu->uiMovie->SetVisible(!seamless);
                if (!seamless) {
                    UpdateProgressBar(a_menu);
                }
            }
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in LoadingMenu::AdvanceMovie");
        }
    }

    // Locks the source frame and configures the selected presentation when the menu opens.
    RE::UI_MESSAGE_RESULTS LoadingProgress::LoadingMenuProcessMessage(RE::IMenu* a_menu, RE::UIMessage& a_message)
    {
        if (hooksEnabled.load(std::memory_order_acquire) &&
            a_message.type == RE::UI_MESSAGE_TYPE::kShow) {
            try {
                CellTransitioner::PrepareForLoad(a_menu);
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in LoadingMenu::ProcessMessage");
            }
        }
        return originalLoadingProcessMessage ?
                   originalLoadingProcessMessage(a_menu, a_message) :
                   RE::UI_MESSAGE_RESULTS::kPassOn;
    }

    void LoadingProgress::LogProgress(const Progress& a_progress)
    {
        const auto candidate = a_progress.total ?
                                   static_cast<std::uint32_t>(std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) :
                                   0u;
        auto       displayed = displayedBasisPoints.load(std::memory_order_relaxed);
        // Newly queued work can lower the raw fraction, but the meter should not move backward.
        while (candidate > displayed &&
               !displayedBasisPoints.compare_exchange_weak(
                   displayed, candidate, std::memory_order_release, std::memory_order_relaxed)) {}
        if (a_progress.total == lastLogged.total && a_progress.completed == lastLogged.completed &&
            a_progress.remaining == lastLogged.remaining) {
            return;
        }
        logger::info("load progress completed={} remaining={} total={} percent={:.1f}",
            a_progress.completed, a_progress.remaining, a_progress.total, a_progress.fraction * 100.0);
        lastLogged = a_progress;
    }

    // Disables plugin behavior while leaving every installed hook as a pass-through.
    void LoadingProgress::DisableHooks(std::string_view a_reason) noexcept
    {
        hooksEnabled.store(false, std::memory_order_release);
        epochActive.store(false, std::memory_order_release);

        if (!failureLogged.exchange(true, std::memory_order_acq_rel)) {
            try {
                logger::critical("loading-progress hooks disabled: {}", a_reason);
            } catch (...) {
                REX::W32::OutputDebugStringA("Skyrim Load Progress: loading-progress hooks disabled\n");
            }
        }
    }

    // Records a direct queue increment without locking or logging on the engine worker thread.
    void LoadingProgress::OnEnqueue(Queue a_queue) noexcept
    {
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount || !hooksEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        liveRemaining[index].fetch_add(1, std::memory_order_relaxed);
        if (epochActive.load(std::memory_order_relaxed)) {
            pendingEnqueued[index].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Records a direct queue decrement without locking or logging on the engine worker thread.
    void LoadingProgress::OnComplete(Queue a_queue) noexcept
    {
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount || !hooksEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        auto& live = liveRemaining[index];
        auto  value = live.load(std::memory_order_relaxed);
        while (value != 0 && !live.compare_exchange_weak(value, value - 1, std::memory_order_relaxed)) {}
        if (epochActive.load(std::memory_order_relaxed)) {
            pendingCompleted[index].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Context-hook adapters identify which engine queue changed.
    void LoadingProgress::CriticalEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::criticalReferences); }
    void LoadingProgress::CriticalComplete(CONTEXT&) noexcept { OnComplete(Queue::criticalReferences); }
    void LoadingProgress::ReferenceEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::references); }
    void LoadingProgress::ReferenceComplete(CONTEXT&) noexcept { OnComplete(Queue::references); }
    void LoadingProgress::DistantEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::distantReferences); }
    void LoadingProgress::DistantComplete(CONTEXT&) noexcept { OnComplete(Queue::distantReferences); }

    // Installs direct hooks on the three decoded reference queue mutations.
    void InstallMutationHooks()
    {
        struct MutationHook
        {
            REL::RelocationID id;
            RuntimeOffset     offset;
            void (*callback)(CONTEXT&) noexcept;
            std::string_view            name;
            std::array<std::uint8_t, 7> expectedBytes;
        };

        const auto mutationHooks = std::to_array<MutationHook>({ { IDs::CriticalReferencesEnqueue, Offsets::CriticalReferencesEnqueue,
                                                                     LoadingProgress::CriticalEnqueue, "critical enqueue",
                                                                     { 0xF0, 0xFF, 0x80, 0x6C, 0x01, 0x00, 0x00 } },
            { IDs::CriticalReferencesComplete, Offsets::CriticalReferencesComplete,
                LoadingProgress::CriticalComplete, "critical completion",
                { 0xF0, 0xFF, 0x88, 0x6C, 0x01, 0x00, 0x00 } },
            { IDs::ReferencesEnqueue, Offsets::ReferencesEnqueue,
                LoadingProgress::ReferenceEnqueue, "reference enqueue",
                { 0xF0, 0xFF, 0x80, 0x70, 0x01, 0x00, 0x00 } },
            { IDs::ReferencesComplete, Offsets::ReferencesComplete,
                LoadingProgress::ReferenceComplete, "reference completion",
                { 0xF0, 0xFF, 0x88, 0x70, 0x01, 0x00, 0x00 } },
            { IDs::DistantReferencesEnqueue, Offsets::DistantReferencesEnqueue,
                LoadingProgress::DistantEnqueue, "distant enqueue",
                { 0xF0, 0xFF, 0x81, 0x74, 0x01, 0x00, 0x00 } },
            { IDs::DistantReferencesComplete, Offsets::DistantReferencesComplete,
                LoadingProgress::DistantComplete, "distant completion",
                { 0xF0, 0xFF, 0x88, 0x74, 0x01, 0x00, 0x00 } } });

        constexpr std::size_t counterInstructionSize = 7;
        constexpr std::size_t minimumTrampolineBytes = 8 * 1024;
        auto&                 trampoline = SKSE::GetTrampoline();
        if (trampoline.free_size() < minimumTrampolineBytes) {
            throw std::runtime_error(fmt::format(
                "queue hooks require at least {} free trampoline bytes; {} remain",
                minimumTrampolineBytes, trampoline.free_size()));
        }

        const auto                    text = REL::Module::get().segment(REL::Segment::textx);
        std::array<std::uintptr_t, 6> addresses{};
        for (std::size_t i = 0; i < mutationHooks.size(); ++i) {
            const auto& hook = mutationHooks[i];
            const auto  functionAddress = REL::Relocation<std::uintptr_t>(hook.id).address();
            const auto  offset = hook.offset.Get();
            if (!functionAddress || offset == 0 || !hook.callback) {
                throw std::runtime_error(fmt::format("could not resolve the {} hook", hook.name));
            }

            const auto address = functionAddress + offset;
            const auto withinText = address >= text.address() &&
                                    address + counterInstructionSize <= text.address() + text.size();
            if (!withinText ||
                std::memcmp(reinterpret_cast<const void*>(address), hook.expectedBytes.data(),
                    hook.expectedBytes.size()) != 0) {
                throw std::runtime_error(fmt::format(
                    "{} hook bytes did not match runtime {} at {:X}", hook.name,
                    REL::Module::get().version().string("."), address));
            }
            addresses[i] = address;
        }

        // All six sites are verified before the first instruction is patched.
        for (std::size_t i = 0; i < mutationHooks.size(); ++i) {
            const auto& hook = mutationHooks[i];
            const auto  address = addresses[i];
            if (!SKSE::stl::install_context_hook(
                    address, counterInstructionSize, hook.callback, counterInstructionSize)) {
                throw std::runtime_error(fmt::format("could not install {} hook at {:X}", hook.name, address));
            }
            logger::info("installed direct {} hook at {:X}", hook.name, address);
        }
    }

    // Installs the LoadingMenu message and movie-advance hooks.
    void InstallLoadingMenuHook()
    {
        // CommonLib's IMenu vtable maps slot 4 to ProcessMessage and slot 5 to AdvanceMovie.
        constexpr std::size_t processMessageIndex = 0x04;
        constexpr std::size_t advanceMovieIndex = 0x05;

        REL::Relocation<std::uintptr_t> vtable{ RE::LoadingMenu::VTABLE[0] };
        if (!vtable.address()) {
            throw std::runtime_error("could not resolve the LoadingMenu vtable");
        }

        const auto processAddress = *reinterpret_cast<const std::uintptr_t*>(
            vtable.address() + processMessageIndex * sizeof(std::uintptr_t));
        const auto advanceAddress = *reinterpret_cast<const std::uintptr_t*>(
            vtable.address() + advanceMovieIndex * sizeof(std::uintptr_t));
        if (!CellTransitioner::IsExecutableAddress(processAddress) ||
            !CellTransitioner::IsExecutableAddress(advanceAddress)) {
            throw std::runtime_error("LoadingMenu had an invalid original vtable function");
        }

        LoadingProgress::originalLoadingProcessMessage =
            reinterpret_cast<LoadingProgress::ProcessMessage_t>(processAddress);
        LoadingProgress::originalAdvanceMovie =
            reinterpret_cast<LoadingProgress::AdvanceMovie_t>(advanceAddress);

        vtable.write_vfunc(processMessageIndex, LoadingProgress::LoadingMenuProcessMessage);
        vtable.write_vfunc(advanceMovieIndex, LoadingProgress::LoadingMenuAdvanceMovie);

        logger::info("installed hidden LoadingMenu::AdvanceMovie experiment hook");
    }

    void LoadingProgress::SeedQueuedWork()
    {
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto baseline = liveRemaining[i].load(std::memory_order_relaxed);
            aggregator.Enqueue(static_cast<Queue>(i), baseline);
        }
    }

    // Starts aggregation when Skyrim opens LoadingMenu.
    void LoadingProgress::BeginLoadingEpoch()
    {
        std::scoped_lock lock(stateLock);

        displayedBasisPoints.store(0, std::memory_order_release);
        for (std::size_t i = 0; i < queueCount; ++i) {
            pendingEnqueued[i].store(0, std::memory_order_relaxed);
            pendingCompleted[i].store(0, std::memory_order_relaxed);
        }

        aggregator.Begin();
        SeedQueuedWork();

        lastLogged = {};
        epochActive.store(true, std::memory_order_release);
        CellTransitioner::BeginLoad();

        logger::info(
            "loading epoch began: Loading Menu opened; baseline remaining={}", aggregator.Current().remaining);
        LogProgress(aggregator.Current());
    }

    // Ends aggregation and starts the retained-frame transition into gameplay.
    void LoadingProgress::EndLoadingEpoch()
    {
        std::scoped_lock lock(stateLock);

        epochActive.store(false, std::memory_order_release);
        for (std::size_t i = 0; i < queueCount; ++i) {
            pendingEnqueued[i].store(0, std::memory_order_relaxed);
            pendingCompleted[i].store(0, std::memory_order_relaxed);
        }
        CellTransitioner::EndLoad();

        const auto final = aggregator.Current();
        logger::info("loading epoch ended: Loading Menu closed; completed={} remaining={} total={}",
            final.completed, final.remaining, final.total);
        aggregator.End();
    }

    // Starts or ends an aggregation epoch with LoadingMenu's lifetime.
    RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
    {
        if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (!hooksEnabled.load(std::memory_order_acquire)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            a_event->opening ? BeginLoadingEpoch() : EndLoadingEpoch();
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in LoadingMenu event sink");
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    // Logs the fully-loaded milestone while a transition is being observed.
    RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
        const RE::TESCellFullyLoadedEvent* a_event,
        RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
    {
        if (!hooksEnabled.load(std::memory_order_acquire)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            if (CellTransitioner::renderObservationState.load(std::memory_order_acquire) != 0 && a_event &&
                a_event->cell) {
                const auto* editorID = a_event->cell->GetFormEditorID();
                logger::info("cell fully loaded: formID={:08X} editorID='{}' menuOpen={} liveRemaining={}",
                    a_event->cell->GetFormID(), editorID ? editorID : "",
                    epochActive.load(std::memory_order_acquire), GetLiveRemaining());
            }
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in cell-loaded event sink");
        }

        return RE::BSEventNotifyControl::kContinue;
    }
    // Resets all counters and starts a new loading epoch.
    void LoadingProgress::Aggregator::Begin()
    {
        remaining_.fill(0);
        total_.fill(0);
        progress_ = {};
        active_ = true;
    }

    // Adds one unit of work to a tracked queue.
    void LoadingProgress::Aggregator::Enqueue(
        LoadingProgress::Queue a_queue, std::uint64_t a_count)
    {
        if (!active_ || a_count == 0) {
            return;
        }
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount) {
            return;
        }

        remaining_[index] += a_count;
        total_[index] += a_count;
        logger::debug("queue '{}' enqueued {} item(s)", queueNames[index], a_count);
        Recalculate();
    }

    // Completes one unit of work, including work first seen at completion.
    void LoadingProgress::Aggregator::Complete(
        LoadingProgress::Queue a_queue, std::uint64_t a_count)
    {
        if (!active_ || a_count == 0) {
            return;
        }
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount) {
            return;
        }

        const auto observed = std::min(remaining_[index], a_count);
        const auto unobserved = a_count - observed;
        remaining_[index] -= observed;
        total_[index] += unobserved;
        logger::debug("queue '{}' completed {} item(s), including {} unobserved",
            queueNames[index], a_count, unobserved);
        Recalculate();
    }

    // Rebuilds aggregate progress from the per-queue counters.
    void LoadingProgress::Aggregator::Recalculate()
    {
        progress_.total = 0;
        progress_.remaining = 0;
        for (std::size_t i = 0; i < queueCount; ++i) {
            progress_.total += total_[i];
            progress_.remaining += remaining_[i];
        }
        progress_.completed = progress_.total - progress_.remaining;
        progress_.fraction = progress_.total ?
                                 static_cast<double>(progress_.completed) / static_cast<double>(progress_.total) :
                                 0.0;
    }

    // Returns the most recently calculated aggregate progress.
    LoadingProgress::Progress LoadingProgress::Aggregator::Current() const { return progress_; }

    // Stops accepting queue mutations for the current epoch.
    void LoadingProgress::Aggregator::End() { active_ = false; }

    // Installs loading progress hooks and registers the singleton event sink.
    void InstallHooks()
    {
        if (!Runtimes::IsSupported()) {
            throw std::runtime_error(fmt::format("unsupported Skyrim runtime {}",
                REL::Module::get().version().string(".")));
        }

        auto& events = LoadingProgress::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!ui || !eventSources) {
            throw std::runtime_error("could not find Skyrim's UI or script event source holder");
        }

        InstallMutationHooks();
        InstallLoadingMenuHook();

        ui->AddEventSink<RE::MenuOpenCloseEvent>(&events);
        eventSources->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
        LoadingProgress::hooksEnabled.store(true, std::memory_order_release);

        logger::info("installed loading-menu and cell-fully-loaded event sinks");
    }
}
