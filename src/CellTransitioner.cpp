// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "Atomic.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"
#include "LoadingProgress.h"
#include "ScopedSpritePipelineState.h"
#include "TransitionUiOverlay.h"
#include "StartupBlackCover.h"
#include "RetainedHdrConversion.h"
#include "FrozenFrameShader.h"

#include <hde64.h>
#include <DirectXPackedVector.h>
#include <ScreenGrab.h>
#include <intrin.h>

// Cell transition injection overview
//
// Skyrim normally stops presenting the world as a continuous scene while LoadingMenu, FaderMenu,
// MistMenu, and loading fades cover the cell change. The transition compositor leaves the engine's
// loading work and render scheduling alone while replacing only the load-owned presentation:
//
// 1. InstallWorldCaptureHook finds the Scaleform-begin call by its Address Library callee, then patches
//    that call in Skyrim's UI render path. The original call binds the UI render target; our callback
//    then copies that target before Scaleform draws. This produces a rolling world-only frame without
//    the HUD, console, or other Scaleform movies.
// 2. LoadingMenu's show hook calls PrepareForLoad. It selects a warm or cold presentation, locks the
//    last complete world frame, and prevents later loading frames from replacing it. BeginLoad opens
//    the compositor gate when the LoadingMenu open event arrives.
// 3. With CS, the post-processing hook captures/composites the completed scene before UI. Present
//    supplies an opaque watchdog when that pass is skipped, refilling CS's HDR input before ApplyHDR.
//    The HDR dispatch observer retains that display-converted output, then places it below native
//    foreground UI after the UI's HDR conversion. Its opaque UI coverage hides generated-world
//    distortion through the hold and crossfade without patching CS instructions. Vanilla uses the
//    completed back buffer directly.
// 4. During a warm load the locked frame replaces the back buffer. During a cold load the current
//    Scaleform output is copied aside, the locked world is blurred or blended toward its dominant
//    color, and the LoadingMenu layer is drawn back on top.
// 5. EndLoad closes the loading gate but keeps the frame locked. Present then fades the retained image
//    or transition color over the newly rendered cell. Once the fade ends, the frame is unlocked and
//    rolling capture resumes.
//
// FaderMenu tracking hides only native load fades during the transition window. Scripted fades and
// image-space modifiers keep their original behavior. MistMenu hooks remove its mist/model layer, and
// the RenderWorld hook is observational; it does not force the renderer to run.

namespace load_progress
{
    namespace
    {
        TransitionUiOverlay transitionUiOverlay;
        RetainedHdrConversion retainedHdrConversion;
        thread_local bool captureHdrConversionThisPresent{};

        // Mutated only with debugging.loading enabled. Keep per-Present coverage
        // separate from phase-change reports so a single missing pass is visible.
        struct TransitionFrameDiagnostics
        {
            std::uint64_t frame{};
            bool insidePresent{};
            bool insideChain{};
            bool owned{};
            unsigned displays{};
            unsigned snapshots{};
            unsigned uiPasses{};
            unsigned unpairedUi{};
            unsigned compositions{};
            unsigned sceneCopies{};
            unsigned frozenCopies{};
            unsigned sceneDispatches{};
            unsigned displayCopies{};
            std::uintptr_t displayTexture{};
        };
        thread_local TransitionFrameDiagnostics transitionFrameDiagnostics;

        enum class NativeLoadPath : std::uint8_t
        {
            none,
            door,
            fastTravel,
            loadSave
        };

        NativeLoadPath GetNativeLoadPath(const RE::FaderData& a_data)
        {
            if (!a_data.unk10) {
                return NativeLoadPath::none;
            }

            const auto callbackVtable =
                *reinterpret_cast<const std::uintptr_t*>(a_data.unk10);
            static REL::Relocation<std::uintptr_t> normalDoor{
                RE::VTABLE___NormalDoorFadeCallback[0]
            };
            static REL::Relocation<std::uintptr_t> autoDoor{
                RE::VTABLE___AutoDoorFadeCallback[0]
            };
            static REL::Relocation<std::uintptr_t> fastTravel{
                RE::VTABLE___FadeThenFastTravelCallback[0]
            };
            static REL::Relocation<std::uintptr_t> loadSave{
                RE::VTABLE___FadeThenLoadCallback[0]
            };

            if (callbackVtable == normalDoor.address() || callbackVtable == autoDoor.address()) {
                return NativeLoadPath::door;
            }

            if (callbackVtable == fastTravel.address()) {
                return NativeLoadPath::fastTravel;
            }

            if (callbackVtable == loadSave.address()) {
                return NativeLoadPath::loadSave;
            }

            return NativeLoadPath::none;
        }

        bool UsesCustomTransition(NativeLoadPath a_path)
        {
            const auto& settings = Settings::GetSingleton();
            if (a_path == NativeLoadPath::fastTravel) {
                return settings.UseTransitionsForFastTravel();
            }

            if (a_path == NativeLoadPath::loadSave) {
                return settings.UseTransitionsForSaveLoads();
            }

            return true;
        }

    }

    /**
     * @brief Returns the singleton that owns all cell-transition state.
     */
    CellTransitioner& CellTransitioner::GetSingleton()
    {
        static CellTransitioner singleton;
        return singleton;
    }

    /**
     * @brief Disables transition hooks and restores presentation mutated while the compositor owned the
     * screen.
     */
    void CellTransitioner::DisableHooks(std::string_view a_reason) noexcept
    {
        // Coordinate with LoadingProgress so one fail-closed path cannot leave the other armed.
        DisablePlugin(a_reason);
    }

    /**
     * @brief Clears compositor atomics and restores Fader/HUD. Invoked only from DisablePlugin.
     */
    void CellTransitioner::ResetOnDisable() noexcept
    {
        Atomic::set(hooksEnabled, false);
        Atomic::set(epochActive, false);
        Atomic::set(frozenFrameLocked, false);
        Atomic::set(preLoadDoorCaptureLocked, false);
        Atomic::set(preLoadDoorTransitionActive, false);
        Atomic::set(preLoadOwnedFader, false);
        Atomic::set(vanillaLoadPending, false);
        Atomic::set(fastTravelBlackPending, false);
        Atomic::set(fastTravelBlackActive, false);
        Atomic::set(loadOwnedFader, false);
        Atomic::set(loadFaderCloseQueued, false);
        Atomic::set(sleepFadeRequestDeadline, 0);
        Atomic::set(sleepFaderActive, false);
        Atomic::set(awaitingControlRestore, false);
        Atomic::set(newGameTransitionActive, false);
        Atomic::set(newGameFadeRequestSeen, false);
        Atomic::set(mainMenuLoadPending, false);
        Atomic::set(mainMenuLoadActive, false);
        Atomic::set(postLoadFadePending, false);
        Atomic::set(postLoadReleasePending, false);
        Atomic::set(postLoadRecoveryDeadline, 0);
        Atomic::set(postLoadFadeStart, 0);
        Atomic::set(worldRenderedSincePresent, false);
        Atomic::set(dominantColorPending, false);
        Atomic::set(loadingTransitionStart, 0);
        Atomic::set(loadingMenuFadeElapsedMs, 0);

        // FaderMenuAdvanceMovie returns early once hooksEnabled is false, so any movie we hid for
        // load ownership must be restored here. Every UI/D3D pointer is treated as possibly null.
        try {
            if (Atomic::get(faderPresentationSuppressed)) {

                auto* ui = RE::UI::GetSingleton();
                if (!ui) {
                    Atomic::set(faderPresentationSuppressed, false);
                } else {

                    auto menu = ui->GetMenu(RE::FaderMenu::MENU_NAME);
                    if (menu && menu->uiMovie) {
                        RestoreFaderPresentation(menu.get());
                    } else {
                        Atomic::set(faderPresentationSuppressed, false);
                    }
                }
            }
        } catch (...) {
            Atomic::set(faderPresentationSuppressed, false);
            REX::W32::OutputDebugStringA(
                "Skyrim Load Progress: could not restore FaderMenu presentation during disable\n");
        }

        RestoreHUDVisibility();
    }

