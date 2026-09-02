// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "ProgressMeter.h"
#include "Settings.h"

namespace load_progress
{
    // Returns the singleton responsible for the Loading Menu's standalone meter.
    ProgressMeter& ProgressMeter::GetSingleton()
    {
        static ProgressMeter singleton;
        return singleton;
    }

    // Writes a numeric member to an ActionScript object.
    void ProgressMeter::SetNumber(RE::GFxValue& a_object, const char* a_name, double a_value)
    {
        if (!a_name || !a_object.IsObject()) {
            return;
        }

        RE::GFxValue value;
        value.SetNumber(a_value);
        a_object.SetMember(a_name, value);
    }

    // Reads a numeric member from an ActionScript object.
    bool ProgressMeter::GetNumber(const RE::GFxValue& a_object, const char* a_name, double& a_value)
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
    bool ProgressMeter::ConvertGlobalPointToLocal(
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
    bool ProgressMeter::ConvertLocalPointToGlobal(
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
    bool ProgressMeter::GetClipBounds(
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
    bool ProgressMeter::GetGlobalClipBounds(
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
    double ProgressMeter::CalculateMeterXScale(
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
    bool ProgressMeter::ApplyLayout(
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

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info(
                "positioned loading meter: x={:.1f}% y={:.1f}% width={:.1f}% "
                "localPosition=({:.1f}, {:.1f})->({:.1f}, {:.1f}) "
                "globalSafe=({:.1f}, {:.1f})-({:.1f}, {:.1f}) globalBounds=({:.1f}, {:.1f})-({:.1f}, {:.1f})",
                layout.xPercent, layout.yPercent, layout.widthPercent, currentX, currentY, appliedX, appliedY,
                safeMinimumX, safeMinimumY, safeMaximumX, safeMaximumY, globalBounds[0], globalBounds[1],
                globalBounds[2], globalBounds[3]);
        }
        return true;
    }

    // Mirrors LoadingMenu's native Menu_mc fade, which carries the level meter. Replacement menus that
    // omit that clip receive the same 20-frame, 30-FPS linear fade authored by Bethesda.
    void ProgressMeter::ApplyFade(
        RE::GFxMovieView* a_view, RE::GFxValue& a_container, float a_interval)
    {
        if (!a_view || !a_container.IsObject()) {
            return;
        }

        double nativeAlpha = 0.0;
        RE::GFxValue nativeMenu;
        if (a_view->GetVariable(&nativeMenu, "_root.Menu_mc") && nativeMenu.IsObject() &&
            GetNumber(nativeMenu, "_alpha", nativeAlpha)) {
            SetNumber(a_container, "_alpha", std::clamp(nativeAlpha, 0.0, 100.0));
            return;
        }

        constexpr double nativeFadeDuration = 20.0 / 30.0;
        double elapsed = 0.0;
        GetNumber(a_container, "_slpFadeElapsed", elapsed);
        elapsed = std::min(nativeFadeDuration,
            elapsed + std::max(0.0, static_cast<double>(a_interval)));
        SetNumber(a_container, "_slpFadeElapsed", elapsed);
        SetNumber(a_container, "_alpha", 100.0 * elapsed / nativeFadeDuration);
    }

    // Loads and initializes the standalone progress-meter movie.
    bool ProgressMeter::Create(RE::GFxMovieView* a_view)
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
            SetNumber(container, "_alpha", 0.0);
            SetNumber(container, "_slpFadeElapsed", 0.0);

            moviePath.SetString("SkyrimLoadProgress/LoadingProgressMeter.swf");
            RE::GFxValue ignored;
            if (!container.Invoke("loadMovie", &ignored, &moviePath, 1)) {
                logger::warn("could not request the standalone loading meter movie");
                return false;
            }

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("requested SkyrimLoadProgress/LoadingProgressMeter.swf");
            }
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
        if (!ApplyLayout(a_view, root, container, progressBar, meterFrame, layoutBounds)) {
            logger::warn("could not position the standalone loading meter");
            return false;
        }

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info("initialized standalone loading meter; frames empty={:.0f} full={:.0f}",
                emptyFrame, fullFrame);
        }
        return true;
    }

    // Maps a percentage onto the external meter's labeled timeline frames.
    bool ProgressMeter::SetPercent(RE::GFxValue& a_meter, double a_percent)
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

    // Creates the progress meter on demand and applies the supplied aggregate percentage.
    void ProgressMeter::Update(RE::IMenu* a_menu, double a_percent, float a_interval)
    {
        // Keep queue tracking active, but do not create the external movie when its UI is disabled.
        if (!Settings::GetSingleton().GetProgressBar().enabled ||
            !a_menu || !a_menu->uiMovie) {
            return;
        }

        RE::GFxValue progressBar;
        if (!a_menu->uiMovie->GetVariable(
                &progressBar, "_root.SkyrimLoadProgress.Meter_mc") ||
            !progressBar.IsObject()) {
            if (!Create(a_menu->uiMovie.get())) {
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
            !Create(a_menu->uiMovie.get())) {
            return;
        }

        RE::GFxValue container;
        if (a_menu->uiMovie->GetVariable(
                &container, "_root.SkyrimLoadProgress") &&
            container.IsObject()) {
            ApplyFade(a_menu->uiMovie.get(), container, a_interval);
        }

        if (!SetPercent(progressBar, a_percent)) {
            static bool warned = false;
            if (!warned) {
                logger::warn("external loading meter has invalid Empty/Full frame labels");
                warned = true;
            }
        }
    }
}