    /**
     * @brief Checks that a callback target is committed executable memory.
     */
    bool CellTransitioner::IsExecutableAddress(std::uintptr_t a_address) noexcept
    {
        if (!a_address) {
            return false;
        }

        REX::W32::MEMORY_BASIC_INFORMATION memory{};
        if (!REX::W32::VirtualQuery(reinterpret_cast<const void*>(a_address), &memory, sizeof(memory)) ||
            memory.state != MEM_COMMIT) {
            return false;
        }

        if ((memory.protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }

        const auto protection = memory.protect & 0xFFU;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    // Finds one rel32 call to the expected callee inside an Address Library function.
    //
    // Address Library provides stable identities for the caller and callee, but it does not identify
    // a particular call instruction inside the caller. The instruction's offset changes when Bethesda
    // rebuilds the function, so adding a runtime-specific constant would make the hook unnecessarily
    // fragile. This resolver instead searches the caller for the call whose decoded destination is the
    // expected Address Library function.
    //
    /**
     * @brief Finds the unique relative call from the relocated caller to the callee, rejecting ambiguous
     * or invalid sites.
     *
     * @details RtlLookupFunctionEntry reads the executable's x64 unwind table. Its begin/end RVAs provide the
     * exact compiled-function boundary, keeping the search out of adjacent functions and
     * padding. The resolver fails closed if the boundary is invalid, the call is absent, or
     * more than one matching call exists. A future runtime therefore disables plugin
     * initialization instead of patching an uncertain instruction.
     */
    std::uintptr_t CellTransitioner::FindUniqueRelativeCall(
        REL::RelocationID a_callerID,
        REL::RelocationID a_calleeID,
        std::string_view  a_name)
    {
        const auto caller = REL::Relocation<std::uintptr_t>(a_callerID).address();
        const auto callee = REL::Relocation<std::uintptr_t>(a_calleeID).address();
        if (!IsExecutableAddress(caller) || !IsExecutableAddress(callee)) {
            throw std::runtime_error(fmt::format("could not resolve the {} caller or callee", a_name));
        }

        // x64 unwind entries store RVAs, so add the image base returned with the entry to recover the
        // live addresses after ASLR.
        DWORD64     imageBase = 0;
        const auto* runtimeFunction = ::RtlLookupFunctionEntry(caller, &imageBase, nullptr);
        if (!runtimeFunction || imageBase == 0) {
            throw std::runtime_error(fmt::format("could not determine the {} function bounds", a_name));
        }

        const auto functionBegin = imageBase + runtimeFunction->BeginAddress;
        const auto functionEnd = imageBase + runtimeFunction->EndAddress;
        const auto text = REL::Module::get().segment(REL::Segment::textx);
        const auto textEnd = text.address() + text.size();
        if (functionBegin != caller || functionBegin >= functionEnd ||
            functionBegin < text.address() || functionEnd > textEnd) {
            throw std::runtime_error(fmt::format("{} had invalid runtime function bounds", a_name));
        }

        constexpr std::size_t relativeCallSize = 5;
        std::uintptr_t        match = 0;

        // Walk decoded instruction boundaries rather than scanning individual bytes. An E8 value can
        // occur inside an immediate or displacement belonging to another instruction and must never
        // be treated as a patch site. HDE64 is the same compact decoder CommonLib uses to validate
        // trampoline patches.
        for (auto instruction = functionBegin; instruction < functionEnd;) {
            hde64s     decoded{};
            const auto length = hde64_disasm(reinterpret_cast<const void*>(instruction), &decoded);
            if (length == 0 || (decoded.flags & F_ERROR) != 0 || instruction + length > functionEnd) {
                throw std::runtime_error(fmt::format(
                    "could not decode {} at {:X}", a_name, instruction));
            }

            // E8 encodes CALL rel32 as a one-byte opcode followed by a signed displacement from the
            // end of the five-byte instruction. Copy the unaligned displacement instead of casting an
            // int32_t pointer, then reproduce the CPU's destination calculation.
            if (decoded.opcode == 0xE8 && length == relativeCallSize) {

                std::int32_t displacement = 0;
                std::memcpy(&displacement,
                    reinterpret_cast<const void*>(instruction + 1), sizeof(displacement));
                const auto target = instruction + relativeCallSize + displacement;
                if (target == callee) {

                    // Multiple calls to the same helper would make the intended semantic position
                    // ambiguous. Refuse the hook instead of guessing which occurrence is correct.
                    if (match != 0) {
                        throw std::runtime_error(fmt::format(
                            "{} contained more than one call to {:X}", a_name, callee));
                    }

                    match = instruction;
                }
            }

            instruction += length;
        }

        if (match == 0) {
            throw std::runtime_error(fmt::format(
                "{} contained no call to {:X}", a_name, callee));
        }

        logger::info("resolved {} semantically at {:X} within [{:X}, {:X})",
            a_name, match, functionBegin, functionEnd);
        return match;
    }

    /**
     * @brief Resolves a render call while preserving an existing hook installed by another plugin.
     */
    std::pair<std::uintptr_t, std::uintptr_t> CellTransitioner::FindChainableRelativeCall(
        REL::RelocationID a_callerID,
        REL::RelocationID a_calleeID,
        std::ptrdiff_t    a_verifiedOffset,
        std::string_view  a_name)
    {
        try {
            const auto callSite = FindUniqueRelativeCall(a_callerID, a_calleeID, a_name);
            return { callSite, REL::Relocation<std::uintptr_t>(a_calleeID).address() };
        } catch (const std::exception& semanticError) {
            const auto caller = REL::Relocation<std::uintptr_t>(a_callerID).address();
            if (!IsExecutableAddress(caller) || a_verifiedOffset <= 0) {
                throw;
            }

            const auto callSite = caller + a_verifiedOffset;
            const auto text = REL::Module::get().segment(REL::Segment::textx);
            const auto textEnd = text.address() + text.size();
            if (callSite < text.address() || callSite + 5 > textEnd) {
                throw std::runtime_error(fmt::format(
                    "{} fallback call site was outside Skyrim's .text section", a_name));
            }

            hde64s     decoded{};
            const auto length = hde64_disasm(reinterpret_cast<const void*>(callSite), &decoded);
            if (length != 5 || (decoded.flags & F_ERROR) != 0 || decoded.opcode != 0xE8) {
                throw std::runtime_error(fmt::format(
                    "{} fallback site was not a rel32 call", a_name));
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement,
                reinterpret_cast<const void*>(callSite + 1), sizeof(displacement));
            const auto currentTarget = callSite + 5 + displacement;
            if (!IsExecutableAddress(currentTarget)) {
                throw std::runtime_error(fmt::format(
                    "{} fallback call had a non-executable target at {:X}", a_name, currentTarget));
            }

            logger::warn(
                "{} no longer targeted Skyrim's original helper ({}); chaining existing target {:X} at verified site {:X}",
                a_name, semanticError.what(), currentTarget, callSite);
            return { callSite, currentTarget };
        }
    }

    /**
     * @brief Locks the last world frame and configures the selected loading presentation.
     */
    CellTransitioner::Presentation CellTransitioner::PrepareForLoad(RE::IMenu* a_menu)
    {
        const auto selected = ChoosePresentation();
        Atomic::set(presentation, selected);
        Atomic::set(postLoadReleasePending, false);

        if (selected == Presentation::vanilla) {

            Atomic::set(frozenFrameLocked, false);
            Atomic::set(preLoadDoorCaptureLocked, false);
            Atomic::set(preLoadDoorTransitionActive, false);
            Atomic::set(postLoadFadeStart, 0);
            Atomic::set(postLoadFadePending, false);
            Atomic::set(postLoadRecoveryDeadline, 0);
            Atomic::set(loadingTransitionStart, 0);
            Atomic::set(loadingMenuFadeElapsedMs, 0);
            Atomic::set(dominantColorPending, false);
            RestoreHUDVisibility();
            return selected;
        }

        // This is the capture gate. Once closed, the rolling texture remains the last pre-load world frame.
        Atomic::set(frozenFrameLocked, true);
        Atomic::set(preLoadDoorCaptureLocked, false);
        Atomic::set(postLoadFadeStart, 0);
        Atomic::set(postLoadFadePending, false);
        Atomic::set(postLoadRecoveryDeadline, 0);
        Atomic::set(postProcessingPassesSincePresent, 0);
        Atomic::set(worldRenderedSincePresent, false);
        // Menu construction and renderer suspension can consume the configured fade before the first
        // loading frame is presented. Start the visible color fade from Present instead.
        Atomic::set(loadingTransitionStart, 0);
        // Scaleform fade elapsed is advanced only from LoadingMenu::AdvanceMovie.
        Atomic::set(loadingMenuFadeElapsedMs, 0);

        const bool selectCapturedColor =
            Atomic::get(transitionType) == Settings::TransitionType::color &&
            Atomic::get(colorSource) == Settings::ColorSource::dominant;

        Atomic::set(dominantColorPending, selectCapturedColor);

        HideHUDForLoad();

        if (!a_menu) {

            logger::warn("could not update LoadingMenu flags because the menu pointer was null");
            return selected;
        }

        if (selected == Presentation::loadingMenu) {
            a_menu->menuFlags.set(
                RE::UI_MENU_FLAGS::kFreezeFrameBackground, RE::UI_MENU_FLAGS::kUsesBlurredBackground);
        } else {
            a_menu->menuFlags.reset(
                RE::UI_MENU_FLAGS::kFreezeFrameBackground, RE::UI_MENU_FLAGS::kUsesBlurredBackground);
        }

        return selected;
    }

    /**
     * @brief Marks the transition as actively loading.
     */
    void CellTransitioner::BeginLoad()
    {
        if (Atomic::get(presentation) == Presentation::vanilla) {

            Atomic::set(preLoadOwnedFader, false);
            Atomic::set(fastTravelBlackPending, false);
            Atomic::set(fastTravelBlackActive, false);
            Atomic::set(loadOwnedFader, false);
            Atomic::set(loadFaderCloseQueued, false);
            Atomic::set(epochActive, false);
            Atomic::set(preLoadDoorTransitionActive, false);
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                Atomic::set(renderObservationState, 0);
            }

            return;
        }

        auto* ui = RE::UI::GetSingleton();
        Atomic::set(faderPresentAtLoadStart, ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME));
        // The native fader that initiates a door, fast-travel, or save load is submitted before
        // LoadingMenu opens. Preserve the callback-derived ownership across that boundary.
        Atomic::set(loadOwnedFader, Atomic::get_and_clear(preLoadOwnedFader));
        Atomic::set(loadFaderCloseQueued, false);

        // Present uses this gate to choose an active loading presentation instead of the post-load fade.
        Atomic::set(epochActive, true);
        Atomic::set(preLoadDoorTransitionActive, false);
        Atomic::set(fastTravelBlackPending, false);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            Atomic::set(renderObservationState, 1);
        }
    }

    /**
     * @brief Starts the retained-frame fade and post-load control diagnostics.
     */
    void CellTransitioner::EndLoad()
    {
        // MQ101 owns its first-gameplay fade. Its native FaderMenu remains below TitleSequenceMenu, so release
        // our loading cover without adding the ordinary post-load compositor above those title cards.
        const bool newGame = Atomic::get(newGameTransitionActive);
        const bool vanilla = Atomic::get(presentation) == Presentation::vanilla;
        const bool nativeFastTravelFade = Atomic::get(fastTravelBlackActive);

        // Opening the post-load gate lets Present composite over the destination cell as soon as it returns.
        Atomic::set(epochActive, false);
        Atomic::set(preLoadDoorTransitionActive, false);
        Atomic::set(postLoadReleasePending, false);
        const auto now = CurrentTimeMilliseconds();
        const bool deferToPostProcessing =
            !newGame && !vanilla && !nativeFastTravelFade && compositeAfterPostProcessing;
        Atomic::set(postLoadFadeStart, newGame || vanilla || nativeFastTravelFade || deferToPostProcessing ? 0 : now);
        Atomic::set(postLoadFadePending, deferToPostProcessing);
        // A visual transition must never retain ownership indefinitely if its source/target is absent.
        // Expiry releases the overlay; it does not start blending against an unready destination.
        Atomic::set(postLoadRecoveryDeadline,
            newGame || vanilla || nativeFastTravelFade ? 0 :
                now + Atomic::get(holdAfterLoad) +
                    Atomic::get(fadeOutDuration) + 5000);
        Atomic::set(postProcessingPassesSincePresent, 0);
        Atomic::set(worldRenderedSincePresent, false);
        // 1.0.7 keeps the retained frame up after LoadingMenu closes (especially the CS post-process
        // fade). Restoring HUD here paints health/stamina over that cover even when
        // show_hud_during_loading is false. Hold the hide until the presentation finishes.
        const bool holdHUDForTransition =
            !newGame && !vanilla && !nativeFastTravelFade &&
            (Atomic::get(mainMenuLoadActive) ||
                !Settings::GetSingleton().ShowHUDDuringLoading());
        if (holdHUDForTransition) {
            HideHUDForLoad();
        } else {
            RestoreHUDVisibility();
        }

        if (newGame || vanilla || nativeFastTravelFade) {
            Atomic::set(frozenFrameLocked, false);
        }

        if (newGame && Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info("handed the new-game transition from the loading compositor to Skyrim's FaderMenu");
        }

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            const auto renderState = Atomic::get(renderObservationState);
            if (renderState == 1 || renderState == 2) {
                Atomic::set(renderObservationState, 3);
            }

            {
                std::scoped_lock lock(controlStateLock);
                lastControlState.reset();
            }
            Atomic::set(awaitingControlRestore, true);
            ObserveControlRestore();
        }

        if (!newGame && !vanilla) {
            CloseResidualLoadingMenus(nativeFastTravelFade);
        }
    }

    /**
     * @brief Arms the one-time black loading presentation before SKSE lets Skyrim create the new game.
     */
    void CellTransitioner::BeginNewGameTransition()
    {
        Atomic::set(newGameFadeRequestSeen, false);
        Atomic::set(newGameTransitionActive, true);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info("armed the new-game black/title-sequence transition");
        }
    }

    /**
     * @brief Clears a pending intro when a save load supersedes it or transition hooks fail.
     */
    void CellTransitioner::CancelNewGameTransition()
    {
        Atomic::set(newGameTransitionActive, false);
        Atomic::set(newGameFadeRequestSeen, false);
    }

    /**
     * @brief Remembers the Main Menu as the origin after its movie closes and before LoadingMenu opens.
     */
    void CellTransitioner::ObserveMainMenuOpening()
    {
        Atomic::set(mainMenuLoadPending, true);
    }

    /**
     * @brief Reapplies the load-scoped HUD policy when Skyrim creates HUDMenu during a Main Menu save
     * load.
     */
    void CellTransitioner::ObserveHUDMenuOpening()
    {
        if (Atomic::get(frozenFrameLocked) &&
            Atomic::get(postLoadFadeStart) <= 0) {
            HideHUDForLoad();
        }
    }

    /**
     * @brief Hides only the top-level HUD movie, preserving all child alpha and animation state. Main Menu
     * save loads never expose gameplay HUD; the setting applies only to in-game transitions.
     */
    void CellTransitioner::HideHUDForLoad()
    {
        if (IsVanilla()) {
            return;
        }

        const bool showHUD = Settings::GetSingleton().ShowHUDDuringLoading();
        const bool forceHidden = Atomic::get(mainMenuLoadActive);
        if (showHUD && !forceHidden) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        auto  movie = ui ? ui->GetMovieView(RE::HUDMenu::MENU_NAME) : nullptr;
        if (!movie) {
            return;
        }

        if (!Atomic::get(hudVisibilityOwned)) {

            // Dialogue and other modal menus can temporarily hide HUDMenu before initiating a load.
            // Do not claim an already-hidden movie: Skyrim may restore it while LoadingMenu is open,
            // and writing the stale hidden state back at EndLoad would leave the gameplay HUD disabled.
            if (!movie->GetVisible()) {
                return;
            }

            Atomic::set(hudVisibilityOwned, true);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("claimed visible HUDMenu for loading");
            }
        }

        movie->SetVisible(false);
    }

    /**
     * @brief Reapplies or releases the HUD policy. Safe to schedule from Present; the movie is touched
     * only inside the UI task.
     */
    void CellTransitioner::QueueHUDVisibilitySync() noexcept
    {
        if (Atomic::get_and_set(hudSyncQueued, true)) {
            return;
        }

        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {

            Atomic::set(hudSyncQueued, false);
            return;
        }

        tasks->AddUITask([]() {
            Atomic::set(hudSyncQueued, false);
            if (!Atomic::get(hooksEnabled)) {

                RestoreHUDVisibility();
                return;
            }

            const bool transitionVisible =
                Atomic::get(epochActive) ||
                Atomic::get(preLoadDoorTransitionActive) ||
                Atomic::get(postLoadFadePending) ||
                Atomic::get(postLoadFadeStart) > 0;
            const bool hide =
                transitionVisible && !IsVanilla() &&
                (Atomic::get(mainMenuLoadActive) ||
                    !Settings::GetSingleton().ShowHUDDuringLoading());
            if (hide) {
                HideHUDForLoad();
            } else {
                RestoreHUDVisibility();
            }
        });
    }

    /**
     * @brief Restores exactly the movie visibility observed before this transition claimed it.
     */
    void CellTransitioner::RestoreHUDVisibility() noexcept
    {
        if (!Atomic::get_and_clear(hudVisibilityOwned)) {
            return;
        }

        try {
            auto* ui = RE::UI::GetSingleton();
            auto  movie = ui ? ui->GetMovieView(RE::HUDMenu::MENU_NAME) : nullptr;
            if (movie) {

                // Ownership is acquired only when this class changes the movie from visible to hidden.
                movie->SetVisible(true);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::info("restored HUDMenu visibility=true");
                }
            }
        } catch (...) {
            REX::W32::OutputDebugStringA(
                "Skyrim Load Progress: could not restore HUDMenu visibility\n");
        }
    }

    /**
     * @brief Returns whether the current transition suppresses LoadingMenu Scaleform.
     */
    bool CellTransitioner::IsSeamless()
    {
        return Atomic::get(presentation) == Presentation::seamless;
    }

    /**
     * @brief Returns whether Skyrim owns the current load presentation without compositor or menu
     * suppression.
     */
    bool CellTransitioner::IsVanilla()
    {
        return Atomic::get(presentation) == Presentation::vanilla;
    }

    /**
     * @brief Advances the custom LoadingMenu fade using movie intervals and returns its alpha in the range
     * [0, 1].
     *
     * @details Computes LoadingMenu Scaleform opacity for custom cold presentations.
     * Queued exterior destinations are intentionally treated as cold; fading the menu in gives
     * the retained frame time to cover that conservative classification (see
     * GetQueuedDestinationCell). Elapsed time advances only from LoadingMenu::AdvanceMovie
     * intervals so Scaleform writes stay on the UI/movie path rather than ProcessMessage,
     * Present, or an external timer.
     */
    float CellTransitioner::LoadingMenuFadeAlpha(float a_interval) noexcept
    {
        if (!Atomic::get(hooksEnabled) ||
            Atomic::get(presentation) != Presentation::loadingMenu) {
            return 1.0F;
        }

        const auto duration = Settings::GetSingleton().GetLoadingMenuFadeIn().count();
        if (duration <= 0) {
            return 1.0F;
        }

        const auto step = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::llround(std::max(0.0F, a_interval) * 1000.0F)));
        auto elapsed = Atomic::get(loadingMenuFadeElapsedMs, std::memory_order_relaxed);
        while (true) {
            const auto next = std::min<std::int64_t>(duration, elapsed + step);
            if (Atomic::compare_and_set_weak(loadingMenuFadeElapsedMs,
                    elapsed, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {

                elapsed = next;
                break;
            }
        }

        return std::clamp(static_cast<float>(elapsed) / static_cast<float>(duration), 0.0F, 1.0F);
    }

    /**
     * @brief Writes Menu_mc alpha from LoadingMenu::AdvanceMovie so tips and the progress meter fade
     * together.
     */
    void CellTransitioner::ApplyLoadingMenuFade(RE::IMenu* a_menu, float a_interval) noexcept
    {
        if (!a_menu || !a_menu->uiMovie) {
            return;
        }

        try {
            const auto alphaPercent = static_cast<double>(LoadingMenuFadeAlpha(a_interval)) * 100.0;
            RE::GFxValue menu;
            if (a_menu->uiMovie->GetVariable(&menu, "_root.Menu_mc") && menu.IsObject()) {

                RE::GFxValue alpha;
                alpha.SetNumber(alphaPercent);
                menu.SetMember("_alpha", alpha);
                return;
            }

            // Replacement LoadingMenu movies may omit Menu_mc; fade the root clip instead.
            RE::GFxValue root;
            if (a_menu->uiMovie->GetVariable(&root, "_root") && root.IsObject()) {

                RE::GFxValue alpha;
                alpha.SetNumber(alphaPercent);
                root.SetMember("_alpha", alpha);
            }
        } catch (...) {
            REX::W32::OutputDebugStringA(
                "Skyrim Load Progress: could not apply LoadingMenu fade alpha\n");
        }
    }

    /**
     * @brief Compiles one of the small pixel shaders used by the loading compositor.
     */
    bool CellTransitioner::CreatePixelShader(
        ::ID3D11Device*       a_device,
        std::string_view      a_source,
        std::string_view      a_name,
        ::ID3D11PixelShader** a_shader)
    {
        if (!a_device || !a_shader || a_source.empty() || a_name.empty()) {
            return false;
        }

        *a_shader = nullptr;
        REX::W32::ID3DBlob* bytecode = nullptr;
        REX::W32::ID3DBlob* errors = nullptr;

        // The shader is HLSL source until compilation produces bytecode in an ID3DBlob.
        // main is the entry point; ps_5_0 selects a Shader Model 5 pixel shader. Compilation
        // can return a diagnostic blob even on success, so release both blobs on every path.
        const auto result = REX::W32::D3DCompile(a_source.data(), a_source.size(), a_name.data(), nullptr, nullptr,
            "main", "ps_5_0", 0, 0, &bytecode, &errors);
        if (FAILED(result) || !bytecode) {

            const auto* errorText = errors && errors->GetBufferPointer() ?
                                        static_cast<const char*>(errors->GetBufferPointer()) :
                                        "unknown error";
            logger::error("could not compile the {} shader: {}", a_name,
                errorText);

            if (bytecode) {
                bytecode->Release();
            }

            if (errors) {
                errors->Release();
            }

            return false;
        }

        if (errors) {
            errors->Release();
        }

        const auto* buffer = bytecode->GetBufferPointer();
        const auto  bufferSize = bytecode->GetBufferSize();
        if (!buffer || bufferSize == 0) {

            bytecode->Release();
            return false;
        }

        // Shader objects belong to the D3D device and retain their compiled program. The
        // temporary bytecode blob can be released after CreatePixelShader returns.
        const auto createResult =
            a_device->CreatePixelShader(buffer, bufferSize, nullptr, a_shader);
        bytecode->Release();

        return SUCCEEDED(createResult);
    }

    /**
     * @brief Creates the shader that recovers alpha from Skyrim's opaque UI target.
     */
    bool CellTransitioner::CreateLoadingOverlayShader(::ID3D11Device* a_device)
    {
        // Vanilla UI is copied from an opaque target, so its alpha cannot isolate the widgets.
        // This shader estimates coverage from RGB intensity: smoothstep fades nearly black
        // background pixels to transparent. It is a heuristic, so dark widget details can also
        // lose coverage. The separate native HDR UI path does not need this reconstruction.
        constexpr std::string_view source = R"(
Texture2D overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);

float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    float4 pixel = overlayTexture.Sample(overlaySampler, textureCoordinate) * color;
    float intensity = max(pixel.r, max(pixel.g, pixel.b));
    pixel.a = smoothstep(0.005, 0.04, intensity);
    return pixel;
}
)";

        return CreatePixelShader(a_device, source, "loading overlay", &loadingOverlayShader);
    }

    /**
     * @brief Creates the shader used to blend to the captured frame's dominant color.
     */
    bool CellTransitioner::CreateSolidColorShader(::ID3D11Device* a_device)
    {
        // SpriteBatch supplies a quad and COLOR0 even when no texture sampling is needed.
        // Returning that color fills the screen; the blend state decides whether its alpha
        // replaces the destination or crossfades over the previously drawn image.
        constexpr std::string_view source = R"(
float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    return color;
}
)";

        return CreatePixelShader(a_device, source, "solid color", &solidColorShader);
    }

    /**
     * @brief Returns a monotonic timestamp for transition timing.
     */
    std::int64_t CellTransitioner::CurrentTimeMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /**
     * @brief Preserves the native sleep/wait fader when SleepWaitMenu opens.
     *
     * @details SleepWaitMenu opens before Skyrim queues the fade that blacks out the time-advance sequence.
     * Keep its persistent FaderMenu movie visible even if an earlier load left that movie
     * hidden.
     */
    void CellTransitioner::ObserveSleepWaitMenuOpening()
    {
        Atomic::set(sleepFaderActive, true);
    }

    /**
     * @brief Marks the callback-free black recovery fade queued while SleepWaitMenu handles its close
     * message. The deadline is a fallback if the menu-open notification was not observed.
     */
    void CellTransitioner::ObserveSleepWaitMenuClosing()
    {
        constexpr std::int64_t sleepFadeRequestWindow = 5000;
        const auto             deadline = CurrentTimeMilliseconds() + sleepFadeRequestWindow;
        Atomic::set(sleepFadeRequestDeadline, deadline);
    }

    /**
     * @brief Creates the inexpensive single-pass blur used on the frozen world frame.
     */
    bool CellTransitioner::CreateFrozenFrameBlurShader(::ID3D11Device* a_device)
    {
        std::string source = FrozenFramePixelShader;

        // Substitute the configured blur radius into HLSL before compiling. A zero radius
        // keeps the same shader interface and opacity handling while disabling spatial blur.
        constexpr std::string_view token = "$BLUR_AMOUNT$";
        const auto& settings = Settings::GetSingleton();
        const auto amount = fmt::format("{:.3f}", settings.IsBlurEnabled() ? settings.GetBlurAmount() : 0.0F);
        for (auto position = source.find(token); position != std::string::npos; position = source.find(token)) {
            source.replace(position, token.size(), amount);
        }

        return CreatePixelShader(a_device, source, "frozen frame blur", &frozenFrameBlurShader);
    }

    /**
     * @brief Resolves only queued destinations that the active TES grid can safely expose.
     */
    RE::TESObjectCELL* CellTransitioner::GetQueuedDestinationCell()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("queued destination resolver: player is unavailable; using a cold presentation");
            }

            return nullptr;
        }

        // Snapshot the record so every decision in this pass uses the same destination fields.
        const auto target = player->GetPlayerRuntimeData().queuedTargetLoc;
        if (!target.isValid) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("queued destination resolver: no valid queued target; using a cold presentation");
            }

            return nullptr;
        }

        if (target.interior) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("queued destination resolver: using the published interior cell");
            }

            return target.interior;
        }

        if (!target.world) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("queued destination resolver: exterior target has no world; using a cold presentation");
            }

            return nullptr;
        }

        auto* tes = RE::TES::GetSingleton();
        if (!tes || tes->GetRuntimeData2().worldSpace != target.world) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info(
                    "queued destination resolver: target world is not active; using a cold presentation");
            }

            return nullptr;
        }

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info(
                "queued destination resolver: exterior target in active world; using a cold presentation");
        }

        return nullptr;
    }

    /**
     * @brief Chooses the warm or cold presentation before the Loading Menu opens.
     */
    CellTransitioner::Presentation CellTransitioner::ChoosePresentation()
    {
        const bool continuingMainMenuLoad = Atomic::get(mainMenuLoadActive) &&
                                            Atomic::get(preLoadDoorTransitionActive);
        Atomic::set(mainMenuLoadActive, false);
        Atomic::set(fastTravelBlackActive, false);

        if (Atomic::get(newGameTransitionActive)) {

            Atomic::set(mainMenuLoadPending, false);
            Atomic::set(vanillaLoadPending, false);
            // The loading compositor supplies opaque black beneath LoadingMenu. Once loading ends, Skyrim's
            // native FaderMenu takes over so TitleSequenceMenu retains its higher UI depth.
            Atomic::set(transitionType, Settings::TransitionType::color);
            Atomic::set(colorSource, Settings::ColorSource::fixed);
            Atomic::set(transitionColor, 0x000000);
            Atomic::set(fadeInDuration, 0);
            Atomic::set(holdAfterLoad, 0);
            Atomic::set(fadeOutDuration, 0);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("selected the opaque new-game loading presentation");
            }

            return Presentation::loadingMenu;
        }

        auto*      ui = RE::UI::GetSingleton();
        const bool fromMainMenu = Atomic::get_and_clear(mainMenuLoadPending) || continuingMainMenuLoad ||
                                  (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
        const bool pendingVanilla =
            Atomic::get_and_clear(vanillaLoadPending);
        const bool useVanilla = pendingVanilla ||
                                (fromMainMenu &&
                                    !Settings::GetSingleton().UseTransitionsForSaveLoads());
        if (useVanilla) {

            Atomic::set(fastTravelBlackPending, false);
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("selected Skyrim's vanilla loading presentation");
            }

            return Presentation::vanilla;
        }

        if (Atomic::get(fastTravelBlackPending)) {

            // MapMenu's 3D scene can be only partially rendered when its fast-travel fade closes it.
            // Continue the native black fade with an opaque compositor cover instead of exposing or
            // blurring that last captured map frame. Do not inspect the queued destination's worldspace
            // cell map here: the engine may still be materializing that map for scripted fast travel.
            const auto& cold = Settings::GetSingleton().GetColdTransition({});
            Atomic::set(transitionType, Settings::TransitionType::color);
            Atomic::set(colorSource, Settings::ColorSource::fixed);
            Atomic::set(transitionColor, 0x000000);
            Atomic::set(fadeInDuration, 0);
            Atomic::set(holdAfterLoad, cold.holdAfterLoad.count());
            Atomic::set(fadeOutDuration, cold.fadeOut.count());
            Atomic::set(fastTravelBlackActive, true);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info(
                    "selected fixed black fast-travel presentation: hold={}ms fadeOut={}ms",
                    Atomic::get(holdAfterLoad, std::memory_order_seq_cst), Atomic::get(fadeOutDuration, std::memory_order_seq_cst));
            }

            // FaderMenu is the only black layer guaranteed to survive the upscaler's final
            // presentation path. Keep LoadingMenu active for its UI, suppress MistMenu separately,
            // and let Skyrim own the native black hold and destination fade-in.
            return Presentation::loadingMenu;
        }

        auto* cell = GetQueuedDestinationCell();
        const bool             resident = cell && cell->GetRuntimeData().loadedData;
        const auto*            editorIDText = cell ? cell->GetFormEditorID() : nullptr;
        const std::string_view editorID = editorIDText ? editorIDText : "";

        if (fromMainMenu) {

            // A menu movie is not a useful retained gameplay frame. Keep Skyrim's native fade to black,
            // then hold that same fixed black beneath LoadingMenu and fade it into the loaded save.
            const auto& cold = Settings::GetSingleton().GetColdTransition(editorID);
            Atomic::set(mainMenuLoadActive, true);
            Atomic::set(transitionType, Settings::TransitionType::color);
            Atomic::set(colorSource, Settings::ColorSource::fixed);
            Atomic::set(transitionColor, 0x000000);
            Atomic::set(fadeInDuration, 0);
            Atomic::set(holdAfterLoad, cold.holdAfterLoad.count());
            Atomic::set(fadeOutDuration, cold.fadeOut.count());

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info(
                    "selected fixed black main-menu loading presentation: cell={:08X} editorID='{}' hold={}ms fadeOut={}ms",
                    cell ? cell->GetFormID() : 0, editorID, Atomic::get(holdAfterLoad, std::memory_order_seq_cst), Atomic::get(fadeOutDuration, std::memory_order_seq_cst));
            }

            return Presentation::loadingMenu;
        }

        const auto selected = resident ? Presentation::seamless : Presentation::loadingMenu;
        if (selected == Presentation::seamless) {

            const auto& warm = Settings::GetSingleton().GetWarmTransition();
            Atomic::set(transitionType, Settings::TransitionType::blur);
            Atomic::set(colorSource, Settings::ColorSource::fixed);
            Atomic::set(transitionColor, 0xFFFFFF);
            Atomic::set(fadeInDuration, 0);
            Atomic::set(holdAfterLoad, warm.holdAfterLoad.count());
            Atomic::set(fadeOutDuration, warm.fadeOut.count());
        } else {

            const auto& cold = Settings::GetSingleton().GetColdTransition(editorID);
            Atomic::set(transitionType, cold.type);
            Atomic::set(colorSource, cold.colorSource);
            Atomic::set(transitionColor, cold.color);
            Atomic::set(fadeInDuration, cold.fadeIn.count());
            Atomic::set(holdAfterLoad, cold.holdAfterLoad.count());
            Atomic::set(fadeOutDuration, cold.fadeOut.count());
        }

        const auto type = Atomic::get(transitionType);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            logger::info(
                "loading destination: cell={:08X} editorID='{}' loadedData={} attached={} presentation={} transition={} "
                "fadeIn={}ms hold={}ms fadeOut={}ms",
                cell ? cell->GetFormID() : 0, editorID, resident,
                cell && cell->IsAttached(), selected == Presentation::seamless ? "seamless" : "loading-menu",
                type == Settings::TransitionType::color ? "color" : "blur", Atomic::get(fadeInDuration, std::memory_order_seq_cst),
                Atomic::get(holdAfterLoad, std::memory_order_seq_cst), Atomic::get(fadeOutDuration, std::memory_order_seq_cst));

            if (type == Settings::TransitionType::color) {
                logger::info("color transition: source={} fallback=#{:06X}",
                    Atomic::get(colorSource) == Settings::ColorSource::dominant ?
                        "dominant" :
                        "fixed",
                    Atomic::get(transitionColor));
            }
        }

        return selected;
    }
    /**
     * @brief Collects the input and menu state used by post-load diagnostics.
     */
    std::optional<CellTransitioner::ControlState> CellTransitioner::GetControlState()
    {
        auto* controls = RE::ControlMap::GetSingleton();
        auto* playerControls = RE::PlayerControls::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!controls || !playerControls || !ui) {
            return std::nullopt;
        }

        std::uint32_t enabled = 0;
        std::uint32_t stored = 0;
        controls->GetControlsState(enabled, stored);
        return ControlState{ enabled, stored, playerControls->blockPlayerInput, ui->GameIsPaused(),
            ui->IsMenuOpen(RE::FaderMenu::MENU_NAME), ui->IsMenuOpen(RE::MistMenu::MENU_NAME),
            playerControls->movementHandler && playerControls->movementHandler->IsInputEventHandlingEnabled(),
            playerControls->lookHandler && playerControls->lookHandler->IsInputEventHandlingEnabled(),
            controls->GetRuntimeData().ignoreKeyboardMouse, ui->modal,
            playerControls->data.remapMode, ui->numPausesGame };
    }

    /**
     * @brief Logs each post-load input-state change until gameplay controls return.
     */
    void CellTransitioner::ObserveControlRestore()
    {
        if (!Settings::GetSingleton().IsLoadingLoggingEnabled() ||
            !Atomic::get(awaitingControlRestore)) {
            return;
        }

        const auto state = GetControlState();
        if (!state) {
            return;
        }

        {
            std::scoped_lock lock(controlStateLock);
            if (Settings::GetSingleton().IsLoadingLoggingEnabled() &&
                (!lastControlState || *state != *lastControlState)) {

                logger::info(
                    "post-load controls: enabled={:08X} stored={:08X} blockInput={} paused={} fader={} mist={} movementHandler={} lookHandler={} ignoreKeyboardMouse={} modal={} remapMode={} pauseCount={}",
                    state->enabled, state->stored, state->blockInput, state->paused, state->faderOpen,
                    state->mistOpen, state->movementHandlerEnabled, state->lookHandlerEnabled,
                    state->ignoreKeyboardMouse, state->modal, state->remapMode, state->pauseCount);
                lastControlState = *state;
            }
        }

        auto* controls = RE::ControlMap::GetSingleton();
        if (!controls) {
            return;
        }

        const bool gameplayReady = controls->IsMovementControlsEnabled() && controls->IsLookingControlsEnabled() &&
                                   controls->IsActivateControlsEnabled() && !state->blockInput && !state->paused &&
                                   state->movementHandlerEnabled && state->lookHandlerEnabled &&
                                   !state->ignoreKeyboardMouse && !state->modal && !state->remapMode;
        if (gameplayReady) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("post-load gameplay controls are ready");
            }

            Atomic::set(awaitingControlRestore, false);
        }
    }

    /**
     * @brief Checks whether the cached textures still match the active render target.
     */
    bool CellTransitioner::MatchesFrozenFrame(const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        return frozenFrame && frozenFrameDesc.width == a_desc.width && frozenFrameDesc.height == a_desc.height &&
               frozenFrameDesc.format == a_desc.format && frozenFrameDesc.sampleDesc.count == a_desc.sampleDesc.count &&
               frozenFrameDesc.sampleDesc.quality == a_desc.sampleDesc.quality;
    }

    /**
     * @brief Checks whether the pre-upscale scene capture matches Community Shaders' active scene target.
     */
    bool CellTransitioner::MatchesSceneFrame(const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        return sceneFrame && sceneFrameDesc.width == a_desc.width && sceneFrameDesc.height == a_desc.height &&
               sceneFrameDesc.format == a_desc.format && sceneFrameDesc.sampleDesc.count == a_desc.sampleDesc.count &&
               sceneFrameDesc.sampleDesc.quality == a_desc.sampleDesc.quality;
    }

    /**
     * @brief Releases textures that must be recreated when the render target changes.
     */
    void CellTransitioner::ReleaseFrameResources()
    {
        transitionUiOverlay.ReleaseTextures();
        retainedHdrConversion.Invalidate();
        if (communityShadersUiTargetView) {

            communityShadersUiTargetView->Release();
            communityShadersUiTargetView = nullptr;
        }

        if (communityShadersUiTarget) {

            communityShadersUiTarget->Release();
            communityShadersUiTarget = nullptr;
        }

        if (communityShadersHdrTargetView) {

            communityShadersHdrTargetView->Release();
            communityShadersHdrTargetView = nullptr;
        }

        if (communityShadersHdrTarget) {

            communityShadersHdrTarget->Release();
            communityShadersHdrTarget = nullptr;
        }

        communityShadersHdrTargetDesc = {};

        if (frozenFrame) {

            frozenFrame->Release();
            frozenFrame = nullptr;
        }

        if (frozenFrameView) {

            frozenFrameView->Release();
            frozenFrameView = nullptr;
        }

        if (dominantColorReadback) {

            dominantColorReadback->Release();
            dominantColorReadback = nullptr;
        }

        if (loadingOverlayView) {

            loadingOverlayView->Release();
            loadingOverlayView = nullptr;
        }

        if (loadingOverlay) {

            loadingOverlay->Release();
            loadingOverlay = nullptr;
        }
    }

    /**
     * @brief Releases the separate rolling scene capture used while the CS upscaler owns presentation.
     */
    void CellTransitioner::ReleaseSceneFrameResources()
    {
        if (sceneFrameView) {

            sceneFrameView->Release();
            sceneFrameView = nullptr;
        }

        if (sceneFrame) {

            sceneFrame->Release();
            sceneFrame = nullptr;
        }

        sceneFrameDesc = {};
    }

    /**
     * @brief Allocates the frozen frame, CPU readback, and Scaleform overlay textures.
     */
    bool CellTransitioner::PrepareFrozenFrame(
        REX::W32::ID3D11Device* a_device, const REX::W32::D3D11_TEXTURE2D_DESC& a_backBufferDesc)
    {
        if (!a_device || a_backBufferDesc.width == 0 || a_backBufferDesc.height == 0) {
            return false;
        }

        if (MatchesFrozenFrame(a_backBufferDesc)) {
            return true;
        }

        // Texture storage cannot be resized in place. A changed descriptor invalidates the
        // retained texture and all views/copies associated with that capture.
        ReleaseFrameResources();

        // A texture is image storage; a shader-resource view describes how a shader reads it.
        // Keep size/format/sample layout compatible with the source for GPU copies, then
        // replace swap-chain-specific usage flags with those required by our private resources.
        auto desc = a_backBufferDesc;

        // The GPU copy is sampled while Skyrim's normal world rendering is paused.
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = 0;
        desc.miscFlags = 0;
        if (a_device->CreateTexture2D(&desc, nullptr, &frozenFrame) < 0 ||
            a_device->CreateShaderResourceView(frozenFrame, nullptr, &frozenFrameView) < 0) {

            logger::error("could not allocate the frozen loading frame texture");
            ReleaseFrameResources();
            return false;
        }

        // A staging copy lets us choose a transition color without mapping a GPU texture.
        // STAGING resources support CPU access but cannot be bound as shader inputs/outputs.
        // This separate allocation is used only for color selection; the displayed photograph
        // remains in the GPU-only texture and is never sampled from staging storage.
        desc.usage = REX::W32::D3D11_USAGE_STAGING;
        desc.bindFlags = 0;
        desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;
        if (a_device->CreateTexture2D(&desc, nullptr, &dominantColorReadback) < 0) {
            logger::warn("could not allocate the dominant-color readback texture");
        }

        // Scaleform is copied separately so it can be composited over our replacement background.
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = 0;
        if (a_device->CreateTexture2D(&desc, nullptr, &loadingOverlay) < 0 ||
            a_device->CreateShaderResourceView(loadingOverlay, nullptr, &loadingOverlayView) < 0) {

            logger::error("could not allocate the loading-menu overlay texture");
            ReleaseFrameResources();
            return false;
        }

        frozenFrameDesc = a_backBufferDesc;
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            loggedFrozenFrame = false;
        }

        return true;
    }

    /**
     * @brief Allocates a shader-readable copy in the native scene format consumed by the CS Present proxy.
     */
    bool CellTransitioner::PrepareSceneFrame(
        REX::W32::ID3D11Device* a_device, const REX::W32::D3D11_TEXTURE2D_DESC& a_sceneDesc)
    {
        if (!a_device || a_sceneDesc.width == 0 || a_sceneDesc.height == 0) {
            return false;
        }

        if (MatchesSceneFrame(a_sceneDesc)) {
            return true;
        }

        ReleaseSceneFrameResources();
        auto desc = a_sceneDesc;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = 0;
        desc.miscFlags = 0;
        if (a_device->CreateTexture2D(&desc, nullptr, &sceneFrame) < 0 ||
            a_device->CreateShaderResourceView(sceneFrame, nullptr, &sceneFrameView) < 0) {

            logger::error("could not allocate the pre-upscale scene capture texture");
            ReleaseSceneFrameResources();
            return false;
        }

        sceneFrameDesc = a_sceneDesc;
        return true;
    }

    /**
     * @brief Returns true when the captured texture uses a supported BGRA byte layout.
     */
    bool CellTransitioner::IsBgraFormat(REX::W32::DXGI_FORMAT a_format)
    {
        return a_format == REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM ||
               a_format == REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    /**
     * @brief Returns true when the captured texture uses a supported RGBA byte layout.
     */
    bool CellTransitioner::IsRgbaFormat(REX::W32::DXGI_FORMAT a_format)
    {
        return a_format == REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM ||
               a_format == REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

    /**
     * @brief Builds a reduced RGB histogram from every fourth captured pixel.
     */
    std::array<std::uint32_t, 4096> CellTransitioner::BuildColorHistogram(
        const REX::W32::D3D11_MAPPED_SUBRESOURCE& a_mapped,
        REX::W32::DXGI_FORMAT                     a_format)
    {
        std::array<std::uint32_t, 4096> histogram{};
        const bool hdr = a_format == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT;
        const std::uint32_t pixelSize = hdr ? 8 : 4;
        if (!a_mapped.data || a_mapped.rowPitch / pixelSize < frozenFrameDesc.width) {
            return histogram;
        }

        const bool bgra = IsBgraFormat(a_format);
        const bool rgba = IsRgbaFormat(a_format);
        const bool rgb10 = a_format == REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM;
        if (!bgra && !rgba && !rgb10 && !hdr) {
            return histogram;
        }

        // RowPitch is the actual byte stride between rows and may include driver padding.
        // Never assume width * bytesPerPixel is the stride. Sampling every fourth row and
        // column reduces this one-time palette calculation to roughly 1/16 of the pixels.
        constexpr std::uint32_t sampleStep = 4;

        for (std::uint32_t y = 0; y < frozenFrameDesc.height; y += sampleStep) {
            const auto* row = static_cast<const std::uint8_t*>(a_mapped.data) + y * a_mapped.rowPitch;

            for (std::uint32_t x = 0; x < frozenFrameDesc.width; x += sampleStep) {
                const auto*   pixel = row + x * pixelSize;
                std::uint32_t red = 0;
                std::uint32_t green = 0;
                std::uint32_t blue = 0;
                std::uint32_t alpha = 0;

                if (hdr) {

                    std::array<DirectX::PackedVector::HALF, 4> channels{};
                    std::memcpy(channels.data(), pixel, sizeof(channels));
                    const auto quantize = [](DirectX::PackedVector::HALF a_channel) {
                        const auto value = DirectX::PackedVector::XMConvertHalfToFloat(a_channel);
                        return std::isfinite(value) ?
                                   static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F) : 0U;
                    };
                    red = quantize(channels[0]);
                    green = quantize(channels[1]);
                    blue = quantize(channels[2]);
                    // HDR scene alpha is not transparency; select the RGB photograph's palette.
                    alpha = 255;
                } else if (rgb10) {

                    std::uint32_t packed = 0;
                    std::memcpy(&packed, pixel, sizeof(packed));

                    // DXGI_FORMAT_R10G10B10A2_UNORM stores R in the least-significant ten bits.
                    // Round each normalized ten-bit channel into the byte range used by the existing
                    // histogram; expand two-bit alpha to the same range for the transparency filter.
                    red = ((packed & 0x3FFU) * 255U + 511U) / 1023U;
                    green = (((packed >> 10U) & 0x3FFU) * 255U + 511U) / 1023U;
                    blue = (((packed >> 20U) & 0x3FFU) * 255U + 511U) / 1023U;
                    alpha = ((packed >> 30U) & 0x03U) * 85U;
                } else {

                    red = bgra ? pixel[2] : pixel[0];
                    green = pixel[1];
                    blue = bgra ? pixel[0] : pixel[2];
                    alpha = pixel[3];
                }

                // Transparent and nearly black pixels do not represent the scene's useful color.
                if (alpha < 128 || std::max({ red, green, blue }) < 16) {
                    continue;
                }

                // Keep the upper four bits of each RGB channel: 16 * 16 * 16 = 4096 palette bins.
                // Grouping nearby colors avoids treating small lighting/noise differences as distinct colors.
                const auto bin = static_cast<std::size_t>((red >> 4) << 8 | (green >> 4) << 4 | (blue >> 4));
                ++histogram[bin];
            }
        }

        return histogram;
    }

    /**
     * @brief Converts the most populated histogram bin back into a packed RGB color.
     */
    std::optional<std::uint32_t> CellTransitioner::SelectDominantColor(
        const std::array<std::uint32_t, 4096>& a_histogram)
    {
        const auto dominantBin = std::max_element(a_histogram.begin(), a_histogram.end());
        if (*dominantBin == 0) {
            return std::nullopt;
        }

        const auto dominant = static_cast<std::uint32_t>(std::distance(a_histogram.begin(), dominantBin));
        const auto red = ((dominant >> 8) & 0x0F) * 17;
        const auto green = ((dominant >> 4) & 0x0F) * 17;
        const auto blue = (dominant & 0x0F) * 17;

        return (red << 16) | (green << 8) | blue;
    }

    /**
     * @brief Reads the locked source frame and selects the color used by dominant-color transitions.
     */
    void CellTransitioner::UpdateTransitionColor(REX::W32::ID3D11DeviceContext* a_context)
    {
        if (!a_context || !frozenFrame || !dominantColorReadback) {

            logger::warn("using the default transition color because no captured frame is readable");
            return;
        }

        const bool supported = IsBgraFormat(frozenFrameDesc.format) ||
                               IsRgbaFormat(frozenFrameDesc.format) ||
                               frozenFrameDesc.format == REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM ||
                               frozenFrameDesc.format == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!supported) {

            logger::warn("using the default transition color for unsupported texture format {}",
                std::to_underlying(frozenFrameDesc.format));
            return;
        }

        // The copy stays on the GPU, but Map(READ) must make its result available to the CPU
        // and can wait for pending GPU work. Do this for the locked transition capture, not
        // on every Present. The mapped pointer is valid only until Unmap.
        a_context->CopyResource(dominantColorReadback, frozenFrame);

        REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
        if (a_context->Map(dominantColorReadback, 0, REX::W32::D3D11_MAP_READ, 0, &mapped) < 0) {

            logger::warn("could not read the captured frame for transition color selection");
            return;
        }

        const auto histogram = BuildColorHistogram(mapped, frozenFrameDesc.format);
        a_context->Unmap(dominantColorReadback, 0);

        const auto color = SelectDominantColor(histogram);
        if (!color) {

            logger::warn("using the default transition color because the captured frame had no usable pixels");
            return;
        }

        Atomic::set(transitionColor, *color);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info("selected captured-frame transition color #{:06X}", Atomic::get(transitionColor, std::memory_order_seq_cst));
        }
    }

    /**
     * @brief Converts the packed transition color and caller-supplied alpha for SpriteBatch.
     */
    DirectX::XMVECTOR CellTransitioner::TransitionColor(float a_alpha)
    {
        const auto color = Atomic::get(transitionColor);
        return DirectX::XMVectorSet(static_cast<float>((color >> 16) & 0xFF) / 255.0F,
            static_cast<float>((color >> 8) & 0xFF) / 255.0F,
            static_cast<float>(color & 0xFF) / 255.0F, a_alpha);
    }

    /**
     * @brief Returns the photograph shader, including explicit opacity when the blur radius is zero.
     */
    ::ID3D11PixelShader* CellTransitioner::GetFrozenFrameShader()
    {
        return frozenFrameBlurShader;
    }

    /**
     * @brief Returns a full-screen destination rectangle for the current back buffer.
     */
    RECT CellTransitioner::GetDestinationRect(const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        return { 0, 0, static_cast<LONG>(a_desc.width), static_cast<LONG>(a_desc.height) };
    }

    /**
     * @brief Draws one full-screen compositor layer with an explicit pixel shader.
     */
    void CellTransitioner::DrawFullscreenLayer(
        REX::W32::ID3D11DeviceContext*      a_context,
        REX::W32::ID3D11ShaderResourceView* a_texture,
        const RECT&                         a_destination,
        ::ID3D11BlendState*                 a_blendState,
        ::ID3D11SamplerState*               a_samplerState,
        ::ID3D11PixelShader*                a_shader,
        DirectX::XMVECTOR                   a_color)
    {
        auto* context = reinterpret_cast<::ID3D11DeviceContext*>(a_context);
        auto* texture = reinterpret_cast<::ID3D11ShaderResourceView*>(a_texture);
        if (!context || !texture || !spriteBatch) {
            return;
        }

        // SpriteBatch renders a screen-space quad with a vertex shader and pixel shader.
        // This guard saves its affected bindings; the caller separately owns render-target
        // and viewport restoration because this helper draws into whichever target is bound.
        const ScopedSpritePipelineState savedPipeline(context);
        if (a_shader) {
            // Deferred mode queues Draw until End. The callback installs our pixel shader when
            // the batch actually prepares its pipeline, after SpriteBatch installs its defaults.
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, a_blendState, a_samplerState, nullptr, nullptr,
                [context, a_shader] { context->PSSetShader(a_shader, nullptr, 0); });
        } else {
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, a_blendState, a_samplerState);
        }

        // The blend state controls how shader output combines with the current target. Opaque
        // replaces it; NonPremultiplied uses source alpha for RGB blending. LinearClamp supplies
        // filtered texture reads without wrapping fullscreen edge samples.
        spriteBatch->Draw(texture, a_destination, a_color);
        spriteBatch->End();
    }

    /**
     * @brief Replaces the loading frame with the captured pre-load world image and configured blur.
     */
    void CellTransitioner::PresentSeamlessFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        if (!a_context || !a_backBuffer || !commonStates) {
            return;
        }

        // Keep capture and composite in the same image space. A proxy GetBuffer before CS Present
        // is not final output: CS 1.8.4 clears it after the preceding Present and fills it in ApplyHDR.
        auto* sourceView = MatchesFrozenFrame(a_desc) ? frozenFrameView :
                           (MatchesSceneFrame(a_desc) ? sceneFrameView : nullptr);
        if (!sourceView) {
            return;
        }

        DrawFullscreenLayer(a_context, sourceView, GetDestinationRect(a_desc),
            commonStates->Opaque(), commonStates->LinearClamp(), GetFrozenFrameShader());

        if (Settings::GetSingleton().IsLoadingLoggingEnabled() && !loggedFrozenPresentation) {

            logger::info("presenting the blurred frozen pre-load frame");
            loggedFrozenPresentation = true;
        }
    }

    /**
     * @brief Draws the blurred source frame and then restores Scaleform above it.
     */
    void CellTransitioner::PresentLoadingMenuFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc,
        bool                                  a_separateUI)
    {
        auto* sourceView = frozenFrameView;
        const bool opaqueFixedColor =
            Atomic::get(transitionType) == Settings::TransitionType::color &&
            Atomic::get(colorSource) == Settings::ColorSource::fixed &&
            Atomic::get(fadeInDuration) <= 0;
        if (!a_context || !a_backBuffer || !commonStates || !sourceView ||
            (!opaqueFixedColor && !MatchesFrozenFrame(a_desc)) ||
            (!a_separateUI && (!loadingOverlay || !loadingOverlayView))) {
            return;
        }

        if (!a_separateUI) {
            // Vanilla renders Scaleform into the scene target. Save it so its widgets can be restored.
            a_context->CopyResource(loadingOverlay, a_backBuffer);
        }

        const auto destination = GetDestinationRect(a_desc);
        if (Atomic::get(mainMenuLoadActive) || opaqueFixedColor) {
            // Immediate fixed-color transitions do not sample the retained texture. This also lets
            // them cover an upscaler's internal loading target when its format differs from the
            // post-processed frame captured before the load.
            DrawFullscreenLayer(a_context, sourceView, destination, commonStates->Opaque(), nullptr,
                solidColorShader, TransitionColor(1.0F));
        } else {
            DrawFullscreenLayer(a_context, sourceView, destination, commonStates->Opaque(),
                commonStates->LinearClamp(), GetFrozenFrameShader());
        }

        if (!Atomic::get(mainMenuLoadActive) && !opaqueFixedColor &&
            Atomic::get(transitionType) == Settings::TransitionType::color) {

            const auto now = CurrentTimeMilliseconds();
            auto       start = Atomic::get(loadingTransitionStart);
            if (start == 0) {
                if (Atomic::compare_and_set(loadingTransitionStart,
                        start, now, std::memory_order_acq_rel, std::memory_order_acquire)) {

                    start = now;

                    if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                        logger::info("started visible color fade-in");
                    }
                }
            }

            const auto elapsed = std::max<std::int64_t>(now - start, 0);
            const auto duration = Atomic::get(fadeInDuration);
            const auto colorAlpha = duration > 0 ?
                                        std::clamp(static_cast<float>(elapsed) / static_cast<float>(duration), 0.0F, 1.0F) :
                                        1.0F;

            // Color transitions move from the captured world toward their fixed or sampled color.
            DrawFullscreenLayer(a_context, sourceView, destination, commonStates->NonPremultiplied(), nullptr,
                solidColorShader, TransitionColor(colorAlpha));
        }

        if (!a_separateUI) {
            // Recover the menu's effective alpha and composite its widgets above our replacement background.
            DrawFullscreenLayer(a_context, loadingOverlayView, destination, commonStates->NonPremultiplied(), nullptr,
                loadingOverlayShader);
        }
    }

    /**
     * @brief Release presentation ownership without modifying Skyrim's pause counter or player controls.
     */
    void CellTransitioner::FinishPostLoadPresentation()
    {
        Atomic::set(postLoadReleasePending, false);
        Atomic::set(postLoadFadeStart, 0);
        Atomic::set(postLoadFadePending, false);
        Atomic::set(postLoadRecoveryDeadline, 0);
        Atomic::set(frozenFrameLocked, false);
        Atomic::set(mainMenuLoadActive, false);
        QueueHUDVisibilitySync();
    }

    /**
     * @brief Draws the retained frame over the new cell until the post-load crossfade ends.
     */
    void CellTransitioner::PresentPostLoadFrame(
        REX::W32::ID3D11DeviceContext* a_context, const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        if (!a_context || !commonStates) {
            return;
        }

        const auto fadeStart = Atomic::get(postLoadFadeStart);
        const bool fadePending = Atomic::get(postLoadFadePending);
        if (fadeStart <= 0 && !fadePending) {
            return;
        }

        // While the preferred CS compositor has not resumed yet, keep the retained frame fully opaque.
        // This closes the handoff between LoadingMenu's last Present and the first post-load fade pass.
        const auto delay = Atomic::get(holdAfterLoad);
        const auto fadeElapsed = fadePending ? 0 : CurrentTimeMilliseconds() - fadeStart - delay;
        const auto duration = Atomic::get(fadeOutDuration);

        if (!fadePending && fadeElapsed >= duration) {

            // Alpha has reached zero, but CS still consumes the scene and UI later
            // inside Present. Do not change UI suppression or restore the HUD midway
            // through this frame. Release only after the chained Present returns.
            if (!Atomic::get_and_set(postLoadReleasePending, true) &&
                Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("post-load crossfade reached zero alpha; retaining UI policy through final Present");
            }

            return;
        }

        if (!frozenFrameView) {
            return;
        }

        // The destination world is already underneath this draw. Start at photograph alpha 1
        // and reduce it toward 0 after the configured hold; blending then reveals the new cell.
        const auto fadeProgress = duration > 0 ?
                                      std::clamp(static_cast<float>(fadeElapsed) / static_cast<float>(duration), 0.0F, 1.0F) :
                                      1.0F;
        const auto alpha = 1.0F - fadeProgress;
        const auto usesColor =
            Atomic::get(transitionType) == Settings::TransitionType::color;
        const auto color = usesColor ?
                               TransitionColor(alpha) :
                               DirectX::XMVectorSet(1.0F, 1.0F, 1.0F, alpha);

        // Blur transitions fade the retained frame; color transitions fade their solid color.
        DrawFullscreenLayer(a_context, frozenFrameView, GetDestinationRect(a_desc),
            commonStates->NonPremultiplied(), commonStates->LinearClamp(),
            usesColor ? solidColorShader : GetFrozenFrameShader(), color);
    }

    /**
     * @brief Selects the compositor path for the current loading state.
     */
    void CellTransitioner::CompositeLoadingFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc,
        bool                                  a_separateUI)
    {
        if (!a_context || !a_backBuffer || !spriteBatch || !commonStates) {
            return;
        }

        // Startup has no gameplay photograph. The solid-color shader still needs
        // a SpriteBatch SRV, including on loading frames before world rendering
        // resumes. Initialize it here rather than waiting for the destination.
        if (Atomic::get(mainMenuLoadActive) && !frozenFrameView &&
            !PrepareFrozenFrame(RE::BSGraphics::Renderer::GetDevice(), a_desc)) {
            return;
        }

        // epochActive is the main loading gate. postLoadFadeStart handles the short tail after it closes.
        const bool loading = Atomic::get(preLoadDoorTransitionActive) ||
                             Atomic::get(epochActive);

        if (loading && Atomic::get(transitionType) == Settings::TransitionType::color &&
            Atomic::get_and_clear(dominantColorPending)) {
            UpdateTransitionColor(a_context);
        }

        if (!loading) {

            PresentPostLoadFrame(a_context, a_desc);
            return;
        }

        if (Atomic::get(presentation) == Presentation::seamless) {

            PresentSeamlessFrame(a_context, a_backBuffer, a_desc);
            return;
        }

        PresentLoadingMenuFrame(a_context, a_backBuffer, a_desc, a_separateUI);
    }

    /**
     * @brief Saves a GPU texture as a DDS file when transition texture diagnostics are enabled.
     */
    void CellTransitioner::SaveDiagnosticTexture(
        REX::W32::ID3D11DeviceContext* a_context, REX::W32::ID3D11Resource* a_source, std::string_view a_stage)
    {
        if (!Settings::GetSingleton().IsTransitionTextureCaptureEnabled()) {
            return;
        }

        auto path = logger::log_directory();
        if (!path || !a_source) {
            return;
        }

        *path /= std::string("SkyrimLoadProgress-") + std::string(a_stage) + ".dds";
        const auto result = DirectX::SaveDDSTextureToFile(
            reinterpret_cast<::ID3D11DeviceContext*>(a_context), reinterpret_cast<::ID3D11Resource*>(a_source), path->c_str());
        logger::info("GPU transition capture: stage={} result={:08X} path={}", a_stage,
            static_cast<std::uint32_t>(result), path->string());
    }

    /**
     * @brief Installs or refreshes chained GPU copy and dispatch observers for the context vtable.
     */
    bool CellTransitioner::EnsureCopyObserver(REX::W32::ID3D11DeviceContext* a_context)
    {
        if (!a_context || !compositeAfterPostProcessing) {
            return false;
        }

        const bool traceCopies = Settings::GetSingleton().IsLoadingLoggingEnabled() ||
            (Settings::GetSingleton().IsTransitionTextureCaptureEnabled() &&
                Atomic::get(diagnosticCaptureStage) < 4);
        // D3D11 can expose a different context vtable after initialization. Refresh
        // coverage on the rendering thread and preserve each table's original method.
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_context);
        const auto slot = vtable + 47 * sizeof(std::uintptr_t);
        const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
        const auto observer = reinterpret_cast<std::uintptr_t>(TraceOutputCopy);
        const auto dispatchSlot = vtable + 41 * sizeof(std::uintptr_t);
        const auto currentDispatch = *reinterpret_cast<const std::uintptr_t*>(dispatchSlot);
        const auto dispatchObserver = reinterpret_cast<std::uintptr_t>(TraceHdrDispatch);
        if ((!traceCopies || current == observer) && currentDispatch == dispatchObserver) {
            return true;
        }

        if (!IsExecutableAddress(current) || !IsExecutableAddress(currentDispatch)) {
            return false;
        }

        const auto count = Atomic::get(copyObserverRecordCount);
        std::uint32_t record = 0;
        while (record < count && copyObserverRecords[record].vtable != vtable) {
            ++record;
        }
        if (record < count) {
            // Another hook changed this same table. Do not build an unknown recursive chain.
            if ((current != observer && reinterpret_cast<std::uintptr_t>(copyObserverRecords[record].original) != current) ||
                (currentDispatch != dispatchObserver && reinterpret_cast<std::uintptr_t>(copyObserverRecords[record].originalDispatch) != currentDispatch)) {
                return false;
            }
        } else {

            if (count == copyObserverRecords.size()) {
                return false;
            }

            copyObserverRecords[count] = { vtable, reinterpret_cast<CopyResource_t>(current), reinterpret_cast<Dispatch_t>(currentDispatch) };
            Atomic::set(copyObserverRecordCount, count + 1);
        }

        if (!originalCopyResource) {
            originalCopyResource = reinterpret_cast<CopyResource_t>(current);
        }

        if (traceCopies) {
            REL::safe_write(slot, &observer, sizeof(observer), &current, sizeof(current));
        }

        REL::safe_write(dispatchSlot, &dispatchObserver, sizeof(dispatchObserver), &currentDispatch, sizeof(currentDispatch));
        const bool installed = (!traceCopies || *reinterpret_cast<const std::uintptr_t*>(slot) == observer) &&
                               *reinterpret_cast<const std::uintptr_t*>(dispatchSlot) == dispatchObserver;
        static std::uint32_t refreshReports{};
        if (Settings::GetSingleton().IsLoadingLoggingEnabled() && (refreshReports++ < 8 || !installed)) {
            logger::info("GPU copy/dispatch observers refreshed: context={:X} vtable={:X} original={:X} installed={}",
                reinterpret_cast<std::uintptr_t>(a_context), vtable, current, installed);
        }

        return installed;
    }

    /**
     * @brief Clears the native HDR UI target when an owned door transition requires the HUD to be hidden.
     */
    void CellTransitioner::PrepareHdrUiForTransition(REX::W32::ID3D11DeviceContext* a_context, const void* a_caller)
    {
        if (!Atomic::get(hooksEnabled) || Settings::GetSingleton().ShowHUDDuringLoading() ||
            Atomic::get(epochActive) || Atomic::get(mainMenuLoadActive)) {
            return;
        }

        const bool active = Atomic::get(preLoadDoorTransitionActive) ||
                            Atomic::get(postLoadFadePending) ||
                            Atomic::get(postLoadFadeStart) > 0;
        if (!active || !frozenFrameView) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                      ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME))) {
            return;
        }

        REX::W32::MEMORY_BASIC_INFORMATION callerMemory{};
        const auto csModule = GetModuleHandleW(L"CommunityShaders.dll");
        if (!csModule || REX::W32::VirtualQuery(a_caller, &callerMemory, sizeof(callerMemory)) == 0 ||
            callerMemory.allocationBase != csModule) {
            return;
        }

        REX::W32::ComPtr<REX::W32::ID3D11UnorderedAccessView> outputView;
        REX::W32::ComPtr<REX::W32::ID3D11Resource> outputResource;
        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> output;
        a_context->CSGetUnorderedAccessViews(0, 1, outputView.GetAddressOf());
        if (!outputView.Get()) {
            return;
        }

        outputView->GetResource(outputResource.GetAddressOf());
        if (!outputResource.Get() || outputResource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(output.GetAddressOf())) < 0) {
            return;
        }

        REX::W32::D3D11_TEXTURE2D_DESC outputDesc{};
        output->GetDesc(&outputDesc);
        const auto* graphics = RE::BSGraphics::State::GetSingleton();
        if (!graphics || outputDesc.width != graphics->screenWidth || outputDesc.height != graphics->screenHeight ||
            outputDesc.format != REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM) {
            return;
        }

        REX::W32::ComPtr<REX::W32::ID3D11ShaderResourceView> uiView;
        REX::W32::ComPtr<REX::W32::ID3D11Resource> uiResource;
        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> uiTexture;
        a_context->CSGetShaderResources(1, 1, uiView.GetAddressOf());
        if (!uiView.Get()) {
            return;
        }

        uiView->GetResource(uiResource.GetAddressOf());
        if (!uiResource.Get() || uiResource.Get() == communityShadersHdrTarget || uiResource.Get() == outputResource.Get() ||
            uiResource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(uiTexture.GetAddressOf())) < 0) {
            return;
        }

        REX::W32::D3D11_TEXTURE2D_DESC uiDesc{};
        uiTexture->GetDesc(&uiDesc);
        if (uiDesc.width != outputDesc.width || uiDesc.height != outputDesc.height || uiDesc.sampleDesc.count != 1 ||
            !(IsRgbaFormat(uiDesc.format) || IsBgraFormat(uiDesc.format))) {
            return;
        }

        if (communityShadersUiTarget != uiTexture.Get()) {

            REX::W32::ComPtr<REX::W32::ID3D11Device> device;
            REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> clearView;
            a_context->GetDevice(device.GetAddressOf());
            if (!device.Get() || device->CreateRenderTargetView(uiTexture.Get(), nullptr, clearView.GetAddressOf()) < 0) {
                return;
            }

            if (communityShadersUiTargetView) {
                communityShadersUiTargetView->Release();
            }

            if (communityShadersUiTarget) {
                communityShadersUiTarget->Release();
            }

            communityShadersUiTarget = uiTexture.Detach();
            communityShadersUiTargetView = clearView.Detach();
        }

        // The hidden-HUD pre-door/exit path needs only the retained HDR scene.
        // LoadingMenu keeps its UI layer for the meter. Operate on the actual
        // bound HDR UI input, after every native/menu writer has finished.
        REX::W32::ID3D11ShaderResourceView* nullView{};
        a_context->CSSetShaderResources(1, 1, &nullView);
        constexpr float transparent[4]{};
        a_context->ClearRenderTargetView(communityShadersUiTargetView, transparent);
        auto* restoreView = uiView.Get();
        a_context->CSSetShaderResources(1, 1, &restoreView);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            static unsigned lastReportedPhase{};
            const unsigned phase = Atomic::get(preLoadDoorTransitionActive) ? 1 : 2;
            if (phase != lastReportedPhase) {

                logger::info("suppressed non-loading CS UI at HDR dispatch: phase={} target={:X} callerRVA={:X}", phase,
                    reinterpret_cast<std::uintptr_t>(communityShadersUiTarget),
                    reinterpret_cast<std::uintptr_t>(a_caller) - reinterpret_cast<std::uintptr_t>(csModule));
                lastReportedPhase = phase;
            }
        }
    }

    /**
     * @brief Observes native HDR conversion dispatches and applies retained-scene conversion and UI
     * composition.
     */
    void CellTransitioner::TraceHdrDispatch(REX::W32::ID3D11DeviceContext* a_context,
        std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
    {
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_context);
        const auto records = Atomic::get(copyObserverRecordCount);
        Dispatch_t original{};
        for (std::uint32_t record = 0; record < records; ++record) {
            if (copyObserverRecords[record].vtable == vtable) {

                original = copyObserverRecords[record].originalDispatch;
                break;
            }
        }
        if (!original) {

            // A runtime-switched table can differ from the table that dispatched this
            // callback. Chain its current native entry rather than dropping GPU work.
            const auto current = *reinterpret_cast<const std::uintptr_t*>(vtable + 41 * sizeof(std::uintptr_t));
            if (current != reinterpret_cast<std::uintptr_t>(TraceHdrDispatch)) {
                original = reinterpret_cast<Dispatch_t>(current);
            }
        }

        TransitionUiOverlay::Binding overlayBinding;
        TransitionUiOverlay::Binding diagnosticBinding;
        const bool loadingDiagnostics = Settings::GetSingleton().IsLoadingLoggingEnabled();
        static thread_local unsigned rollingDiagnosticReports{};
        const bool overlayOwnsImage = Atomic::get(hooksEnabled) && !IsVanilla() &&
            frozenFrameView && (Atomic::get(epochActive) ||
                Atomic::get(preLoadDoorTransitionActive) ||
                Atomic::get(postLoadFadePending) ||
                Atomic::get(postLoadFadeStart) > 0 ||
                Atomic::get(postLoadReleasePending));
        const bool inspectRollingDiagnostics = loadingDiagnostics && rollingDiagnosticReports < 8 &&
            Atomic::get(completedHdrSceneSincePresent) &&
            !Atomic::get(frozenFrameLocked);
        if (Atomic::get(hooksEnabled) &&
            (overlayOwnsImage || captureHdrConversionThisPresent || inspectRollingDiagnostics) && original) {

            REX::W32::MEMORY_BASIC_INFORMATION callerMemory{};
            const auto csModule = GetModuleHandleW(L"CommunityShaders.dll");
            const auto* graphics = RE::BSGraphics::State::GetSingleton();
            if (csModule && graphics && REX::W32::VirtualQuery(_ReturnAddress(), &callerMemory, sizeof(callerMemory)) != 0 &&
                callerMemory.allocationBase == csModule) {

                auto binding = transitionUiOverlay.Inspect(reinterpret_cast<ID3D11DeviceContext*>(a_context),
                    graphics->screenWidth, graphics->screenHeight);
                if (overlayOwnsImage || captureHdrConversionThisPresent) {
                    overlayBinding = std::move(binding);
                } else {
                    // Observe early native conversion without changing its production eligibility.
                    diagnosticBinding = std::move(binding);
                }
            }
        }

        HRESULT startupCoverResult = S_FALSE;
        try {
            const bool startupPending = Atomic::get(mainMenuLoadActive) &&
                (Atomic::get(epochActive) || Atomic::get(preLoadDoorTransitionActive) ||
                    Atomic::get(postLoadFadePending));
            startupCoverResult = CoverPendingStartupScene(
                reinterpret_cast<ID3D11DeviceContext*>(a_context), overlayBinding, startupPending,
                reinterpret_cast<ID3D11Texture2D*>(communityShadersHdrTarget));
            PrepareHdrUiForTransition(a_context, _ReturnAddress());
        } catch (const std::exception& error) {
            logger::warn("CS UI preparation failed; continuing native HDR dispatch: {}", error.what());
        } catch (...) {
            logger::warn("CS UI preparation failed; continuing native HDR dispatch");
        }
        // There are two native conversions to observe: display conversion produces the HDR
        // photograph, then a paired UI conversion prepares the frame-generation UI layer.
        // While fully holding the photograph, keep its captured scene-encoding flags; once
        // the crossfade starts, native live-scene conversion takes over again.
        HRESULT conversionResult = S_FALSE;
        HRESULT conversionCaptureResult = S_FALSE;
        bool holdingImage{};
        if (overlayBinding.pass == TransitionUiOverlay::Pass::display) {

            if (captureHdrConversionThisPresent && overlayBinding.scene.Get() ==
                reinterpret_cast<ID3D11Texture2D*>(communityShadersHdrTarget)) {

                conversionResult = retainedHdrConversion.Capture(reinterpret_cast<ID3D11DeviceContext*>(a_context),
                    overlayBinding.constants.Get(), overlayBinding.scene.Get());
                if (loadingDiagnostics) conversionCaptureResult = conversionResult;
            }

            const auto fadeStart = Atomic::get(postLoadFadeStart);
            holdingImage = overlayOwnsImage && (Atomic::get(preLoadDoorTransitionActive) ||
                Atomic::get(epochActive) || Atomic::get(postLoadFadePending) ||
                (fadeStart > 0 && CurrentTimeMilliseconds() - fadeStart <= Atomic::get(holdAfterLoad)));
            if (holdingImage) {

                conversionResult = retainedHdrConversion.Apply(reinterpret_cast<ID3D11DeviceContext*>(a_context),
                    overlayBinding.constants.Get(), overlayBinding.scene.Get(),
                    reinterpret_cast<RetainedHdrConversion::Dispatch>(original));
                if (conversionResult == S_OK && Settings::GetSingleton().IsLoadingLoggingEnabled()) {

                    static unsigned lastReportedPhase{};
                    const unsigned phase = Atomic::get(epochActive) ? 2 :
                        Atomic::get(preLoadDoorTransitionActive) ? 1 : 3;
                    if (phase != lastReportedPhase) {

                        logger::info("retained HDR scene conversion during image hold: phase={}; native UI/FG policy preserved", phase);
                        lastReportedPhase = phase;
                    }
                }
            }
        }

        // Always run the original GPU dispatch exactly once. Capturing display output or
        // composing onto UI happens afterward, when native conversion has finished writing it.
        if (original) {
            original(a_context, a_x, a_y, a_z);
        }

        // Restore before the native UI pass observes the original constant-buffer identity.
        if (overlayBinding.pass == TransitionUiOverlay::Pass::display) {

            auto* nativeConstants = overlayBinding.constants.Get();
            reinterpret_cast<ID3D11DeviceContext*>(a_context)->CSSetConstantBuffers(0, 1, &nativeConstants);
        }

        if (FAILED(conversionResult)) {

            static bool failureReported{};
            if (!failureReported) {

                logger::warn("retained HDR conversion unavailable ({:08X}); using native scene conversion",
                    static_cast<std::uint32_t>(conversionResult));
                failureReported = true;
            }
        }

        // Capture the converted display in the first pass, then insert it beneath the native
        // UI in the matching second pass. An opaque UI cover also covers generated world frames;
        // drawing only into a late swap-chain buffer would not establish that coverage.
        HRESULT overlayResult = S_FALSE;
        if (overlayOwnsImage && overlayBinding.pass == TransitionUiOverlay::Pass::display) {
            overlayResult = transitionUiOverlay.Capture(reinterpret_cast<ID3D11DeviceContext*>(a_context), overlayBinding);
        } else if (overlayOwnsImage && overlayBinding.pass == TransitionUiOverlay::Pass::ui) {

            overlayResult = transitionUiOverlay.Compose(reinterpret_cast<ID3D11DeviceContext*>(a_context), overlayBinding,
                reinterpret_cast<TransitionUiOverlay::Dispatch>(original));
            if (overlayResult == S_OK && Settings::GetSingleton().IsLoadingLoggingEnabled()) {

                static unsigned lastReportedPhase{};
                const unsigned phase = Atomic::get(epochActive) ? 2 :
                    Atomic::get(preLoadDoorTransitionActive) ? 1 : 3;
                if (phase != lastReportedPhase) {

                    logger::info("transition image composed into native frame-generation UI: phase={} size={}x{}",
                        phase, overlayBinding.desc.Width, overlayBinding.desc.Height);
                    lastReportedPhase = phase;
                }
            }
        }

        if (loadingDiagnostics) {

            const auto& observed = overlayBinding.pass != TransitionUiOverlay::Pass::none || overlayBinding.texture ?
                overlayBinding : diagnosticBinding;
            const bool reportRolling = observed.pass == TransitionUiOverlay::Pass::display &&
                !overlayOwnsImage && rollingDiagnosticReports < 8;
            if (observed.pass == TransitionUiOverlay::Pass::display && (overlayOwnsImage || reportRolling)) {

                const auto state = retainedHdrConversion.InspectDiagnostics(observed.constants.Get());
                const auto phase = Atomic::get(epochActive) ? 2U :
                    Atomic::get(preLoadDoorTransitionActive) ? 1U :
                    overlayOwnsImage ? 3U : 0U;
                logger::info("HDR frame trace: frame={} phase={} insidePresent={} insideChain={} captureEligible={} "
                    "captureResult={:08X} hold={} applyResult={:08X} expectedScene={:X} nativeScene={:X} "
                    "capturedScene={:X} cb={:X} cbBytes={} compatible={} init={:08X} output={:X} ui={:X} snapshot={:08X} startupCover={:08X}",
                    transitionFrameDiagnostics.frame, phase, transitionFrameDiagnostics.insidePresent,
                    transitionFrameDiagnostics.insideChain, captureHdrConversionThisPresent,
                    static_cast<std::uint32_t>(conversionCaptureResult), holdingImage,
                    static_cast<std::uint32_t>(conversionResult),
                    reinterpret_cast<std::uintptr_t>(communityShadersHdrTarget),
                    reinterpret_cast<std::uintptr_t>(observed.scene.Get()),
                    reinterpret_cast<std::uintptr_t>(state.capturedSource),
                    reinterpret_cast<std::uintptr_t>(observed.constants.Get()), state.constantBytes, state.compatible,
                    static_cast<std::uint32_t>(state.initialization), reinterpret_cast<std::uintptr_t>(observed.texture.Get()),
                    reinterpret_cast<std::uintptr_t>(observed.ui.Get()), static_cast<std::uint32_t>(overlayResult),
                    static_cast<std::uint32_t>(startupCoverResult));
                if (reportRolling) ++rollingDiagnosticReports;
                if (overlayOwnsImage) {

                    ++transitionFrameDiagnostics.displays;
                    transitionFrameDiagnostics.snapshots += overlayResult == S_OK;
                    transitionFrameDiagnostics.displayTexture = reinterpret_cast<std::uintptr_t>(observed.texture.Get());
                }
            } else if (overlayOwnsImage && observed.pass == TransitionUiOverlay::Pass::ui) {

                ++transitionFrameDiagnostics.uiPasses;
                transitionFrameDiagnostics.compositions += overlayResult == S_OK;
                logger::info("UI frame trace: frame={} insidePresent={} insideChain={} target={:X} cb={:X} compose={:08X}",
                    transitionFrameDiagnostics.frame, transitionFrameDiagnostics.insidePresent,
                    transitionFrameDiagnostics.insideChain, reinterpret_cast<std::uintptr_t>(observed.texture.Get()),
                    reinterpret_cast<std::uintptr_t>(observed.constants.Get()), static_cast<std::uint32_t>(overlayResult));
            } else if (overlayOwnsImage && observed.texture.Get() ==
                reinterpret_cast<ID3D11Texture2D*>(communityShadersHdrTarget) && observed.texture) {

                ++transitionFrameDiagnostics.sceneDispatches;
                logger::info("scene compute write trace: frame={} insidePresent={} insideChain={} target={:X}",
                    transitionFrameDiagnostics.frame, transitionFrameDiagnostics.insidePresent,
                    transitionFrameDiagnostics.insideChain, reinterpret_cast<std::uintptr_t>(observed.texture.Get()));
            } else if (overlayOwnsImage && observed.texture && observed.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                observed.texture.Get() == reinterpret_cast<ID3D11Texture2D*>(communityShadersUiTarget)) {

                ++transitionFrameDiagnostics.unpairedUi;
                logger::info("UI frame trace: frame={} insidePresent={} insideChain={} target={:X} cb={:X} unpaired=true",
                    transitionFrameDiagnostics.frame, transitionFrameDiagnostics.insidePresent,
                    transitionFrameDiagnostics.insideChain, reinterpret_cast<std::uintptr_t>(observed.texture.Get()),
                    reinterpret_cast<std::uintptr_t>(observed.constants.Get()));
            }
        }

        if (FAILED(overlayResult)) {

            static bool failureReported{};
            if (!failureReported) {

                logger::warn("transition UI overlay unavailable ({:08X}); retaining scene composition",
                    static_cast<std::uint32_t>(overlayResult));
                failureReported = true;
            }
        }

        if (!Settings::GetSingleton().IsTransitionTextureCaptureEnabled()) {
            return;
        }

        const auto stage = Atomic::get(diagnosticCaptureStage);
        static thread_local bool readingBack = false;
        if (readingBack || stage == 0 || stage >= 4 || !Atomic::get(hooksEnabled)) {
            return;
        }

        const bool loading = Atomic::get(epochActive);
        const bool destination = Atomic::get(postLoadFadeStart) > 0;
        const std::uint8_t phase = destination ? 4 : loading ? 2 : 1;
        if (Atomic::get(diagnosticDispatchSamples) & phase) {
            return;
        }

        REX::W32::MEMORY_BASIC_INFORMATION callerMemory{};
        const auto caller = _ReturnAddress();
        const auto csModule = GetModuleHandleW(L"CommunityShaders.dll");
        if (!csModule || REX::W32::VirtualQuery(caller, &callerMemory, sizeof(callerMemory)) == 0 ||
            callerMemory.allocationBase != csModule) {
            return;
        }

        // Observe bound resources at the actual output dispatch; no CS-private offsets.
        // ApplyHDR leaves these bindings intact until this callback returns.
        REX::W32::ComPtr<REX::W32::ID3D11UnorderedAccessView> outputView;
        REX::W32::ComPtr<REX::W32::ID3D11Resource> outputResource;
        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> output;
        a_context->CSGetUnorderedAccessViews(0, 1, outputView.GetAddressOf());
        if (!outputView.Get()) {
            return;
        }

        outputView->GetResource(outputResource.GetAddressOf());
        if (!outputResource.Get() || outputResource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(output.GetAddressOf())) < 0) {
            return;
        }

        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        output->GetDesc(&desc);
        const auto* graphics = RE::BSGraphics::State::GetSingleton();
        if (!graphics || desc.width != graphics->screenWidth || desc.height != graphics->screenHeight ||
            desc.format != REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM) {
            return;
        }

        diagnosticDispatchSamples.fetch_or(phase, std::memory_order_acq_rel);
        readingBack = true;
        try {
            logger::info("CS HDR dispatch checkpoint: phase={} callerRVA={:X} output={}x{} format={}", phase,
                reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(csModule),
                desc.width, desc.height, std::to_underlying(desc.format));
            std::array<REX::W32::ID3D11ShaderResourceView*, 2> views{};
            a_context->CSGetShaderResources(0, 2, views.data());
            std::array<REX::W32::ComPtr<REX::W32::ID3D11ShaderResourceView>, 2> retainedViews;
            for (std::size_t index = 0; index < views.size(); ++index) {
                retainedViews[index].Attach(views[index]);
            }
            for (std::size_t index = 0; index < views.size(); ++index) {
                if (!retainedViews[index].Get()) {
                    continue;
                }

                REX::W32::ComPtr<REX::W32::ID3D11Resource> inputResource;
                REX::W32::ComPtr<REX::W32::ID3D11Texture2D> input;
                retainedViews[index]->GetResource(inputResource.GetAddressOf());
                if (inputResource.Get() && inputResource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                        reinterpret_cast<void**>(input.GetAddressOf())) >= 0) {

                    const auto name = std::string(index == 0 ? "hdr-scene-" : "hdr-ui-") +
                                      (destination ? "destination" : loading ? "loading" : "door");
                    SaveDiagnosticTexture(a_context, input.Get(), name);
                }
            }
            SaveDiagnosticTexture(a_context, output.Get(), destination ? "hdr-output-destination" :
                loading ? "hdr-output-loading" : "hdr-output-door");
        } catch (const std::exception& error) {
            logger::warn("HDR dispatch capture failed: {}", error.what());
        } catch (...) {
            logger::warn("HDR dispatch capture failed with an unknown exception");
        }
        readingBack = false;
    }

    /**
     * @brief Observe CS's final output copy before the proxy clears its buffer. Copies themselves remain
     * unchanged; only the first ordinary door in a diagnostic run is read back.
     */
    void CellTransitioner::TraceOutputCopy(
        REX::W32::ID3D11DeviceContext* a_context, REX::W32::ID3D11Resource* a_destination,
        REX::W32::ID3D11Resource* a_source)
    {
        auto original = originalCopyResource;
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_context);
        const auto records = Atomic::get(copyObserverRecordCount);
        for (std::uint32_t record = 0; record < records; ++record) {
            if (copyObserverRecords[record].vtable == vtable) {

                original = copyObserverRecords[record].original;
                break;
            }
        }
        if (original) {
            original(a_context, a_destination, a_source);
        }

        if (Settings::GetSingleton().IsLoadingLoggingEnabled() && Atomic::get(hooksEnabled) &&
            frozenFrameView && (Atomic::get(epochActive) ||
                Atomic::get(preLoadDoorTransitionActive) ||
                Atomic::get(postLoadFadePending) ||
                Atomic::get(postLoadFadeStart) > 0 ||
                Atomic::get(postLoadReleasePending))) {

            const auto source = reinterpret_cast<std::uintptr_t>(a_source);
            const auto destination = reinterpret_cast<std::uintptr_t>(a_destination);
            const bool sceneWrite = communityShadersHdrTarget && destination ==
                reinterpret_cast<std::uintptr_t>(communityShadersHdrTarget);
            const bool frozenWrite = frozenFrame && destination == reinterpret_cast<std::uintptr_t>(frozenFrame);
            const bool displayRead = transitionFrameDiagnostics.displayTexture &&
                source == transitionFrameDiagnostics.displayTexture;
            if (sceneWrite || frozenWrite || displayRead) {

                transitionFrameDiagnostics.sceneCopies += sceneWrite;
                transitionFrameDiagnostics.frozenCopies += frozenWrite;
                transitionFrameDiagnostics.displayCopies += displayRead;
                logger::info("scene copy trace: frame={} insidePresent={} insideChain={} source={:X} destination={:X} "
                    "sceneWrite={} frozenWrite={} displayRead={} caller={:X}",
                    transitionFrameDiagnostics.frame, transitionFrameDiagnostics.insidePresent,
                    transitionFrameDiagnostics.insideChain, source, destination, sceneWrite, frozenWrite, displayRead,
                    reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
            }
        }

        if (!Settings::GetSingleton().IsTransitionTextureCaptureEnabled()) {
            return;
        }

        static thread_local bool readingBack = false;
        if (readingBack || !Atomic::get(hooksEnabled)) {
            return;
        }

        const auto stage = Atomic::get(diagnosticCaptureStage);
        if (stage == 0 || stage >= 4) {
            return;
        }

        // CS can finish its output copy before our vtable Present hook is entered.
        // Identify the CS caller and encoded display-sized source, independently of
        // the Present window and COM interface pointer aliases.
        REX::W32::MEMORY_BASIC_INFORMATION callerMemory{};
        const auto caller = _ReturnAddress();
        const auto csModule = GetModuleHandleW(L"CommunityShaders.dll");
        const bool callerResolved = REX::W32::VirtualQuery(caller, &callerMemory, sizeof(callerMemory)) != 0;
        // Record coverage before applying the caller/texture filters. Absence of a saved
        // output must not be mistaken for absence of CS rendering.
        static std::atomic_uint32_t coverageSamples{};
        if (Atomic::get_and_add(coverageSamples, 1, std::memory_order_relaxed) < 8) {
            logger::info("GPU copy coverage: stage={} context={:X} caller={:X} module={:X} csModule={:X}",
                stage, reinterpret_cast<std::uintptr_t>(a_context), reinterpret_cast<std::uintptr_t>(caller),
                callerResolved ? reinterpret_cast<std::uintptr_t>(callerMemory.allocationBase) : 0,
                reinterpret_cast<std::uintptr_t>(csModule));
        }

        if (!csModule || !a_source || !callerResolved || callerMemory.allocationBase != csModule) {
            return;
        }

        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> outputSource;
        if (a_source->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                reinterpret_cast<void**>(outputSource.GetAddressOf())) < 0 || !outputSource.Get()) {
            return;
        }

        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        outputSource->GetDesc(&desc);
        static std::atomic_uint32_t csSamples{};
        if (Atomic::get_and_add(csSamples, 1, std::memory_order_relaxed) < 8) {
            logger::info("CS copy candidate: stage={} callerRVA={:X} source={}x{} format={}", stage,
                reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(csModule),
                desc.width, desc.height, std::to_underlying(desc.format));
        }

        const auto* graphics = RE::BSGraphics::State::GetSingleton();
        if (!graphics || desc.width != graphics->screenWidth || desc.height != graphics->screenHeight ||
            !(desc.format == REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM || IsRgbaFormat(desc.format) || IsBgraFormat(desc.format))) {
            return;
        }

        const bool loading = Atomic::get(epochActive);
        const bool destination = Atomic::get(postLoadFadeStart) > 0;
        if (stage == 2 && !loading && !destination) {
            return;
        }

        if (stage == 3 && !destination) {
            return;
        }

        readingBack = true;
        try {
            logger::info("CS display output copy: stage={} insidePresent={} callerRVA={:X} source={}x{} format={}",
                stage, Atomic::get(diagnosticOutputTarget) != 0,
                reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(csModule),
                desc.width, desc.height, std::to_underlying(desc.format));
            if (stage == 1) {

                if (frozenFrame && !Atomic::get_and_set(diagnosticFrozenSaved, true)) {
                    SaveDiagnosticTexture(a_context, frozenFrame, "frozen-source");
                }

                SaveDiagnosticTexture(a_context, a_source, "door-output");
                Atomic::set(diagnosticCaptureStage, 2);
            } else if (loading) {

                SaveDiagnosticTexture(a_context, a_source, "loading-output");
                Atomic::set(diagnosticCaptureStage, 3);
            } else {

                SaveDiagnosticTexture(a_context, a_source, "destination-output");
                Atomic::set(diagnosticCaptureStage, 4);
            }
        } catch (const std::exception& error) {
            logger::warn("GPU transition capture failed: {}", error.what());
            Atomic::set(diagnosticCaptureStage, 4);
        } catch (...) {
            logger::warn("GPU transition capture failed with an unknown exception");
            Atomic::set(diagnosticCaptureStage, 4);
        }
        readingBack = false;
    }

    /**
     * @brief Composites the owned transition, chains the original Present, and releases completed
     * presentation state.
     */
    REX::W32::HRESULT CellTransitioner::PresentFrozenFrame(
        REX::W32::IDXGISwapChain* a_swapChain, std::uint32_t a_syncInterval, std::uint32_t a_flags)
    {
        // Win32 E_POINTER is the only safe result when the hooked COM receiver is unavailable.
        constexpr auto nullPointerResult = static_cast<REX::W32::HRESULT>(0x80004003U);
        if (!a_swapChain || !originalPresent) {
            return nullPointerResult;
        }

        transitionUiOverlay.BeginPresent();
        const bool completedHdrScene = Atomic::get_and_clear(completedHdrSceneSincePresent);
        captureHdrConversionThisPresent = Atomic::get(hooksEnabled) && completedHdrScene &&
            Atomic::get(worldRenderedSincePresent) &&
            !Atomic::get(frozenFrameLocked);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            const auto nextFrame = transitionFrameDiagnostics.frame + 1;
            const auto displayTexture = transitionFrameDiagnostics.displayTexture;
            transitionFrameDiagnostics = {};
            transitionFrameDiagnostics.frame = nextFrame;
            transitionFrameDiagnostics.displayTexture = displayTexture;
            transitionFrameDiagnostics.insidePresent = true;
            transitionFrameDiagnostics.owned = Atomic::get(hooksEnabled) && !IsVanilla() &&
                frozenFrameView && (Atomic::get(epochActive) ||
                    Atomic::get(preLoadDoorTransitionActive) ||
                    Atomic::get(postLoadFadePending) ||
                    Atomic::get(postLoadFadeStart) > 0 ||
                    Atomic::get(postLoadReleasePending));
            if (transitionFrameDiagnostics.owned || nextFrame <= 8) {

                const auto state = retainedHdrConversion.InspectDiagnostics(nullptr);
                logger::info("transition Present begin: frame={} thread={} flags={:08X} sync={} owned={} epoch={} door={} pending={} fadeStart={} "
                    "completedScene={} freshWorld={} locked={} captureEligible={} expectedScene={:X} frozen={:X} capturedScene={:X}",
                    nextFrame, ::GetCurrentThreadId(), a_flags, a_syncInterval, transitionFrameDiagnostics.owned,
                    Atomic::get(epochActive),
                    Atomic::get(preLoadDoorTransitionActive),
                    Atomic::get(postLoadFadePending), Atomic::get(postLoadFadeStart),
                    completedHdrScene, Atomic::get(worldRenderedSincePresent),
                    Atomic::get(frozenFrameLocked), captureHdrConversionThisPresent,
                    reinterpret_cast<std::uintptr_t>(communityShadersHdrTarget), reinterpret_cast<std::uintptr_t>(frozenFrame),
                    reinterpret_cast<std::uintptr_t>(state.capturedSource));
            }
        }

        if (Atomic::get(hooksEnabled)) {

            try {
                if (compositeAfterPostProcessing) {

                    auto* observerRenderer = RE::BSGraphics::Renderer::GetSingleton();
                    EnsureCopyObserver(observerRenderer ? observerRenderer->GetRuntimeData().context : nullptr);
                }

                const auto captureStage = Settings::GetSingleton().IsTransitionTextureCaptureEnabled() ?
                    Atomic::get(diagnosticCaptureStage) : 0;
                if (captureStage > 0 && captureStage < 4 && originalCopyResource) {

                    static std::uintptr_t lastContext{};
                    static std::uintptr_t lastCopySlot{};
                    auto* traceRenderer = RE::BSGraphics::Renderer::GetSingleton();
                    auto* traceContext = traceRenderer ? traceRenderer->GetRuntimeData().context : nullptr;
                    if (traceContext) {

                        const auto contextIdentity = reinterpret_cast<std::uintptr_t>(traceContext);
                        const auto contextVtable = *reinterpret_cast<const std::uintptr_t*>(traceContext);
                        const auto copySlot = *reinterpret_cast<const std::uintptr_t*>(contextVtable + 47 * sizeof(std::uintptr_t));
                        if (contextIdentity != lastContext || copySlot != lastCopySlot) {

                            logger::info("GPU observer coverage at Present: context={:X} CopyResource={:X} observerInstalled={}",
                                contextIdentity, copySlot, copySlot == reinterpret_cast<std::uintptr_t>(TraceOutputCopy));
                            lastContext = contextIdentity;
                            lastCopySlot = copySlot;
                        }
                    }

                    Atomic::set(diagnosticOutputTarget, 0);
                    REX::W32::ComPtr<REX::W32::ID3D11Texture2D> output;
                    if (a_swapChain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D,
                            reinterpret_cast<void**>(output.GetAddressOf())) >= 0) {

                        Atomic::set(diagnosticOutputTarget, reinterpret_cast<std::uintptr_t>(output.Get()));
                        // Log this checkpoint, but do not read back the cleared proxy again:
                        // the prior run established that it is not final output at hook entry.
                        const bool nativeLoading = Atomic::get(epochActive);
                        const bool destinationFade = Atomic::get(postLoadFadeStart) > 0;
                        const std::uint8_t phase = destinationFade ? 4 : nativeLoading ? 2 : 1;
                        if (traceContext && !(diagnosticPresentSamples.fetch_or(phase, std::memory_order_acq_rel) & phase)) {

                            REX::W32::D3D11_TEXTURE2D_DESC outputDesc{};
                            output->GetDesc(&outputDesc);
                            logger::info("GPU Present-entry checkpoint: phase={} output={}x{} format={} retainedHdr={:X}",
                                phase, outputDesc.width, outputDesc.height, std::to_underlying(outputDesc.format),
                                reinterpret_cast<std::uintptr_t>(communityShadersHdrTarget));
                        }
                    }

                    if (frozenFrame && !Atomic::get_and_set(diagnosticFrozenSaved, true)) {

                        auto* traceRenderer = RE::BSGraphics::Renderer::GetSingleton();
                        if (auto* traceContext = traceRenderer ? traceRenderer->GetRuntimeData().context : nullptr) {
                            SaveDiagnosticTexture(traceContext, frozenFrame, "frozen-source");
                        }
                    }
                }

                ObserveControlRestore();
                const auto recoveryDeadline = Atomic::get(postLoadRecoveryDeadline);
                if (!Atomic::get(epochActive) &&
                    !Atomic::get(preLoadDoorTransitionActive) &&
                    recoveryDeadline > 0 && CurrentTimeMilliseconds() >= recoveryDeadline) {

                    logger::warn("post-load presentation exceeded its recovery deadline; releasing overlay and HUD ownership");
                    FinishPostLoadPresentation();
                }

                // The image-space call is not guaranteed to run for every frame Skyrim presents,
                // particularly while menu/loading render paths are changing. Treat it as the preferred
                // CS/Upscaler path, but retain this final compositor as a watchdog for any frame it misses.
                const bool loading = Atomic::get(preLoadDoorTransitionActive) ||
                                     Atomic::get(epochActive);
                bool       fadePending = Atomic::get(postLoadFadePending);
                const bool transitionActive = loading || fadePending ||
                                        Atomic::get(postLoadFadeStart) > 0;
                const auto postProcessingPasses = compositeAfterPostProcessing ?
                                                      Atomic::get_and_set(postProcessingPassesSincePresent, 0) :
                                                      0;
                const bool postProcessingComposited = postProcessingPasses > 0;
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

                    const unsigned gates = (loading ? 1U : 0U) | (fadePending ? 2U : 0U) |
                                           (transitionActive ? 4U : 0U) | (postProcessingComposited ? 8U : 0U) |
                                           (Atomic::get(worldRenderedSincePresent) ? 16U : 0U);
                    static unsigned lastLoggedGates = ~0U;
                    if (gates != lastLoggedGates) {

                        logger::info("Present gates: loading={} pending={} active={} postProcessPasses={} freshWorld={} hdrTarget={}",
                            loading, fadePending, transitionActive, postProcessingPasses,
                            (gates & 16U) != 0, communityShadersHdrTarget != nullptr);
                        lastLoggedGates = gates;
                    }
                }

                // A missing world/post-process frame supplies no new destination to blend with.
                // Re-arm the opaque hold instead of accumulating alpha over our own previous cover.
                if (compositeAfterPostProcessing && !loading && transitionActive &&
                    !postProcessingComposited && !fadePending) {

                    Atomic::set(postLoadReleasePending, false);
                    Atomic::set(postLoadFadeStart, 0);
                    Atomic::set(postLoadFadePending, true);
                    fadePending = true;
                }

                const bool waitingForPostProcessing =
                    compositeAfterPostProcessing && !loading && fadePending;
                const bool usePresentFallback = !compositeAfterPostProcessing || loading ||
                                                waitingForPostProcessing;
                const bool fallbackNeeded = transitionActive && usePresentFallback &&
                                            !postProcessingComposited;
                if (fallbackNeeded) {

                    REX::W32::ComPtr<REX::W32::ID3D11Texture2D> backBuffer;
                    const auto                                  result = a_swapChain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D,
                                                         reinterpret_cast<void**>(backBuffer.GetAddressOf()));
                    if (result >= 0 && backBuffer.Get()) {

                        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                        auto* device = RE::BSGraphics::Renderer::GetDevice();
                        auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;

                        if (device && context) {

                            auto& framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

                            REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> boundView;
                            REX::W32::ComPtr<REX::W32::ID3D11DepthStencilView> boundDepth;
                            REX::W32::ComPtr<REX::W32::ID3D11Resource>         boundResource;
                            REX::W32::ComPtr<REX::W32::ID3D11Texture2D>        boundTexture;
                            REX::W32::ComPtr<REX::W32::ID3D11Resource>         sceneResource;
                            REX::W32::ComPtr<REX::W32::ID3D11Texture2D>        sceneTexture;
                            context->OMGetRenderTargets(
                                1, boundView.GetAddressOf(), boundDepth.GetAddressOf());
                            if (boundView.Get()) {

                                boundView->GetResource(boundResource.GetAddressOf());
                                if (boundResource.Get()) {
                                    boundResource->QueryInterface(
                                        REX::W32::IID_ID3D11Texture2D,
                                        reinterpret_cast<void**>(boundTexture.GetAddressOf()));
                                }
                            }

                            if (framebuffer.SRV) {

                                reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
                                    framebuffer.SRV)
                                    ->GetResource(sceneResource.GetAddressOf());
                                if (sceneResource.Get()) {
                                    sceneResource->QueryInterface(
                                        REX::W32::IID_ID3D11Texture2D,
                                        reinterpret_cast<void**>(sceneTexture.GetAddressOf()));
                                }
                            }

                            // Community Shaders keeps its lit scene in a floating-point HDR target and
                            // converts that target into the proxy swap chain from its Present hook. During
                            // loading or render suspension, the image-space pass can be skipped but CS
                            // still performs this final conversion. Refill the retained CS target so the preserved
                            // HDR lighting goes through the same output transform as a normal frame.
                            bool compositedIntoCommunityShadersHdr = false;
                            if (communityShadersHdrTarget && communityShadersHdrTargetView &&
                                MatchesFrozenFrame(communityShadersHdrTargetDesc)) {

                                std::array<REX::W32::D3D11_VIEWPORT,
                                    D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                                              previousViewports{};
                                std::uint32_t viewportCount =
                                    static_cast<std::uint32_t>(previousViewports.size());
                                context->RSGetViewports(&viewportCount, previousViewports.data());

                                // A render-target view (RTV) makes this texture the output of Draw calls;
                                // its SRV exposes the same storage for later shader reads. The viewport
                                // must match this target's dimensions, not a previously bound upscaler
                                // target, or the fullscreen photograph can be cropped or scaled incorrectly.
                                auto* target = communityShadersHdrTargetView;
                                context->OMSetRenderTargets(1, &target, nullptr);
                                const REX::W32::D3D11_VIEWPORT viewport{
                                    0.0F, 0.0F,
                                    static_cast<float>(communityShadersHdrTargetDesc.width),
                                    static_cast<float>(communityShadersHdrTargetDesc.height),
                                    0.0F, 1.0F
                                };
                                context->RSSetViewports(1, &viewport);
                                CompositeLoadingFrame(context, communityShadersHdrTarget,
                                    communityShadersHdrTargetDesc, true);
                                compositedIntoCommunityShadersHdr = true;

                                auto* previousTarget = boundView.Get();
                                context->OMSetRenderTargets(
                                    previousTarget ? 1U : 0U,
                                    previousTarget ? &previousTarget : nullptr, boundDepth.Get());
                                if (viewportCount > 0) {
                                    context->RSSetViewports(viewportCount, previousViewports.data());
                                }
                            }

                            // While LoadingMenu is active, Community Shaders redirects Scaleform to a
                            // separate transparent UI target and retains the scene in kFRAMEBUFFER.SRV.
                            // Once the menu closes, the watchdog must instead bind the real swap buffer:
                            // CS can leave its lower-resolution internal target and viewport bound here.
                            REX::W32::D3D11_TEXTURE2D_DESC backBufferDesc{};
                            backBuffer->GetDesc(&backBufferDesc);
                            const bool separateUIAvailable =
                                compositeAfterPostProcessing && (loading || waitingForPostProcessing) &&
                                sceneTexture.Get() && boundTexture.Get() &&
                                sceneTexture.Get() != boundTexture.Get();
                            const bool compositeIntoScene =
                                Atomic::get(presentation) != Presentation::seamless &&
                                separateUIAvailable;
                            if (compositedIntoCommunityShadersHdr) {
                                // CS's Present hook consumes the HDR target after this hook returns.
                            } else if (compositeIntoScene) {

                                REX::W32::D3D11_TEXTURE2D_DESC sceneDesc{};
                                sceneTexture->GetDesc(&sceneDesc);

                                REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> sceneView;
                                if (device->CreateRenderTargetView(
                                        sceneTexture.Get(), nullptr, sceneView.GetAddressOf()) >= 0 &&
                                    sceneView.Get()) {

                                    std::array<REX::W32::D3D11_VIEWPORT,
                                        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                                                  previousViewports{};
                                    std::uint32_t viewportCount =
                                        static_cast<std::uint32_t>(previousViewports.size());
                                    context->RSGetViewports(&viewportCount, previousViewports.data());

                                    auto* target = sceneView.Get();
                                    context->OMSetRenderTargets(1, &target, nullptr);
                                    const REX::W32::D3D11_VIEWPORT viewport{
                                        0.0F, 0.0F, static_cast<float>(sceneDesc.width),
                                        static_cast<float>(sceneDesc.height), 0.0F, 1.0F
                                    };
                                    context->RSSetViewports(1, &viewport);

                                    CompositeLoadingFrame(context, sceneTexture.Get(), sceneDesc, true);

                                    auto* previousTarget = boundView.Get();
                                    context->OMSetRenderTargets(
                                        previousTarget ? 1U : 0U,
                                        previousTarget ? &previousTarget : nullptr, boundDepth.Get());
                                    if (viewportCount > 0) {
                                        context->RSSetViewports(
                                            viewportCount, previousViewports.data());
                                    }
                                }
                            } else {

                                // The completed swap buffer contains the final scene and Scaleform output.
                                // Bind it explicitly; drawing with CS's internal viewport still active clips
                                // the cover to the upper-left portion of the output-sized destination rect.
                                const auto& desc = backBufferDesc;

                                REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> backBufferView;
                                if (device->CreateRenderTargetView(
                                        backBuffer.Get(), nullptr, backBufferView.GetAddressOf()) >= 0 &&
                                    backBufferView.Get()) {

                                    std::array<REX::W32::D3D11_VIEWPORT,
                                        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                                                  previousViewports{};
                                    std::uint32_t viewportCount =
                                        static_cast<std::uint32_t>(previousViewports.size());
                                    context->RSGetViewports(&viewportCount, previousViewports.data());

                                    auto* target = backBufferView.Get();
                                    context->OMSetRenderTargets(1, &target, nullptr);
                                    const REX::W32::D3D11_VIEWPORT viewport{
                                        0.0F, 0.0F, static_cast<float>(desc.width),
                                        static_cast<float>(desc.height), 0.0F, 1.0F
                                    };
                                    context->RSSetViewports(1, &viewport);

                                    // Scaleform remains in CS's separate UI texture. Draw only the retained
                                    // background here; the proxy composites that UI texture during Present.
                                    CompositeLoadingFrame(
                                        context, backBuffer.Get(), desc, separateUIAvailable);

                                    auto* previousTarget = boundView.Get();
                                    context->OMSetRenderTargets(
                                        previousTarget ? 1U : 0U,
                                        previousTarget ? &previousTarget : nullptr, boundDepth.Get());
                                    if (viewportCount > 0) {
                                        context->RSSetViewports(
                                            viewportCount, previousViewports.data());
                                    }
                                }
                            }
                        }
                    }
                }

                // Community Shaders composites the live UI target after LoadingMenu closes. Keep a hidden
                // HUD hidden for the retained-frame presentation, and release it on the UI thread
                // once that presentation ends. Movie writes never happen in Present itself.
                const bool hudTransition =
                    Atomic::get(epochActive) ||
                    Atomic::get(preLoadDoorTransitionActive) ||
                    Atomic::get(postLoadFadePending) ||
                    Atomic::get(postLoadFadeStart) > 0;
                const bool wantsHUDHidden =
                    Atomic::get(mainMenuLoadActive) ||
                    !Settings::GetSingleton().ShowHUDDuringLoading();
                if (Atomic::get(hudVisibilityOwned) ||
                    (hudTransition && wantsHUDHidden && !IsVanilla())) {
                    QueueHUDVisibilitySync();
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in IDXGISwapChain::Present");
            }
        }

        Atomic::set(worldRenderedSincePresent, false);
        if (Atomic::get(hooksEnabled) && compositeAfterPostProcessing) {

            auto* observerRenderer = RE::BSGraphics::Renderer::GetSingleton();
            EnsureCopyObserver(observerRenderer ? observerRenderer->GetRuntimeData().context : nullptr);
        }

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            transitionFrameDiagnostics.insideChain = true;
        }

        // Present is the handoff to the swap chain and any chained rendering integrations.
        // Keep retained-image and HUD ownership until that chain has consumed this frame;
        // reaching zero fade alpha earlier in a Draw call is not yet a completed handoff.
        const auto result = originalPresent(a_swapChain, a_syncInterval, a_flags);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            if (transitionFrameDiagnostics.owned) {
                logger::info("transition Present end: frame={} thread={} flags={:08X} result={:08X} displays={} snapshots={} uiPasses={} "
                    "compositions={} unpairedUi={} sceneCopies={} frozenCopies={} sceneDispatches={} displayReads={}",
                    transitionFrameDiagnostics.frame, ::GetCurrentThreadId(), a_flags, static_cast<std::uint32_t>(result),
                    transitionFrameDiagnostics.displays, transitionFrameDiagnostics.snapshots,
                    transitionFrameDiagnostics.uiPasses, transitionFrameDiagnostics.compositions,
                    transitionFrameDiagnostics.unpairedUi, transitionFrameDiagnostics.sceneCopies,
                    transitionFrameDiagnostics.frozenCopies, transitionFrameDiagnostics.sceneDispatches,
                    transitionFrameDiagnostics.displayCopies);
            }

            transitionFrameDiagnostics.insideChain = false;
            transitionFrameDiagnostics.insidePresent = false;
        }

        captureHdrConversionThisPresent = false;
        transitionUiOverlay.BeginPresent();
        if (Settings::GetSingleton().IsTransitionTextureCaptureEnabled()) {
            Atomic::set(diagnosticOutputTarget, 0);
        }

        if (result >= 0 && Atomic::get(hooksEnabled) &&
            !Atomic::get(epochActive) &&
            !Atomic::get(preLoadDoorTransitionActive) &&
            Atomic::get_and_clear(postLoadReleasePending)) {

            FinishPostLoadPresentation();
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("post-load frozen-frame crossfade completed after final Present; releasing UI policy and HUD");
            }
        }

        return result;
    }

    /**
     * @brief Logs the world state at the two render milestones used by this experiment.
     */
    void CellTransitioner::LogRenderState(std::string_view a_timing)
    {
        if (!Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            return;
        }

        const auto* player = RE::PlayerCharacter::GetSingleton();
        const auto* cell = player ? player->GetParentCell() : nullptr;
        logger::info("normal world render {}: cell={:08X} worldRoot={} camera={} player3D={}", a_timing,
            cell ? cell->GetFormID() : 0, RE::Main::WorldRootNode() != nullptr,
            RE::Main::WorldRootCamera() != nullptr, player && player->Get3D() != nullptr);
    }

    /**
     * @brief Corrects excess Improved Camera eye-height displacement during a stationary cell transition.
     *
     * @details Improved Camera applies NPCEyeBone height after Skyrim updates the first-person camera.
     * During a cell handoff that offset can accumulate while Skyrim's camera anchor stays
     * fixed. Correct only that extra positive displacement, before drawing the destination
     * world. The baseline retains Improved Camera's intended first-person offset; moving the
     * player opts out.
     */
    void CellTransitioner::CorrectImprovedCameraTransitionBounce() noexcept
    {
        constexpr float maximumInitialOffset = 8.0F;
        constexpr float movementTolerance = 0.1F;
        constexpr float bounceThreshold = 2.0F;
        constexpr float maximumBounce = 24.0F;

        static bool active = false;
        static bool abandoned = false;
        static float baseline = 0.0F;
        static RE::NiPoint3 initialPlayerPosition{};

        // Master's existing post-load fade flags bound the destination-camera handoff window.
        const bool destinationRendering =
            !Atomic::get(epochActive) &&
            (Atomic::get(postLoadFadePending) ||
                Atomic::get(postLoadFadeStart) > 0);
        if (!Atomic::get(hooksEnabled) || !destinationRendering ||
            !GetModuleHandleW(L"ImprovedCameraSE.dll")) {

            active = false;
            abandoned = false;
            return;
        }

        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!camera || !camera->IsInFirstPerson() || !player ||
            player->AsActorState()->IsWeaponDrawn()) {

            active = false;
            abandoned = true;
            return;
        }

        auto* root = camera->cameraRoot.get();
        auto* rootNode = root ? root->AsNode() : nullptr;
        auto* cameraNode = rootNode && !rootNode->GetChildren().empty() ?
                               rootNode->GetChildren()[0].get() : nullptr;
        if (!root || !cameraNode || abandoned) {
            return;
        }

        const auto anchor = camera->GetRuntimeData2().pos;
        const float offset = root->world.translate.z - anchor.z;
        if (!std::isfinite(offset)) {

            abandoned = true;
            return;
        }

        if (!active) {

            // A large first sample means we missed the stable camera; leave it untouched.
            if (std::abs(offset) > maximumInitialOffset) {

                abandoned = true;
                return;
            }

            baseline = offset;
            initialPlayerPosition = player->GetPosition();
            active = true;
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("Improved Camera transition correction armed: camera offset {:.3f}", baseline);
            }

            return;
        }

        const auto position = player->GetPosition();
        const auto displacement = position - initialPlayerPosition;
        if (displacement.Length() > movementTolerance) {

            abandoned = true;
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("Improved Camera transition correction released for player movement");
            }

            return;
        }

        const float excess = offset - baseline;
        if (excess < -bounceThreshold || excess > maximumBounce) {

            // A new camera state is not the small repeatable handoff pulse.
            abandoned = true;
            return;
        }

        if (excess > bounceThreshold) {

            root->local.translate.z -= excess;
            root->world.translate.z -= excess;
            cameraNode->world.translate.z -= excess;
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("Improved Camera transition camera bounce corrected by {:.3f}", excess);
            }
        }
    }

    /**
     * @brief Observes when Skyrim stops and resumes its normal world-render call.
     */
    void CellTransitioner::ObserveRenderWorld(bool a_firstPerson)
    {
        CorrectImprovedCameraTransitionBounce();
        if (Atomic::get(hooksEnabled) && Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            try {
                auto state = Atomic::get(renderObservationState);
                if (state == 1 && Atomic::compare_and_set(renderObservationState, state, 2, std::memory_order_seq_cst)) {
                    LogRenderState("while Loading Menu is open");
                } else if (state == 3 && Atomic::compare_and_set(renderObservationState, state, 0, std::memory_order_seq_cst)) {
                    LogRenderState("for the first time after Loading Menu closed");
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in the world-render observer");
            }
        }

        if (originalRenderWorld.address()) {

            originalRenderWorld(a_firstPerson);
            Atomic::set(worldRenderedSincePresent, true);
        }
    }

    /**
     * @brief Copies the currently bound world target into the rolling frozen-frame texture.
     */
    void CellTransitioner::CaptureBoundWorldTarget()
    {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        auto* device = RE::BSGraphics::Renderer::GetDevice();
        auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
        if (!device || !context) {
            return;
        }

        REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> renderTargetView;
        context->OMGetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);
        if (!renderTargetView.Get()) {

            logger::warn("no render target was bound before Scaleform rendering");
            return;
        }

        REX::W32::ComPtr<REX::W32::ID3D11Resource>  resource;
        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> boundTarget;
        renderTargetView->GetResource(resource.GetAddressOf());

        const bool isTexture = resource.Get() &&
                               resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                   reinterpret_cast<void**>(boundTarget.GetAddressOf())) >= 0 &&
                               boundTarget.Get();
        if (!isTexture) {
            logger::warn("bound UI render target was not a texture");
        } else {

            auto&                                       framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
            REX::W32::ComPtr<REX::W32::ID3D11Resource>  framebufferResource;
            REX::W32::ComPtr<REX::W32::ID3D11Texture2D> framebufferScene;
            if (framebuffer.SRV) {

                reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
                    framebuffer.SRV)
                    ->GetResource(framebufferResource.GetAddressOf());
                if (framebufferResource.Get()) {
                    framebufferResource->QueryInterface(
                        REX::W32::IID_ID3D11Texture2D,
                        reinterpret_cast<void**>(framebufferScene.GetAddressOf()));
                }
            }

            auto* renderTarget =
                framebufferScene.Get() && framebufferScene.Get() != boundTarget.Get() ?
                    framebufferScene.Get() :
                    boundTarget.Get();

            REX::W32::D3D11_TEXTURE2D_DESC desc{};
            renderTarget->GetDesc(&desc);

            const bool useSceneCapture = compositeAfterPostProcessing;
            const bool prepared = useSceneCapture ?
                                      PrepareSceneFrame(device, desc) :
                                      PrepareFrozenFrame(device, desc);
            if (prepared) {

                // This is a framebuffer-space fallback. HDRDisplay restores kFRAMEBUFFER.SRV after
                // ISHDR, so it is not interchangeable with our primary floating-point HDR capture.
                context->CopyResource(useSceneCapture ? sceneFrame : frozenFrame, renderTarget);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    loggedFrozenPresentation = false;
                }

                if (Settings::GetSingleton().IsLoadingLoggingEnabled() && !loggedFrozenFrame) {

                    if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                        logger::info(
                            "capturing rolling {}x{} world frames before Scaleform ({}; {})",
                            desc.width, desc.height,
                            renderTarget == framebufferScene.Get() &&
                                    framebufferScene.Get() != boundTarget.Get() ?
                                "separate scene target" :
                                "bound target",
                            useSceneCapture ? "CS scene copy" : "primary copy");
                    }

                    loggedFrozenFrame = true;
                }
            }
        }
    }

    /**
     * @brief Captures after Skyrim binds the Scaleform target but before it draws the UI.
     */
    void CellTransitioner::CaptureAfterScaleformBegin(void* a_renderer)
    {
        // The original call must run first because it binds the render target that contains the finished world.
        if (!originalBeginScaleform.address()) {
            return;
        }

        originalBeginScaleform(a_renderer);

        if (Atomic::get(hooksEnabled)) {

            try {
                // Locking preserves the last complete world frame throughout the loading epoch.
                if (!Atomic::get(frozenFrameLocked)) {
                    CaptureBoundWorldTarget();
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in the world-frame capture");
            }
        }
    }

    /**
     * @brief Lets Community Shaders finish upscaling/HDR work, then composites into the final image-space
     * target it leaves bound. This keeps the captured frame and destination in the same temporal
     * and viewport space instead of feeding a retained color image through live DLSS motion data.
     */
    void CellTransitioner::CompositeAfterPostProcessing(
        RE::ImageSpaceManager* a_manager, std::uint32_t a_3, RE::RENDER_TARGET a_target,
        void* a_4, bool a_5)
    {
        if (originalImageSpacePostProcessing) {
            originalImageSpacePostProcessing(a_manager, a_3, a_target, a_4, a_5);
        }

        if (Atomic::get(hooksEnabled)) {

            try {
                auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                auto* device = RE::BSGraphics::Renderer::GetDevice();
                auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
                if (device && context) {

                    REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> targetView;
                    REX::W32::ComPtr<REX::W32::ID3D11DepthStencilView> targetDepth;
                    REX::W32::ComPtr<REX::W32::ID3D11Resource>         targetResource;
                    REX::W32::ComPtr<REX::W32::ID3D11Texture2D>        targetTexture;
                    context->OMGetRenderTargets(
                        1, targetView.GetAddressOf(), targetDepth.GetAddressOf());
                    if (targetView.Get()) {
                        targetView->GetResource(targetResource.GetAddressOf());
                    }

                    if (targetResource.Get()) {
                        targetResource->QueryInterface(
                            REX::W32::IID_ID3D11Texture2D,
                            reinterpret_cast<void**>(targetTexture.GetAddressOf()));
                    }

                    if (targetView.Get() && targetTexture.Get()) {

                        REX::W32::D3D11_TEXTURE2D_DESC desc{};
                        targetTexture->GetDesc(&desc);

                        // Report target changes rather than every frame; CS may bind different outputs
                        // while switching between its HDR scene and the engine framebuffer.
                        static REX::W32::D3D11_TEXTURE2D_DESC lastLoggedDesc{};
                        if (Settings::GetSingleton().IsLoadingLoggingEnabled() &&
                            (desc.width != lastLoggedDesc.width || desc.height != lastLoggedDesc.height ||
                                desc.format != lastLoggedDesc.format)) {

                            logger::info("post-CS output target={}x{} format={} frozen={}x{} format={} locked={} freshWorld={}",
                                desc.width, desc.height, std::to_underlying(desc.format),
                                frozenFrameDesc.width, frozenFrameDesc.height, std::to_underlying(frozenFrameDesc.format),
                                Atomic::get(frozenFrameLocked),
                                Atomic::get(worldRenderedSincePresent));
                            lastLoggedDesc = desc;
                        }

                        // A returned post-processing call can leave a scalar scratch mask bound
                        // instead of the completed scene (observed R8_UNORM at half resolution).
                        // Never capture, draw into, or start a fade against that intermediate.
                        const auto* graphics = RE::BSGraphics::State::GetSingleton();
                        const bool sceneColorFormat = IsRgbaFormat(desc.format) || IsBgraFormat(desc.format) ||
                                                      desc.format == REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM ||
                                                      desc.format == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT;
                        const bool displaySize = graphics && desc.width == graphics->screenWidth &&
                                                 desc.height == graphics->screenHeight;
                        if (!sceneColorFormat || !displaySize || desc.sampleDesc.count != 1) {

                            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

                                static REX::W32::D3D11_TEXTURE2D_DESC lastRejectedDesc{};
                                if (desc.width != lastRejectedDesc.width || desc.height != lastRejectedDesc.height ||
                                    desc.format != lastRejectedDesc.format) {

                                    logger::info("ignoring intermediate post-CS target={}x{} format={}; awaiting display-sized scene color",
                                        desc.width, desc.height, std::to_underlying(desc.format));
                                    lastRejectedDesc = desc;
                                }
                            }

                            return;
                        }

                        const bool freshWorld = Atomic::get(worldRenderedSincePresent);
                        Atomic::set(completedHdrSceneSincePresent, freshWorld);
                        const bool loading = Atomic::get(preLoadDoorTransitionActive) ||
                                             Atomic::get(epochActive);
                        const bool postLoadTransition = Atomic::get(postLoadFadePending) ||
                                                        Atomic::get(postLoadFadeStart) > 0;
                        if (Settings::GetSingleton().IsLoadingLoggingEnabled() && (loading || postLoadTransition)) {
                            logger::info("post-CS scene trace: frame={} insidePresent={} target={:X} expectedScene={:X} "
                                "freshWorld={} locked={} matchesFrozen={} size={}x{} format={}",
                                transitionFrameDiagnostics.frame, transitionFrameDiagnostics.insidePresent,
                                reinterpret_cast<std::uintptr_t>(targetTexture.Get()),
                                reinterpret_cast<std::uintptr_t>(communityShadersHdrTarget), freshWorld,
                                Atomic::get(frozenFrameLocked), MatchesFrozenFrame(desc),
                                desc.width, desc.height, std::to_underlying(desc.format));
                        }

                        // Main-menu black uses no captured pixels. Prepare its binding at
                        // the first valid loading OR destination target, before that frame
                        // can be converted for display and frame-generation UI.
                        if (Atomic::get(mainMenuLoadActive) && (loading || postLoadTransition) &&
                            !MatchesFrozenFrame(desc) && PrepareFrozenFrame(device, desc)) {

                            context->CopyResource(frozenFrame, targetTexture.Get());
                        }

                        if (freshWorld && !loading && postLoadTransition && !MatchesFrozenFrame(desc)) {

                            logger::warn(
                                "no compatible retained scene after loading (source={}x{} format={}, target={}x{} format={}); releasing presentation",
                                frozenFrameDesc.width, frozenFrameDesc.height, std::to_underlying(frozenFrameDesc.format),
                                desc.width, desc.height, std::to_underlying(desc.format));
                            FinishPostLoadPresentation();
                        }

                        // Paused CS menus can produce an SDR scene even when gameplay
                        // uses HDR. Replacing the retained HDR photograph here also drops
                        // its conversion state; the next save load then aborts its fade
                        // on the SDR/HDR mismatch. Keep the last completed gameplay image.
                        auto* ui = RE::UI::GetSingleton();
                        const bool menuSdrFrame = compositeAfterPostProcessing && frozenFrame &&
                            frozenFrameDesc.format == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT &&
                            desc.format != REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT &&
                            ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME));
                        if (menuSdrFrame) {
                            // The photograph did not change, so its HDR encoding
                            // constants must not be recaptured from this menu frame.
                            Atomic::set(completedHdrSceneSincePresent, false);
                        }
                        if (freshWorld && !menuSdrFrame && !Atomic::get(frozenFrameLocked) &&
                            PrepareFrozenFrame(device, desc)) {

                            context->CopyResource(frozenFrame, targetTexture.Get());
                            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                                loggedFrozenPresentation = false;
                            }

                            if (Settings::GetSingleton().IsLoadingLoggingEnabled() && !loggedFrozenFrame) {

                                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                                    logger::info(
                                        "capturing rolling {}x{} world frames after CS post-processing",
                                        desc.width, desc.height);
                                }

                                loggedFrozenFrame = true;
                            }
                        }

                        if (desc.format == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT &&
                            MatchesFrozenFrame(desc) &&
                            communityShadersHdrTarget != targetTexture.Get()) {

                            if (communityShadersHdrTargetView) {
                                communityShadersHdrTargetView->Release();
                            }

                            if (communityShadersHdrTarget) {
                                communityShadersHdrTarget->Release();
                            }

                            communityShadersHdrTarget = targetTexture.Get();
                            communityShadersHdrTarget->AddRef();
                            communityShadersHdrTargetView = targetView.Get();
                            communityShadersHdrTargetView->AddRef();
                            communityShadersHdrTargetDesc = desc;
                        }

                        if (!freshWorld && !loading && Atomic::get(postLoadFadeStart) > 0) {

                            Atomic::set(postLoadReleasePending, false);
                            Atomic::set(postLoadFadeStart, 0);
                            Atomic::set(postLoadFadePending, true);
                        }

                        if (freshWorld && MatchesFrozenFrame(desc) &&
                            !Atomic::get(preLoadDoorTransitionActive) &&
                            !Atomic::get(epochActive) &&
                            Atomic::get_and_clear(postLoadFadePending)) {

                            const auto now = CurrentTimeMilliseconds();
                            Atomic::set(postLoadFadeStart, now);
                            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                                logger::info("world rendering and post-CS compositor resumed; starting post-load fade");
                            }
                        }

                        const bool transitionActive =
                            Atomic::get(preLoadDoorTransitionActive) ||
                            Atomic::get(epochActive) ||
                            Atomic::get(postLoadFadePending) ||
                            Atomic::get(postLoadFadeStart) > 0;
                        if (transitionActive && MatchesFrozenFrame(desc)) {

                            std::array<REX::W32::D3D11_VIEWPORT,
                                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                                          previousViewports{};
                            std::uint32_t viewportCount =
                                static_cast<std::uint32_t>(previousViewports.size());
                            context->RSGetViewports(&viewportCount, previousViewports.data());

                            auto* outputView = targetView.Get();
                            context->OMSetRenderTargets(1, &outputView, nullptr);
                            const REX::W32::D3D11_VIEWPORT viewport{
                                0.0F, 0.0F, static_cast<float>(desc.width),
                                static_cast<float>(desc.height), 0.0F, 1.0F
                            };
                            context->RSSetViewports(1, &viewport);
                            CompositeLoadingFrame(context, targetTexture.Get(), desc, true);
                            Atomic::get_and_add(postProcessingPassesSincePresent, 1);

                            context->OMSetRenderTargets(1, &outputView, targetDepth.Get());
                            if (viewportCount > 0) {
                                context->RSSetViewports(viewportCount, previousViewports.data());
                            }
                        }
                    }
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in post-CS transition compositor");
            }
        }
    }

    /**
     * @brief Records every engine fast-travel fade completion before its shared callback starts the load.
     */
    void CellTransitioner::FastTravelFadeCallbackRun(void* a_callback)
    {
        if (Atomic::get(hooksEnabled)) {

            const bool useCustomTransition =
                Settings::GetSingleton().UseTransitionsForFastTravel();
            Atomic::set(vanillaLoadPending, !useCustomTransition);
            Atomic::set(fastTravelBlackPending, useCustomTransition);
            if (!useCustomTransition) {

                Atomic::set(preLoadOwnedFader, false);
                Atomic::set(loadOwnedFader, false);
                Atomic::set(loadFaderCloseQueued, false);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::info("fast travel selected Skyrim's vanilla loading presentation");
                }
            }
        }

        if (originalFastTravelFadeCallbackRun) {
            originalFastTravelFadeCallbackRun(a_callback);
        }
    }

    /**
     * @brief Records every engine save-load fade completion before its shared callback starts the load.
     */
    void CellTransitioner::SaveLoadFadeCallbackRun(void* a_callback)
    {
        if (Atomic::get(hooksEnabled)) {

            Atomic::set(fastTravelBlackPending, false);
            const bool useCustomTransition =
                Settings::GetSingleton().UseTransitionsForSaveLoads();
            Atomic::set(vanillaLoadPending, !useCustomTransition);
            if (!useCustomTransition) {

                Atomic::set(preLoadOwnedFader, false);
                Atomic::set(loadOwnedFader, false);
                Atomic::set(loadFaderCloseQueued, false);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::info("save load selected Skyrim's vanilla loading presentation");
                }
            }
        }

        if (originalSaveLoadFadeCallbackRun) {
            originalSaveLoadFadeCallbackRun(a_callback);
        }
    }

    /**
     * @brief Restores only the persistent movie state changed by load-fader suppression.
     */
    void CellTransitioner::RestoreFaderPresentation(RE::IMenu* a_menu)
    {
        if (!a_menu || !a_menu->uiMovie ||
            !Atomic::get_and_clear(faderPresentationSuppressed)) {
            return;
        }

        // FaderMenu is persistent. Restore the exact presentation state observed before suppression;
        // forcing an opaque Scaleform background here produces a gray frame during StatsMenu tweens.
        a_menu->uiMovie->SetBackgroundAlpha(Atomic::get(faderBackgroundAlpha));
        a_menu->uiMovie->SetVisible(Atomic::get(faderWasVisible));
    }

    /**
     * @brief Observes native fade requests, protecting only our internal rolling capture while Skyrim
     * fades out. The original FaderData and menu behavior remain untouched for script-driven fades
     * and image modifiers.
     */
    RE::UI_MESSAGE_RESULTS CellTransitioner::FaderMenuProcessMessage(
        RE::IMenu* a_menu, RE::UIMessage& a_message)
    {
        bool queuePostLoadClose = false;
        bool restorePresentation = false;

        if (Atomic::get(hooksEnabled)) {

            const bool activeEpoch = Atomic::get(epochActive);
            const bool postLoadTransition =
                Atomic::get(postLoadFadePending) ||
                Atomic::get(postLoadFadeStart) > 0;

            if (a_message.data &&
                (a_message.type == RE::UI_MESSAGE_TYPE::kShow ||
                    a_message.type == RE::UI_MESSAGE_TYPE::kUpdate)) {

                const auto* data = static_cast<const RE::FaderData*>(a_message.data);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::info("native fader request: message={} callback={:X} out={} black={} pauses={} duration={} minDuration={} epoch={} postLoad={}",
                        std::to_underlying(a_message.type.get()), data->unk10, data->isFadingOut,
                        data->isBlack, data->pausesGame, data->fadeDuration, data->minDuration,
                        activeEpoch, postLoadTransition);
                }

                // SleepWaitMenu queues a callback-free, non-pausing black fade before advancing game
                // time. Sleeping can open LoadingMenu before that queued request is handled, so do not
                // let the ordinary new-black-fader-during-a-load fallback claim it.
                const auto sleepFadeDeadline =
                    Atomic::get(sleepFadeRequestDeadline);
                const bool sleepFadeRequest =
                    sleepFadeDeadline >= CurrentTimeMilliseconds() && !data->unk10 &&
                    data->isFadingOut && data->isBlack && !data->pausesGame;
                const bool preserveSleepFader =
                    sleepFadeRequest || Atomic::get(sleepFaderActive);
                const auto nativeLoadPath = GetNativeLoadPath(*data);
                const bool nativeLoadFade = nativeLoadPath != NativeLoadPath::none && data->isBlack;
                const bool vanillaLoadFade = nativeLoadFade && !UsesCustomTransition(nativeLoadPath);
                if (nativeLoadFade) {
                    Atomic::set(fastTravelBlackPending,
                        nativeLoadPath == NativeLoadPath::fastTravel && !vanillaLoadFade);
                }

                auto*      ui = RE::UI::GetSingleton();
                const bool mapMenuFade = !nativeLoadFade && ui &&
                                         ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
                if (vanillaLoadFade && data->isFadingOut) {

                    Atomic::set(vanillaLoadPending, true);
                    Atomic::set(preLoadOwnedFader, false);
                    Atomic::set(loadOwnedFader, false);
                    Atomic::set(loadFaderCloseQueued, false);
                }

                if (sleepFadeRequest) {

                    Atomic::set(sleepFadeRequestDeadline, 0);
                    Atomic::set(sleepFaderActive, true);
                }

                if (preserveSleepFader) {

                    Atomic::set(preLoadOwnedFader, false);
                    Atomic::set(loadOwnedFader, false);
                    Atomic::set(loadFaderCloseQueued, false);
                }

                if (mapMenuFade) {

                    // MapMenu uses FaderMenu for its own camera transitions when opening and closing.
                    // Do not let stale or fallback load ownership hide that menu-owned fade. A real
                    // fast-travel request remains load-owned because it has a native load callback.
                    Atomic::set(preLoadOwnedFader, false);
                    Atomic::set(loadOwnedFader, false);
                    Atomic::set(loadFaderCloseQueued, false);
                }

                if (Atomic::get(newGameTransitionActive)) {
                    if (!data->isFadingOut && data->isBlack && data->fadeDuration > 0.0F) {
                        Atomic::set(newGameFadeRequestSeen, true);
                    }
                } else if (!preserveSleepFader && !mapMenuFade) {

                    // Static xrefs show that native load fades carry one of four dedicated completion
                    // callbacks. Papyrus FadeOutGame uses the separate callback-free builder, so this
                    // claims the initiating fader without suppressing arbitrary scripted fades.
                    if (nativeLoadFade && !vanillaLoadFade) {

                        Atomic::set(vanillaLoadPending, false);
                        Atomic::set(loadOwnedFader, true);
                        if (!activeEpoch && !postLoadTransition) {

                            Atomic::set(preLoadOwnedFader, true);
                            if (nativeLoadPath == NativeLoadPath::door ||
                                nativeLoadPath == NativeLoadPath::loadSave) {

                                bool expected = false;
                                if (Atomic::compare_and_set(preLoadDoorCaptureLocked, expected, true)) {

                                    // Doors and save loads must lock the last completed world
                                    // image before their native callback changes the world or
                                    // menu render path, rather than at LoadingMenu construction.
                                    Atomic::set(frozenFrameLocked, true);
                                    Atomic::set(presentation, ChoosePresentation());
                                    Atomic::set(preLoadDoorTransitionActive, true);
                                    if (Settings::GetSingleton().IsTransitionTextureCaptureEnabled()) {

                                        std::uint8_t expected = 0;
                                        Atomic::compare_and_set(diagnosticCaptureStage, expected, 1, std::memory_order_seq_cst);
                                    }
                                }
                            }
                        }
                    }

                    // Skyrim can enqueue its control-blocking load fader a few milliseconds after
                    // LoadingMenu closes. Keep ownership through our post-load crossfade and close it
                    // immediately; non-pausing script fades outside this transition window are untouched.
                    if (!vanillaLoadFade && data->isBlack && data->pausesGame &&
                        (activeEpoch || postLoadTransition)) {

                        Atomic::set(loadOwnedFader, true);
                        if (postLoadTransition &&
                            !Atomic::get_and_set(loadFaderCloseQueued, true)) {
                            queuePostLoadClose = true;
                        }
                    }

                    if (!vanillaLoadFade && activeEpoch &&
                        !Atomic::get(faderPresentAtLoadStart) &&
                        data->isBlack) {
                        Atomic::set(loadOwnedFader, true);
                    }
                }

                // A prior load can leave this persistent movie hidden. Restore only for a request
                // outside the load-suppression window (or for explicitly preserved native flows).
                restorePresentation = preserveSleepFader || mapMenuFade || vanillaLoadFade ||
                                      Atomic::get(newGameTransitionActive) ||
                                      (!activeEpoch && !postLoadTransition && !nativeLoadFade);
            } else if (a_message.type == RE::UI_MESSAGE_TYPE::kHide) {

                Atomic::set(sleepFadeRequestDeadline, 0);
                Atomic::set(sleepFaderActive, false);
                if (!activeEpoch) {

                    // FadeThenFastTravelCallback/FadeThenLoadCallback finish before LoadingMenu
                    // opens. Keep their decision latched across this native fader hide so
                    // PrepareForLoad can consume it when the actual load begins.
                    Atomic::set(preLoadOwnedFader, false);
                    Atomic::set(loadOwnedFader, false);
                    Atomic::set(loadFaderCloseQueued, false);
                    Atomic::set(fastTravelBlackActive, false);
                }
            }
        }

        // The original receives the unmodified message and FaderData. Skyrim remains responsible for
        // timing, the fade curve, menu lifetime, pause behavior, and TitleSequence layering.
        const auto result = originalFaderProcessMessage ?
                                originalFaderProcessMessage(a_menu, a_message) :
                                RE::UI_MESSAGE_RESULTS::kPassOn;
        if (Atomic::get(hooksEnabled) &&
            Settings::GetSingleton().IsLoadingLoggingEnabled() && a_menu) {

            logger::info("native fader processed: message={} result={} active={} closeQueued={} owned={}",
                std::to_underlying(a_message.type.get()), std::to_underlying(result),
                static_cast<RE::FaderMenu*>(a_menu)->GetRuntimeData().isActive, queuePostLoadClose,
                Atomic::get(loadOwnedFader));
            // A native request may arrive after EndLoad first reported restored controls.
            Atomic::set(awaitingControlRestore, true);
            ObserveControlRestore();
        }

        if (restorePresentation) {
            RestoreFaderPresentation(a_menu);
        }

        if (queuePostLoadClose) {
            if (auto* messages = RE::UIMessageQueue::GetSingleton()) {
                messages->AddMessage(RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }

        return result;
    }

    /**
     * @brief Advances every FaderMenu normally and hides only the fade-in owned by the active ordinary
     * load.
     */
    void CellTransitioner::FaderMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
    {
        if (originalFaderAdvanceMovie) {
            originalFaderAdvanceMovie(a_menu, a_interval, a_currentTime);
        }

        if (!Atomic::get(hooksEnabled)) {
            return;
        }

        try {
            if (a_menu && a_menu->uiMovie) {

                if (Atomic::get(newGameTransitionActive)) {

                    // Keep Skyrim's native black fade at menu depth 3, below TitleSequenceMenu at depth 4.
                    RestoreFaderPresentation(a_menu);
                    a_menu->uiMovie->SetVisible(true);

                    const bool requestSeen = Atomic::get(newGameFadeRequestSeen);
                    const bool fadeFinished =
                        requestSeen && !static_cast<RE::FaderMenu*>(a_menu)->GetRuntimeData().isActive;
                    if (fadeFinished) {

                        CancelNewGameTransition();
                        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                            logger::info("new-game native fade-in completed; restored custom transition suppression");
                        }
                    }

                    return;
                }

                if (Atomic::get(sleepFaderActive)) {

                    // A load-owned request may have left this persistent movie hidden. Sleeping owns
                    // the complete native fade-out/fade-in lifetime, so explicitly restore it.
                    RestoreFaderPresentation(a_menu);
                    a_menu->uiMovie->SetVisible(true);
                    return;
                }

                if (Atomic::get(fastTravelBlackActive)) {

                    // Map fast travel keeps Skyrim's native FaderMenu above the loading presentation.
                    // Once loading ends, allow the same native movie to perform its destination fade-in.
                    RestoreFaderPresentation(a_menu);
                    if (Atomic::get(epochActive)) {
                        a_menu->uiMovie->SetVisible(true);
                    }

                    return;
                }

                const bool transitionWindow = Atomic::get(epochActive) ||
                                              Atomic::get(postLoadFadePending) ||
                                              Atomic::get(postLoadFadeStart) > 0;
                // Native door, fast-travel, and save-load callbacks submit their fader before
                // LoadingMenu opens. Hide that already-identified load cover immediately so it cannot
                // flash black in the short gap before the captured-frame transition takes over.
                const bool suppressLoadFader =
                    Atomic::get(loadOwnedFader) &&
                    (transitionWindow || Atomic::get(preLoadOwnedFader)) &&
                    !(Atomic::get(fastTravelBlackPending) && !transitionWindow) &&
                    !(Atomic::get(fastTravelBlackActive) &&
                        (Atomic::get(epochActive) ||
                            Atomic::get(postLoadFadePending)));
                if (suppressLoadFader) {

                    bool expected = false;
                    if (Atomic::compare_and_set(faderPresentationSuppressed, expected, true)) {

                        Atomic::set(faderWasVisible, a_menu->uiMovie->GetVisible());
                        Atomic::set(faderBackgroundAlpha, a_menu->uiMovie->GetBackgroundAlpha());
                    }

                    a_menu->uiMovie->SetBackgroundAlpha(0.0F);
                    a_menu->uiMovie->SetVisible(false);
                }
            }
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in FaderMenu::AdvanceMovie");
        }
    }

    /**
     * @brief Suppresses MistMenu presentation only while our loading compositor owns the screen.
     * AdvanceMovie remains completely native: showMist and showLoadScreen are initialization
     * guards, not visibility flags.
     */
    void CellTransitioner::MistMenuPostDisplay(RE::IMenu* a_menu)
    {
        const bool suppressPresentation =
            Atomic::get(hooksEnabled) &&
            (Atomic::get(epochActive) ||
                Atomic::get(postLoadFadePending) ||
                Atomic::get(postLoadFadeStart) > 0 ||
                Atomic::get(fastTravelBlackActive) ||
                Atomic::get(newGameTransitionActive));
        if (!suppressPresentation && originalMistPostDisplay) {
            originalMistPostDisplay(a_menu);
        }
    }

    /**
     * @brief Closes only the FaderMenu claimed by this load, plus the load-specific MistMenu.
     */
    void CellTransitioner::CloseResidualLoadingMenus(bool a_preserveFader)
    {
        auto* ui = RE::UI::GetSingleton();
        auto* messages = RE::UIMessageQueue::GetSingleton();
        if (!ui || !messages) {

            logger::warn("could not close residual loading menus because UI services were unavailable");
            return;
        }

        const bool closeFader = !a_preserveFader &&
                                Atomic::get_and_clear(loadOwnedFader);
        Atomic::set(preLoadOwnedFader, false);
        if (!a_preserveFader) {
            Atomic::set(faderPresentAtLoadStart, false);
        }

        if (closeFader && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME) &&
            !Atomic::get_and_set(loadFaderCloseQueued, true)) {
            messages->AddMessage(RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }

        if (ui->IsMenuOpen(RE::MistMenu::MENU_NAME)) {
            messages->AddMessage(RE::MistMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    namespace transitions
    {
        namespace
        {
            // Hooks the shared fade-completion callback used by fast travel from every entry point.
            void InstallFastTravelFadeCallbackHook()
            {
                constexpr std::size_t           runIndex = 0x01;
                REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE___FadeThenFastTravelCallback[0] };
                if (!vtable.address()) {
                    throw std::runtime_error("could not resolve the FadeThenFastTravelCallback vtable");
                }

                const auto originalAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + runIndex * sizeof(std::uintptr_t));
                if (!CellTransitioner::IsExecutableAddress(originalAddress)) {
                    throw std::runtime_error("FadeThenFastTravelCallback::Run had no original function");
                }

                CellTransitioner::originalFastTravelFadeCallbackRun =
                    reinterpret_cast<decltype(CellTransitioner::originalFastTravelFadeCallbackRun)>(
                        originalAddress);
                vtable.write_vfunc(runIndex, CellTransitioner::FastTravelFadeCallbackRun);
            }

            // Hooks the shared fade-completion callback used whenever Skyrim loads a save.
            void InstallSaveLoadFadeCallbackHook()
            {
                constexpr std::size_t           runIndex = 0x01;
                REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE___FadeThenLoadCallback[0] };
                if (!vtable.address()) {
                    throw std::runtime_error("could not resolve the FadeThenLoadCallback vtable");
                }

                const auto originalAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + runIndex * sizeof(std::uintptr_t));
                if (!CellTransitioner::IsExecutableAddress(originalAddress)) {
                    throw std::runtime_error("FadeThenLoadCallback::Run had no original function");
                }

                CellTransitioner::originalSaveLoadFadeCallbackRun =
                    reinterpret_cast<decltype(CellTransitioner::originalSaveLoadFadeCallbackRun)>(
                        originalAddress);
                vtable.write_vfunc(runIndex, CellTransitioner::SaveLoadFadeCallbackRun);
            }

            // Replaces the FaderMenu update gate while preserving its normal movie bookkeeping.
            void InstallFaderMenuHook()
            {
                // CommonLib's IMenu vtable maps slots 4 and 5 to ProcessMessage and AdvanceMovie.
                constexpr std::size_t           processMessageIndex = 0x04;
                constexpr std::size_t           advanceMovieIndex = 0x05;
                REL::Relocation<std::uintptr_t> vtable{ RE::FaderMenu::VTABLE[0] };
                if (!vtable.address()) {
                    throw std::runtime_error("could not resolve the FaderMenu vtable");
                }

                const auto originalProcessAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + processMessageIndex * sizeof(std::uintptr_t));
                const auto originalAdvanceAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + advanceMovieIndex * sizeof(std::uintptr_t));
                if (!CellTransitioner::IsExecutableAddress(originalProcessAddress) ||
                    !CellTransitioner::IsExecutableAddress(originalAdvanceAddress)) {
                    throw std::runtime_error("FaderMenu hooks had no original function");
                }

                CellTransitioner::originalFaderProcessMessage =
                    reinterpret_cast<decltype(CellTransitioner::originalFaderProcessMessage)>(
                        originalProcessAddress);
                CellTransitioner::originalFaderAdvanceMovie =
                    reinterpret_cast<CellTransitioner::AdvanceMovie_t>(originalAdvanceAddress);
                vtable.write_vfunc(processMessageIndex, CellTransitioner::FaderMenuProcessMessage);
                vtable.write_vfunc(advanceMovieIndex, CellTransitioner::FaderMenuAdvanceMovie);
                logger::info("installed load-owned FaderMenu tracking hooks");
            }

            // Suppresses MistMenu drawing only while the transition compositor owns presentation.
            void InstallMistMenuHooks()
            {
                // AdvanceMovie remains native so MistMenu can initialize and update its scene graph normally.
                constexpr std::size_t           postDisplayIndex = 0x06;
                REL::Relocation<std::uintptr_t> vtable{ RE::MistMenu::VTABLE[0] };
                if (!vtable.address()) {
                    throw std::runtime_error("could not resolve the MistMenu vtable");
                }

                const auto postDisplayAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + postDisplayIndex * sizeof(std::uintptr_t));
                if (!CellTransitioner::IsExecutableAddress(postDisplayAddress)) {
                    throw std::runtime_error("MistMenu::PostDisplay had no original function");
                }

                CellTransitioner::originalMistPostDisplay =
                    reinterpret_cast<CellTransitioner::PostDisplay_t>(postDisplayAddress);
                vtable.write_vfunc(postDisplayIndex, CellTransitioner::MistMenuPostDisplay);
                logger::info("installed load-scoped MistMenu presentation hook");
            }

            // Observes the normal world-render call without changing when Skyrim is allowed to render.
            void InstallRenderObservationHook()
            {
                // Main::DrawWorld contains one call to the Address Library identity for the normal
                // world renderer. Resolving the pair keeps this hook stable when instructions before
                // the call grow or shrink between runtime builds.
                constexpr std::size_t relativeCallSize = 5;
                const auto [callSite, currentTarget] = CellTransitioner::FindChainableRelativeCall(
                    IDs::NormalWorldRenderCaller, IDs::NormalWorldRenderer,
                    Offsets::NormalWorldRenderCall.Get(),
                    "normal-world-render call");
                CellTransitioner::originalRenderWorld =
                    currentTarget;
                SKSE::GetTrampoline().write_call<relativeCallSize>(
                    callSite, CellTransitioner::ObserveRenderWorld);
                logger::info(
                    "installed passive normal-world-render observation hook at {:X}; chained target {:X}",
                    callSite, currentTarget);
            }

            // Installs the rolling world-only capture after target binding and immediately before Scaleform draws.
            void InstallWorldCaptureHook()
            {
                // The helper is called elsewhere in Skyrim, so its function entry is not hooked. Only
                // the call owned by the UI render function is replaced; this preserves the narrow point
                // after target setup and before HUD, console, or menu movies enter the captured texture.
                constexpr std::size_t relativeCallSize = 5;
                const auto [callSite, currentTarget] = CellTransitioner::FindChainableRelativeCall(
                    IDs::ScaleformRenderCaller, IDs::ScaleformBeginHelper,
                    Offsets::ScaleformBeginCall.Get(),
                    "Scaleform-begin call");
                CellTransitioner::originalBeginScaleform =
                    currentTarget;
                SKSE::GetTrampoline().write_call<relativeCallSize>(
                    callSite, CellTransitioner::CaptureAfterScaleformBegin);
                logger::info(
                    "installed world-only capture hook after Scaleform target binding at {:X}; chained target {:X}",
                    callSite, currentTarget);
            }

            // Chain Community Shaders first, then composite on the completed image-space target it
            // leaves bound. The Present watchdog covers frames where that render path is suspended.
            void InstallCommunityShadersCompositeHook()
            {
                if (!GetModuleHandleW(L"CommunityShaders.dll")) {
                    return;
                }

                constexpr std::size_t           relativeCallSize = 5;
                REL::Relocation<std::uintptr_t> caller{ IDs::ImageSpacePostProcessingCaller };
                const auto                      callSite = caller.address() + Offsets::ImageSpacePostProcessingCall.Get();
                if (!caller.address() || *reinterpret_cast<const std::uint8_t*>(callSite) != 0xE8) {
                    throw std::runtime_error("Community Shaders post-processing site was not a relative call");
                }

                std::int32_t displacement = 0;
                std::memcpy(&displacement,
                    reinterpret_cast<const void*>(callSite + 1), sizeof(displacement));
                const auto currentTarget = callSite + relativeCallSize + displacement;
                if (!CellTransitioner::IsExecutableAddress(currentTarget)) {
                    throw std::runtime_error("Community Shaders post-processing call had no executable target");
                }

                CellTransitioner::originalImageSpacePostProcessing =
                    reinterpret_cast<CellTransitioner::ImageSpacePostProcessing_t>(currentTarget);
                SKSE::GetTrampoline().write_call<relativeCallSize>(
                    callSite, CellTransitioner::CompositeAfterPostProcessing);
                CellTransitioner::compositeAfterPostProcessing = true;
                logger::info(
                    "installed post-CS Community Shaders transition compositor at {:X}; chained target {:X}",
                    callSite, currentTarget);
            }

            // Installs the final compositor gate and creates the shaders/state reused by every presented frame.
            void InstallFrozenFrameHook()
            {
                // IDXGISwapChain's COM ABI defines Present as vtable slot 8.
                constexpr std::size_t presentIndex = 0x08;

                auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
                if (!window || !window->swapChain) {
                    throw std::runtime_error("could not find Skyrim's swap chain");
                }

                auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                auto* device = RE::BSGraphics::Renderer::GetDevice();
                auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
                if (!renderer || !device || !context) {
                    throw std::runtime_error("could not find Skyrim's D3D11 device or context");
                }

                CellTransitioner::spriteBatch = std::make_unique<DirectX::SpriteBatch>(
                    reinterpret_cast<::ID3D11DeviceContext*>(context));
                CellTransitioner::commonStates =
                    std::make_unique<DirectX::CommonStates>(reinterpret_cast<::ID3D11Device*>(device));

                // Even with zero blur, retained scene alpha must not control photograph opacity.
                if (!CellTransitioner::CreateFrozenFrameBlurShader(reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the frozen-frame shader");
                }

                if (!Settings::GetSingleton().IsBlurEnabled()) {
                    logger::info("frozen-frame blur is disabled; photograph opacity shader remains active");
                }

                if (!CellTransitioner::CreateSolidColorShader(reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the solid-color shader");
                }

                if (!CellTransitioner::CreateLoadingOverlayShader(reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the loading overlay shader");
                }

                // Compile outside the transition so the first covered frame does not hitch.
                if (CellTransitioner::compositeAfterPostProcessing) {

                    const auto conversionStatus = retainedHdrConversion.Initialize(reinterpret_cast<::ID3D11Device*>(device));
                    if (FAILED(conversionStatus)) {
                        logger::warn("could not initialize retained HDR conversion ({:08X}); using native scene conversion",
                            static_cast<std::uint32_t>(conversionStatus));
                    }

                    const auto overlayStatus = transitionUiOverlay.Initialize(reinterpret_cast<::ID3D11Device*>(device));
                    if (FAILED(overlayStatus)) {
                        logger::warn("could not initialize native transition UI overlay ({:08X}); retaining scene composition",
                            static_cast<std::uint32_t>(overlayStatus));
                    } else {
                        logger::info("native HDR transition UI overlay ready; Community Shaders instructions are not patched");
                    }
                }

                // Patch Present only after every resource needed by its callback is ready.
                const auto vtableAddress = *reinterpret_cast<std::uintptr_t*>(window->swapChain);
                if (!vtableAddress) {
                    throw std::runtime_error("Skyrim's swap chain had no vtable");
                }

                REL::Relocation<std::uintptr_t> vtable{ vtableAddress };
                const auto                      originalAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + presentIndex * sizeof(std::uintptr_t));
                if (!CellTransitioner::IsExecutableAddress(originalAddress)) {
                    throw std::runtime_error("IDXGISwapChain::Present had no original function");
                }

                CellTransitioner::originalPresent =
                    reinterpret_cast<CellTransitioner::Present_t>(originalAddress);

                REX::W32::MEMORY_BASIC_INFORMATION presentMemory{};
                const auto communityShaders = GetModuleHandleW(L"CommunityShaders.dll");
                if (communityShaders &&
                    REX::W32::VirtualQuery(
                        reinterpret_cast<const void*>(originalAddress), &presentMemory,
                        sizeof(presentMemory)) != 0) {
                    CellTransitioner::communityShadersFrameGenerationProxy =
                        presentMemory.allocationBase == communityShaders;
                }

                vtable.write_vfunc(presentIndex, CellTransitioner::PresentFrozenFrame);

                if (CellTransitioner::compositeAfterPostProcessing && context) {

                    const bool installed = CellTransitioner::EnsureCopyObserver(context);
                    logger::info("CS HDR dispatch hook installed={}; sprite pipeline state preserved; GPU readbacks opt-in={}",
                        /**
                         * @brief Returns the settings instance loaded before the transition hooks are
                         * installed.
                         */
                        installed, Settings::GetSingleton().IsTransitionTextureCaptureEnabled());
                }

                logger::info(
                    "installed frozen-frame swap-chain Present hook; CS frame-generation proxy={}",
                    CellTransitioner::communityShadersFrameGenerationProxy);
            }

        }

        /**
         * @brief Installs renderer and visual-transition hooks.
         */
        void InstallHooks()
        {
            CellTransitioner::GetSingleton();

            InstallRenderObservationHook();
            InstallWorldCaptureHook();
            InstallCommunityShadersCompositeHook();
            InstallFrozenFrameHook();
            InstallFastTravelFadeCallbackHook();
            InstallSaveLoadFadeCallbackHook();
            InstallFaderMenuHook();
            InstallMistMenuHooks();
            Atomic::set(CellTransitioner::hooksEnabled, true);
            logger::info("transition recovery enabled; engine pause counter and player controls remain native");
        }
    }
}
