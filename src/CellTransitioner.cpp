// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"
#include "LoadingProgress.h"

#include <hde64.h>

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
// 3. Before the renderer becomes volatile, the locked capture is blurred/prepared once into a separate
//    immutable shader resource. It is never an alias of Skyrim's world, HDR, or swap-chain targets.
// 4. FaderMenu remains the stable UI owner while its PostDisplay hook confirms that Scaleform has
//    actually drawn the opaque bridge. Present is only the final SDR watchdog if that bridge or the
//    normal image-space compositor cannot safely cover a frame.
// 5. EndLoad enters a stability gate. Only consecutive completed destination renders hand the same
//    resource to the ordinary compositor, so the fade begins with no copy and no one-frame gap.
// 6. Missing resources or invalid states fail closed to opaque black and restore FaderMenu.
//
// FaderMenu tracking hides only native load fades during the transition window. Scripted fades and
// image-space modifiers keep their original behavior. MistMenu hooks remove its mist/model layer, and
// the RenderWorld hook does not force the renderer to run. Its optional Improved Camera compatibility
// correction affects only a bounded first-person destination-camera offset during custom handoffs.

namespace load_progress
{
    namespace
    {
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

        constexpr std::uint32_t firstPersonPostHandoffMinimumFrames = 8;
        constexpr std::int64_t  firstPersonPostHandoffMinimumObservation = 500;

    }

    // Returns the singleton that owns all cell-transition state.
    CellTransitioner& CellTransitioner::GetSingleton()
    {
        static CellTransitioner singleton;
        return singleton;
    }

    // Disables transition hooks and restores presentation mutated while the compositor owned the screen.
    void CellTransitioner::DisableHooks(std::string_view a_reason) noexcept
    {
        // Coordinate with LoadingProgress so one fail-closed path cannot leave the other armed.
        DisablePlugin(a_reason);
    }

    // Clears compositor atomics and restores Fader/HUD. Invoked only from DisablePlugin.
    void CellTransitioner::ResetOnDisable() noexcept
    {
        hooksEnabled.store(false, std::memory_order_release);
        epochActive.store(false, std::memory_order_release);
        frozenFrameLocked.store(false, std::memory_order_release);
        preLoadDoorCaptureLocked.store(false, std::memory_order_release);
        preLoadDoorTransitionActive.store(false, std::memory_order_release);
        preLoadOwnedFader.store(false, std::memory_order_release);
        vanillaLoadPending.store(false, std::memory_order_release);
        fastTravelBlackPending.store(false, std::memory_order_release);
        fastTravelBlackActive.store(false, std::memory_order_release);
        loadOwnedFader.store(false, std::memory_order_release);
        loadFaderCloseQueued.store(false, std::memory_order_release);
        sleepFadeRequestDeadline.store(0, std::memory_order_release);
        sleepFaderActive.store(false, std::memory_order_release);
        awaitingControlRestore.store(false, std::memory_order_release);
        newGameTransitionActive.store(false, std::memory_order_release);
        newGameFadeRequestSeen.store(false, std::memory_order_release);
        mainMenuLoadPending.store(false, std::memory_order_release);
        mainMenuLoadActive.store(false, std::memory_order_release);
        postLoadFadePending.store(false, std::memory_order_release);
        postLoadFadeStart.store(0, std::memory_order_release);
        postLoadFadeRequestedAt.store(0, std::memory_order_release);
        postLoadPresentFallback.store(false, std::memory_order_release);
        stableUICompositeAvailable.store(false, std::memory_order_release);
        transitionState.store(TransitionState::rollingCapture, std::memory_order_release);
        destinationWorldFrames.store(0, std::memory_order_release);
        stablePipelineFrames.store(0, std::memory_order_release);
        firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
        firstPersonPostHandoffQualificationStart.store(0, std::memory_order_release);
        firstPersonPostHandoffRenderedFrames.store(0, std::memory_order_release);
        fallbackFaderActiveSeen.store(false, std::memory_order_release);
        if (faderBridgeActive.load(std::memory_order_acquire)) {
            QueueFaderBridge(false);
        }
        faderBridgeActive.store(false, std::memory_order_release);
        faderBridgeReleaseQueued.store(false, std::memory_order_release);
        faderBridgeDisplaySerial.store(0, std::memory_order_release);
        dominantColorPending.store(false, std::memory_order_release);
        loadingTransitionStart.store(0, std::memory_order_release);
        loadingMenuFadeElapsedMs.store(0, std::memory_order_release);
        ReleasePersistentTransitionFrame();

        // FaderMenuAdvanceMovie returns early once hooksEnabled is false, so any movie we hid for
        // load ownership must be restored here. Every UI/D3D pointer is treated as possibly null.
        try {
            if (faderPresentationSuppressed.load(std::memory_order_acquire)) {
                auto* ui = RE::UI::GetSingleton();
                if (!ui) {
                    faderPresentationSuppressed.store(false, std::memory_order_release);
                } else {
                    auto menu = ui->GetMenu(RE::FaderMenu::MENU_NAME);
                    if (menu && menu->uiMovie) {
                        RestoreFaderPresentation(menu.get());
                    } else {
                        faderPresentationSuppressed.store(false, std::memory_order_release);
                    }
                }
            }
        } catch (...) {
            faderPresentationSuppressed.store(false, std::memory_order_release);
            REX::W32::OutputDebugStringA(
                "Skyrim Load Progress: could not restore FaderMenu presentation during disable\n");
        }

        RestoreHUDVisibility();
    }

    // Checks that a callback target is committed executable memory.
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
    // RtlLookupFunctionEntry reads the executable's x64 unwind table. Its begin/end RVAs provide the
    // exact compiled-function boundary, keeping the search out of adjacent functions and padding. The
    // resolver fails closed if the boundary is invalid, the call is absent, or more than one matching
    // call exists. A future runtime therefore disables plugin initialization instead of patching an
    // uncertain instruction.
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

    // Resolves a render call while preserving an existing hook installed by another plugin.
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

    // Locks the last world frame and configures the selected loading presentation.
    CellTransitioner::Presentation CellTransitioner::PrepareForLoad(RE::IMenu* a_menu)
    {
        const auto selected = ChoosePresentation();
        presentation.store(selected, std::memory_order_release);

        if (selected == Presentation::vanilla) {
            ReleasePersistentTransitionFrame();
            transitionState.store(TransitionState::rollingCapture, std::memory_order_release);
            frozenFrameLocked.store(false, std::memory_order_release);
            preLoadDoorCaptureLocked.store(false, std::memory_order_release);
            preLoadDoorTransitionActive.store(false, std::memory_order_release);
            postLoadFadeStart.store(0, std::memory_order_release);
            postLoadFadePending.store(false, std::memory_order_release);
            postLoadFadeRequestedAt.store(0, std::memory_order_release);
            postLoadPresentFallback.store(false, std::memory_order_release);
            loadingTransitionStart.store(0, std::memory_order_release);
            loadingMenuFadeElapsedMs.store(0, std::memory_order_release);
            dominantColorPending.store(false, std::memory_order_release);
            RestoreHUDVisibility();
            return selected;
        }

        // This is the capture gate. Once closed, the rolling texture remains the last pre-load world frame.
        frozenFrameLocked.store(true, std::memory_order_release);
        ReleasePersistentTransitionFrame();
        transitionState.store(TransitionState::preparing, std::memory_order_release);
        destinationWorldFrames.store(0, std::memory_order_release);
        stablePipelineFrames.store(0, std::memory_order_release);
        firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
        firstPersonPostHandoffQualificationStart.store(0, std::memory_order_release);
        firstPersonPostHandoffRenderedFrames.store(0, std::memory_order_release);
        preLoadDoorCaptureLocked.store(false, std::memory_order_release);
        postLoadFadeStart.store(0, std::memory_order_release);
        postLoadFadePending.store(false, std::memory_order_release);
        postLoadFadeRequestedAt.store(0, std::memory_order_release);
        postLoadPresentFallback.store(false, std::memory_order_release);
        postProcessingPassesSincePresent.store(0, std::memory_order_release);
        // Menu construction and renderer suspension can consume the configured fade before the first
        // loading frame is presented. Start the visible color fade from Present instead.
        loadingTransitionStart.store(0, std::memory_order_release);
        // Scaleform fade elapsed is advanced only from LoadingMenu::AdvanceMovie.
        loadingMenuFadeElapsedMs.store(0, std::memory_order_release);

        const bool selectCapturedColor =
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color &&
            colorSource.load(std::memory_order_acquire) == Settings::ColorSource::dominant;

        dominantColorPending.store(selectCapturedColor, std::memory_order_release);

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

    // Marks the transition as actively loading.
    void CellTransitioner::BeginLoad()
    {
        if (presentation.load(std::memory_order_acquire) == Presentation::vanilla) {
            preLoadOwnedFader.store(false, std::memory_order_release);
            fastTravelBlackPending.store(false, std::memory_order_release);
            fastTravelBlackActive.store(false, std::memory_order_release);
            loadOwnedFader.store(false, std::memory_order_release);
            loadFaderCloseQueued.store(false, std::memory_order_release);
            epochActive.store(false, std::memory_order_release);
            preLoadDoorTransitionActive.store(false, std::memory_order_release);
            renderObservationState.store(0, std::memory_order_release);
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        faderPresentAtLoadStart.store(
            ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME), std::memory_order_release);
        // The native fader that initiates a door, fast-travel, or save load is submitted before
        // LoadingMenu opens. Preserve the callback-derived ownership across that boundary.
        loadOwnedFader.store(
            preLoadOwnedFader.exchange(false, std::memory_order_acq_rel),
            std::memory_order_release);
        loadFaderCloseQueued.store(false, std::memory_order_release);

        // Present uses this gate to choose an active loading presentation instead of the post-load fade.
        epochActive.store(true, std::memory_order_release);
        preLoadDoorTransitionActive.store(false, std::memory_order_release);
        fastTravelBlackPending.store(false, std::memory_order_release);
        renderObservationState.store(1, std::memory_order_release);

        // A Main Menu save load has no useful captured world image, so its persistent texture is
        // intentionally black. Arm the native black movie underneath LoadingMenu well before the
        // renderer suspension ends; unlike a late Present draw, this layer is part of Skyrim's final
        // UI composition and is already opaque when LoadingMenu closes.
        if (mainMenuLoadActive.load(std::memory_order_acquire) && !QueueFaderBridge(true)) {
            EnterFaderFallback("could not arm the native main-menu save-load bridge");
        }

        const auto state = transitionState.load(std::memory_order_acquire);
        if (state != TransitionState::preparing && state != TransitionState::uiHold &&
            state != TransitionState::faderFallback) {
            EnterFaderFallback("load began without a prepared transition owner");
        }
    }

    // Starts the retained-frame fade and post-load control diagnostics.
    void CellTransitioner::EndLoad()
    {
        // MQ101 owns its first-gameplay fade. Its native FaderMenu remains below TitleSequenceMenu, so release
        // our loading cover without adding the ordinary post-load compositor above those title cards.
        const bool newGame = newGameTransitionActive.load(std::memory_order_acquire);
        const bool vanilla = presentation.load(std::memory_order_acquire) == Presentation::vanilla;
        const bool nativeFastTravelFade = fastTravelBlackActive.load(std::memory_order_acquire);

        const auto now = CurrentTimeMilliseconds();
        const bool faderFallback = transitionState.load(std::memory_order_acquire) ==
                                   TransitionState::faderFallback;
        const bool customFade = !newGame && !vanilla && !nativeFastTravelFade && !faderFallback;
        postLoadFadeStart.store(0, std::memory_order_release);
        postLoadFadeRequestedAt.store(customFade ? now : 0, std::memory_order_release);
        postLoadFadePending.store(customFade || faderFallback, std::memory_order_release);
        postLoadPresentFallback.store(false, std::memory_order_release);
        postProcessingPassesSincePresent.store(0, std::memory_order_release);
        destinationWorldFrames.store(0, std::memory_order_release);
        stablePipelineFrames.store(0, std::memory_order_release);
        firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
        firstPersonPostHandoffQualificationStart.store(0, std::memory_order_release);
        firstPersonPostHandoffRenderedFrames.store(0, std::memory_order_release);

        if (customFade) {
            // Publish the successor owner before releasing the loading gate. Present can therefore
            // observe either owner during this crossover, but never a frame with neither owner active.
            transitionState.store(
                TransitionState::waitingForStableRenderer, std::memory_order_release);
            epochActive.store(false, std::memory_order_release);
        } else if (faderFallback) {
            // The emergency black compositor was armed above; FaderMenu remains the final owner.
            epochActive.store(false, std::memory_order_release);
        } else {
            // Native/new-game paths intentionally release custom presentation immediately.
            epochActive.store(false, std::memory_order_release);
            transitionState.store(TransitionState::rollingCapture, std::memory_order_release);
        }
        preLoadDoorTransitionActive.store(false, std::memory_order_release);
        // Keep the retained frame up after LoadingMenu closes (especially during the image-space
        // fade). Restoring HUD here paints health/stamina over that cover even when
        // show_hud_during_loading is false. Hold the hide until the presentation finishes.
        const bool holdHUDForTransition =
            !newGame && !vanilla && !nativeFastTravelFade &&
            (mainMenuLoadActive.load(std::memory_order_acquire) ||
                !Settings::GetSingleton().ShowHUDDuringLoading());
        if (holdHUDForTransition) {
            HideHUDForLoad();
        } else {
            RestoreHUDVisibility();
        }
        if (newGame || vanilla || nativeFastTravelFade) {
            frozenFrameLocked.store(false, std::memory_order_release);
            ReleasePersistentTransitionFrame();
        }
        if (newGame) {
            logger::debug("handed the new-game transition from the loading compositor to Skyrim's FaderMenu");
        }

        const auto renderState = renderObservationState.load(std::memory_order_acquire);
        if (renderState == 1 || renderState == 2) {
            renderObservationState.store(3, std::memory_order_release);
        }

        {
            std::scoped_lock lock(controlStateLock);
            lastControlState.reset();
        }
        awaitingControlRestore.store(true, std::memory_order_release);
        if (!newGame && !vanilla) {
            CloseResidualLoadingMenus(
                nativeFastTravelFade || faderFallback ||
                faderBridgeActive.load(std::memory_order_acquire));
        }
    }

    // Arms the one-time black loading presentation before SKSE lets Skyrim create the new game.
    void CellTransitioner::BeginNewGameTransition()
    {
        newGameFadeRequestSeen.store(false, std::memory_order_release);
        newGameTransitionActive.store(true, std::memory_order_release);
        logger::debug("armed the new-game black/title-sequence transition");
    }

    // Clears a pending intro when a save load supersedes it or transition hooks fail.
    void CellTransitioner::CancelNewGameTransition()
    {
        newGameTransitionActive.store(false, std::memory_order_release);
        newGameFadeRequestSeen.store(false, std::memory_order_release);
    }

    // Remembers the Main Menu as the origin after its movie closes and before LoadingMenu opens.
    void CellTransitioner::ObserveMainMenuOpening()
    {
        mainMenuLoadPending.store(true, std::memory_order_release);
    }

    // Reapplies the load-scoped HUD policy when Skyrim creates HUDMenu during a Main Menu save load.
    void CellTransitioner::ObserveHUDMenuOpening()
    {
        if (frozenFrameLocked.load(std::memory_order_acquire) &&
            postLoadFadeStart.load(std::memory_order_acquire) <= 0) {
            HideHUDForLoad();
        }
    }

    // Hides only the top-level HUD movie, preserving all child alpha and animation state.
    // Main Menu save loads never expose gameplay HUD; the setting applies only to in-game transitions.
    void CellTransitioner::HideHUDForLoad()
    {
        if (IsVanilla()) {
            return;
        }

        const bool showHUD = Settings::GetSingleton().ShowHUDDuringLoading();
        const bool forceHidden = mainMenuLoadActive.load(std::memory_order_acquire);
        if (showHUD && !forceHidden) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        auto  movie = ui ? ui->GetMovieView(RE::HUDMenu::MENU_NAME) : nullptr;
        if (!movie) {
            return;
        }

        if (!hudVisibilityOwned.load(std::memory_order_acquire)) {
            // Dialogue and other modal menus can temporarily hide HUDMenu before initiating a load.
            // Do not claim an already-hidden movie: Skyrim may restore it while LoadingMenu is open,
            // and writing the stale hidden state back at EndLoad would leave the gameplay HUD disabled.
            if (!movie->GetVisible()) {
                return;
            }

            hudVisibilityOwned.store(true, std::memory_order_release);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("claimed visible HUDMenu for loading");
            }
        }

        movie->SetVisible(false);
    }

    // Reapplies or releases the HUD policy. Safe to schedule from Present; the movie is touched
    // only inside the UI task.
    void CellTransitioner::QueueHUDVisibilitySync() noexcept
    {
        if (hudSyncQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            hudSyncQueued.store(false, std::memory_order_release);
            return;
        }

        tasks->AddUITask([]() {
            hudSyncQueued.store(false, std::memory_order_release);
            if (!hooksEnabled.load(std::memory_order_acquire)) {
                RestoreHUDVisibility();
                return;
            }

            const bool transitionVisible =
                epochActive.load(std::memory_order_acquire) ||
                preLoadDoorTransitionActive.load(std::memory_order_acquire) ||
                postLoadFadePending.load(std::memory_order_acquire) ||
                postLoadFadeStart.load(std::memory_order_acquire) > 0;
            const bool hide =
                transitionVisible && !IsVanilla() &&
                (mainMenuLoadActive.load(std::memory_order_acquire) ||
                    !Settings::GetSingleton().ShowHUDDuringLoading());
            if (hide) {
                HideHUDForLoad();
            } else {
                RestoreHUDVisibility();
            }
        });
    }

    // Restores exactly the movie visibility observed before this transition claimed it.
    void CellTransitioner::RestoreHUDVisibility() noexcept
    {
        if (!hudVisibilityOwned.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        try {
            auto* ui = RE::UI::GetSingleton();
            auto  movie = ui ? ui->GetMovieView(RE::HUDMenu::MENU_NAME) : nullptr;
            if (movie) {
                // Ownership is acquired only when this class changes the movie from visible to hidden.
                movie->SetVisible(true);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::debug("restored HUDMenu visibility=true");
                }
            }
        } catch (...) {
            REX::W32::OutputDebugStringA(
                "Skyrim Load Progress: could not restore HUDMenu visibility\n");
        }
    }

    // Returns whether the current transition suppresses LoadingMenu Scaleform.
    bool CellTransitioner::IsSeamless()
    {
        return presentation.load(std::memory_order_acquire) == Presentation::seamless;
    }

    // Returns whether Skyrim owns the current load presentation without compositor or menu suppression.
    bool CellTransitioner::IsVanilla()
    {
        return presentation.load(std::memory_order_acquire) == Presentation::vanilla;
    }

    // Computes LoadingMenu Scaleform opacity for custom cold presentations.
    // The active-grid residency probe can classify an upcoming warm destination as cold; fading the
    // menu in gives the retained frame time to cover that false-cold pop (see GetQueuedDestinationCell).
    // Elapsed time advances only from LoadingMenu::AdvanceMovie intervals so Scaleform writes stay on
    // the UI/movie path rather than ProcessMessage, Present, or an external timer.
    float CellTransitioner::LoadingMenuFadeAlpha(float a_interval) noexcept
    {
        if (!hooksEnabled.load(std::memory_order_acquire) ||
            presentation.load(std::memory_order_acquire) != Presentation::loadingMenu) {
            return 1.0F;
        }

        const auto duration = Settings::GetSingleton().GetLoadingMenuFadeIn().count();
        if (duration <= 0) {
            return 1.0F;
        }

        const auto step = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::llround(std::max(0.0F, a_interval) * 1000.0F)));
        auto elapsed = loadingMenuFadeElapsedMs.load(std::memory_order_relaxed);
        while (true) {
            const auto next = std::min<std::int64_t>(duration, elapsed + step);
            if (loadingMenuFadeElapsedMs.compare_exchange_weak(
                    elapsed, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                elapsed = next;
                break;
            }
        }

        return std::clamp(static_cast<float>(elapsed) / static_cast<float>(duration), 0.0F, 1.0F);
    }

    // Writes Menu_mc alpha from LoadingMenu::AdvanceMovie so tips and the progress meter fade together.
    void CellTransitioner::ApplyLoadingMenuFade(RE::IMenu* a_menu, float a_interval) noexcept
    {
        if (!a_menu || !a_menu->uiMovie) {
            return;
        }

        try {
            const auto   alphaPercent = static_cast<double>(LoadingMenuFadeAlpha(a_interval)) * 100.0;
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

    // Compiles one of the small pixel shaders used by the loading compositor.
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

        const auto createResult =
            a_device->CreatePixelShader(buffer, bufferSize, nullptr, a_shader);
        bytecode->Release();

        return SUCCEEDED(createResult);
    }

    // Creates the shader that recovers alpha from Skyrim's opaque UI target.
    bool CellTransitioner::CreateLoadingOverlayShader(::ID3D11Device* a_device)
    {
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

    // Creates the shader used to blend to the captured frame's dominant color.
    bool CellTransitioner::CreateSolidColorShader(::ID3D11Device* a_device)
    {
        constexpr std::string_view source = R"(
float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    return color;
}
)";

        return CreatePixelShader(a_device, source, "solid color", &solidColorShader);
    }

    // Creates the shader used to fade retained RGB independently of the source render target's alpha.
    // Skyrim's world and image-space targets do not promise meaningful alpha; multiplying their alpha
    // by the fade color can therefore make the entire transition layer transparent.
    bool CellTransitioner::CreateTransitionFadeShader(::ID3D11Device* a_device)
    {
        constexpr std::string_view source = R"(
Texture2D transitionTexture : register(t0);
SamplerState transitionSampler : register(s0);

float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    float3 retained = transitionTexture.Sample(transitionSampler, textureCoordinate).rgb;
    return float4(retained * color.rgb, color.a);
}
)";

        return CreatePixelShader(a_device, source, "transition fade", &transitionFadeShader);
    }

    // Community Shaders' frame-generation compositor consumes a premultiplied SDR UI buffer after
    // it interpolates the world. Keeping the retained frame in that buffer makes its opacity identical
    // on real and generated frames. The captured ISHDR output is already gamma encoded when Linear
    // Lighting is disabled, which is the representation CS expects before UIBrightnessCS encodes it.
    bool CellTransitioner::CreateStableUITransitionShaders(::ID3D11Device* a_device)
    {
        std::string textureSource = R"(
Texture2D transitionTexture : register(t0);
SamplerState transitionSampler : register(s0);

float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    uint width;
    uint height;
    transitionTexture.GetDimensions(width, height);
    float2 texel = float2($BLUR_AMOUNT$ / width, $BLUR_AMOUNT$ / height);

    float3 retained;
#if USE_BLUR
    retained = transitionTexture.Sample(transitionSampler, textureCoordinate).rgb * 0.227027;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate + float2(1.384615, 0.0) * texel).rgb * 0.158108;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate - float2(1.384615, 0.0) * texel).rgb * 0.158108;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate + float2(0.0, 1.384615) * texel).rgb * 0.158108;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate - float2(0.0, 1.384615) * texel).rgb * 0.158108;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate + float2(3.230769, 3.230769) * texel).rgb * 0.035880;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate + float2(3.230769, -3.230769) * texel).rgb * 0.035880;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate + float2(-3.230769, 3.230769) * texel).rgb * 0.035880;
    retained += transitionTexture.Sample(transitionSampler, textureCoordinate - float2(3.230769, 3.230769) * texel).rgb * 0.035880;
#else
    retained = transitionTexture.Sample(transitionSampler, textureCoordinate).rgb;
#endif
    return float4(retained * color.a, color.a);
}
)";

        constexpr std::string_view token = "$BLUR_AMOUNT$";
        const auto amount = fmt::format("{:.3f}", Settings::GetSingleton().GetBlurAmount());
        for (auto position = textureSource.find(token); position != std::string::npos;
             position = textureSource.find(token)) {
            textureSource.replace(position, token.size(), amount);
        }
        textureSource = fmt::format("#define USE_BLUR {}\n{}",
            Settings::GetSingleton().IsBlurEnabled() ? 1 : 0, textureSource);

        constexpr std::string_view colorSource = R"(
float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    return float4(color.rgb * color.a, color.a);
}
)";

        return CreatePixelShader(a_device, textureSource, "stable UI transition", &stableUITransitionShader) &&
               CreatePixelShader(a_device, colorSource, "stable UI color", &stableUIColorShader);
    }

    // Returns a monotonic timestamp for transition timing.
    std::int64_t CellTransitioner::CurrentTimeMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // SleepWaitMenu opens before Skyrim queues the fade that blacks out the time-advance sequence.
    // Keep its persistent FaderMenu movie visible even if an earlier load left that movie hidden.
    void CellTransitioner::ObserveSleepWaitMenuOpening()
    {
        sleepFaderActive.store(true, std::memory_order_release);
    }

    // Marks the callback-free black recovery fade queued while SleepWaitMenu handles its close
    // message. The deadline is a fallback if the menu-open notification was not observed.
    void CellTransitioner::ObserveSleepWaitMenuClosing()
    {
        constexpr std::int64_t sleepFadeRequestWindow = 5000;
        const auto             deadline = CurrentTimeMilliseconds() + sleepFadeRequestWindow;
        sleepFadeRequestDeadline.store(deadline, std::memory_order_release);
    }

    // Creates the inexpensive single-pass blur used on the frozen world frame.
    bool CellTransitioner::CreateFrozenFrameBlurShader(::ID3D11Device* a_device)
    {
        std::string source = R"(
Texture2D frozenTexture : register(t0);
SamplerState frozenSampler : register(s0);

float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    uint width;
    uint height;
    frozenTexture.GetDimensions(width, height);
    float2 texel = float2($BLUR_AMOUNT$ / width, $BLUR_AMOUNT$ / height);

    float4 pixel = frozenTexture.Sample(frozenSampler, textureCoordinate) * 0.227027;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(1.384615, 0.0) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate - float2(1.384615, 0.0) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(0.0, 1.384615) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate - float2(0.0, 1.384615) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(3.230769, 3.230769) * texel) * 0.035880;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(3.230769, -3.230769) * texel) * 0.035880;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(-3.230769, 3.230769) * texel) * 0.035880;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate - float2(3.230769, 3.230769) * texel) * 0.035880;
    // World/image-space render targets do not guarantee meaningful alpha. Preserve their RGB,
    // but make the compositor's requested opacity authoritative for both opaque holds and fades.
    return float4(pixel.rgb * color.rgb, color.a);
}
)";

        constexpr std::string_view token = "$BLUR_AMOUNT$";
        const auto                 amount = fmt::format("{:.3f}", Settings::GetSingleton().GetBlurAmount());
        for (auto position = source.find(token); position != std::string::npos; position = source.find(token)) {
            source.replace(position, token.size(), amount);
        }

        return CreatePixelShader(a_device, source, "frozen frame blur", &frozenFrameBlurShader);
    }

    // Resolves only queued destinations that the active TES grid can safely expose.
    RE::TESObjectCELL* CellTransitioner::GetQueuedDestinationCell()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("queued destination resolver: player is unavailable; using a cold presentation");
            }
            return nullptr;
        }

        // Snapshot the record so every decision in this pass uses the same destination fields.
        const auto target = player->GetPlayerRuntimeData().queuedTargetLoc;
        if (!target.isValid) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("queued destination resolver: no valid queued target; using a cold presentation");
            }
            return nullptr;
        }
        if (target.interior) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("queued destination resolver: using the published interior cell");
            }
            return target.interior;
        }
        if (!target.world) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("queued destination resolver: exterior target has no world; using a cold presentation");
            }
            return nullptr;
        }

        auto* tes = RE::TES::GetSingleton();
        if (!tes || tes->GetRuntimeData2().worldSpace != target.world) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug(
                    "queued destination resolver: target world is not active; using a cold presentation");
            }
            return nullptr;
        }

        // Never traverse TESWorldSpace::cellMap here. Exterior CellLoaderTask work can insert,
        // remove, or rehash that container while scripted travel opens LoadingMenu. TES::GetCell
        // is not sufficient on every supported runtime because its grid miss falls back to that
        // worldspace map. Restrict the probe to the active GridCellArray instead.
        auto* grid = tes->gridCells;
        if (!grid || grid->length == 0) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("queued destination resolver: active grid is unavailable; using a cold presentation");
            }
            return nullptr;
        }

        const auto cellX = static_cast<std::int32_t>(std::floor(target.location.x / 4096.0F));
        const auto cellY = static_cast<std::int32_t>(std::floor(target.location.y / 4096.0F));
        const auto half = static_cast<std::int32_t>(grid->length >> 1);
        const auto gridX = cellX + half - tes->currentGridX;
        const auto gridY = cellY + half - tes->currentGridY;
        auto*      cell = gridX >= 0 && gridY >= 0 ?
                              grid->GetCell(
                             static_cast<std::uint32_t>(gridX), static_cast<std::uint32_t>(gridY)) :
                              nullptr;
        if (cell && !cell->IsAttached()) {
            cell = nullptr;
        }
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug(
                "queued destination resolver: exterior ({}, {}) active-grid lookup {}",
                cellX, cellY,
                cell ? "found an attached cell" : "found no attached cell; using a cold presentation");
        }
        return cell;
    }

    // Chooses the warm or cold presentation before the Loading Menu opens.
    CellTransitioner::Presentation CellTransitioner::ChoosePresentation()
    {
        mainMenuLoadActive.store(false, std::memory_order_release);
        fastTravelBlackActive.store(false, std::memory_order_release);

        if (newGameTransitionActive.load(std::memory_order_acquire)) {
            mainMenuLoadPending.store(false, std::memory_order_release);
            vanillaLoadPending.store(false, std::memory_order_release);
            // The loading compositor supplies opaque black beneath LoadingMenu. Once loading ends, Skyrim's
            // native FaderMenu takes over so TitleSequenceMenu retains its higher UI depth.
            transitionType.store(Settings::TransitionType::color, std::memory_order_release);
            colorSource.store(Settings::ColorSource::fixed, std::memory_order_release);
            transitionColor.store(0x000000, std::memory_order_release);
            fadeInDuration.store(0, std::memory_order_release);
            holdAfterLoad.store(0, std::memory_order_release);
            fadeOutDuration.store(0, std::memory_order_release);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("selected the opaque new-game loading presentation");
            }
            return Presentation::loadingMenu;
        }

        auto*      ui = RE::UI::GetSingleton();
        const bool fromMainMenu = mainMenuLoadPending.exchange(false, std::memory_order_acq_rel) ||
                                  (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
        const bool pendingVanilla =
            vanillaLoadPending.exchange(false, std::memory_order_acq_rel);
        const bool useVanilla = pendingVanilla ||
                                (fromMainMenu &&
                                    !Settings::GetSingleton().UseTransitionsForSaveLoads());
        if (useVanilla) {
            fastTravelBlackPending.store(false, std::memory_order_release);
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("selected Skyrim's vanilla loading presentation");
            }
            return Presentation::vanilla;
        }
        if (fastTravelBlackPending.load(std::memory_order_acquire)) {
            // MapMenu's 3D scene can be only partially rendered when its fast-travel fade closes it.
            // Continue the native black fade with an opaque compositor cover instead of exposing or
            // blurring that last captured map frame. Do not inspect the queued destination's worldspace
            // cell map here: the engine may still be materializing that map for scripted fast travel.
            const auto& cold = Settings::GetSingleton().GetColdTransition({});
            transitionType.store(Settings::TransitionType::color, std::memory_order_release);
            colorSource.store(Settings::ColorSource::fixed, std::memory_order_release);
            transitionColor.store(0x000000, std::memory_order_release);
            fadeInDuration.store(0, std::memory_order_release);
            holdAfterLoad.store(cold.holdAfterLoad.count(), std::memory_order_release);
            fadeOutDuration.store(cold.fadeOut.count(), std::memory_order_release);
            fastTravelBlackActive.store(true, std::memory_order_release);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug(
                    "selected fixed black fast-travel presentation: hold={}ms fadeOut={}ms",
                    holdAfterLoad.load(), fadeOutDuration.load());
            }
            // FaderMenu is the only black layer guaranteed to survive the upscaler's final
            // presentation path. Keep LoadingMenu active for its UI, suppress MistMenu separately,
            // and let Skyrim own the native black hold and destination fade-in.
            return Presentation::loadingMenu;
        }

        auto*                  cell = GetQueuedDestinationCell();
        const bool             resident = cell && cell->GetRuntimeData().loadedData;
        const auto*            editorIDText = cell ? cell->GetFormEditorID() : nullptr;
        const std::string_view editorID = editorIDText ? editorIDText : "";

        if (fromMainMenu) {
            // A menu movie is not a useful retained gameplay frame. Keep Skyrim's native fade to black,
            // then hold that same fixed black beneath LoadingMenu and fade it into the loaded save.
            const auto& cold = Settings::GetSingleton().GetColdTransition(editorID);
            mainMenuLoadActive.store(true, std::memory_order_release);
            transitionType.store(Settings::TransitionType::color, std::memory_order_release);
            colorSource.store(Settings::ColorSource::fixed, std::memory_order_release);
            transitionColor.store(0x000000, std::memory_order_release);
            fadeInDuration.store(0, std::memory_order_release);
            holdAfterLoad.store(cold.holdAfterLoad.count(), std::memory_order_release);
            fadeOutDuration.store(cold.fadeOut.count(), std::memory_order_release);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug(
                    "selected fixed black main-menu loading presentation: cell={:08X} editorID='{}' hold={}ms fadeOut={}ms",
                    cell ? cell->GetFormID() : 0, editorID, holdAfterLoad.load(), fadeOutDuration.load());
            }
            return Presentation::loadingMenu;
        }

        const auto selected = resident ? Presentation::seamless : Presentation::loadingMenu;
        if (selected == Presentation::seamless) {
            const auto& warm = Settings::GetSingleton().GetWarmTransition();
            transitionType.store(Settings::TransitionType::blur, std::memory_order_release);
            colorSource.store(Settings::ColorSource::fixed, std::memory_order_release);
            transitionColor.store(0xFFFFFF, std::memory_order_release);
            fadeInDuration.store(0, std::memory_order_release);
            holdAfterLoad.store(warm.holdAfterLoad.count(), std::memory_order_release);
            fadeOutDuration.store(warm.fadeOut.count(), std::memory_order_release);
        } else {
            const auto& cold = Settings::GetSingleton().GetColdTransition(editorID);
            transitionType.store(cold.type, std::memory_order_release);
            colorSource.store(cold.colorSource, std::memory_order_release);
            transitionColor.store(cold.color, std::memory_order_release);
            fadeInDuration.store(cold.fadeIn.count(), std::memory_order_release);
            holdAfterLoad.store(cold.holdAfterLoad.count(), std::memory_order_release);
            fadeOutDuration.store(cold.fadeOut.count(), std::memory_order_release);
        }

        const auto type = transitionType.load(std::memory_order_acquire);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug(
                "loading destination: cell={:08X} editorID='{}' loadedData={} attached={} presentation={} transition={} "
                "fadeIn={}ms hold={}ms fadeOut={}ms",
                cell ? cell->GetFormID() : 0, editorID, resident,
                cell && cell->IsAttached(), selected == Presentation::seamless ? "seamless" : "loading-menu",
                type == Settings::TransitionType::color ? "color" : "blur", fadeInDuration.load(),
                holdAfterLoad.load(), fadeOutDuration.load());

            if (type == Settings::TransitionType::color) {
                logger::debug("color transition: source={} fallback=#{:06X}",
                    colorSource.load(std::memory_order_acquire) == Settings::ColorSource::dominant ?
                        "dominant" :
                        "fixed",
                    transitionColor.load(std::memory_order_acquire));
            }
        }
        return selected;
    }
    // Collects the input and menu state used by post-load diagnostics.
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
            ui->IsMenuOpen(RE::FaderMenu::MENU_NAME), ui->IsMenuOpen(RE::MistMenu::MENU_NAME) };
    }

    // Logs each post-load input-state change until gameplay controls return.
    void CellTransitioner::ObserveControlRestore()
    {
        if (!awaitingControlRestore.load(std::memory_order_acquire)) {
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
                logger::debug(
                    "post-load controls: enabled={:08X} stored={:08X} blockInput={} paused={} fader={} mist={}",
                    state->enabled, state->stored, state->blockInput, state->paused, state->faderOpen,
                    state->mistOpen);
                lastControlState = *state;
            }
        }

        auto* controls = RE::ControlMap::GetSingleton();
        if (!controls) {
            return;
        }

        const bool gameplayReady = controls->IsMovementControlsEnabled() && controls->IsLookingControlsEnabled() &&
                                   controls->IsActivateControlsEnabled() && !state->blockInput && !state->paused;
        if (gameplayReady) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("post-load gameplay controls are ready");
            }
            awaitingControlRestore.store(false, std::memory_order_release);
        }
    }

    // Checks whether the cached textures still match the active render target.
    bool CellTransitioner::MatchesFrozenFrame(const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        return frozenFrame && frozenFrameDesc.width == a_desc.width && frozenFrameDesc.height == a_desc.height &&
               frozenFrameDesc.format == a_desc.format && frozenFrameDesc.sampleDesc.count == a_desc.sampleDesc.count &&
               frozenFrameDesc.sampleDesc.quality == a_desc.sampleDesc.quality;
    }

    // Releases textures that must be recreated when the render target changes.
    void CellTransitioner::ReleaseFrameResources()
    {
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

        if (stableUIOverlayView) {
            stableUIOverlayView->Release();
            stableUIOverlayView = nullptr;
        }

        if (stableUIOverlay) {
            stableUIOverlay->Release();
            stableUIOverlay = nullptr;
        }
        stableUIOverlayDesc = {};
    }

    // Releases only the immutable handoff surface. It deliberately has a lifetime independent of
    // the rolling world captures and renderer-owned targets above.
    void CellTransitioner::ReleasePersistentTransitionFrame()
    {
        if (persistentTransitionTarget) {
            persistentTransitionTarget->Release();
            persistentTransitionTarget = nullptr;
        }
        if (persistentTransitionView) {
            persistentTransitionView->Release();
            persistentTransitionView = nullptr;
        }
        if (persistentTransitionFrame) {
            persistentTransitionFrame->Release();
            persistentTransitionFrame = nullptr;
        }
        persistentTransitionDesc = {};
    }

    // Fails closed without disabling unrelated progress instrumentation. FaderMenu's UI-thread hook
    // sees this state and restores the native opaque layer; the current render pass also draws black.
    void CellTransitioner::EnterFaderFallback(std::string_view a_reason) noexcept
    {
        transitionState.store(TransitionState::faderFallback, std::memory_order_release);
        fallbackFaderActiveSeen.store(false, std::memory_order_release);
        // Keep the emergency black compositor armed until the UI thread has restored FaderMenu.
        // This avoids exposing one frame if the failure occurs between movie advances.
        postLoadFadePending.store(true, std::memory_order_release);
        postLoadFadeStart.store(0, std::memory_order_release);
        frozenFrameLocked.store(true, std::memory_order_release);
        logger::error("transition presentation fell back to FaderMenu: {}", a_reason);
    }

    // The observed first-person pulse occurs only after the Fader bridge is released. Camera-space
    // stability is not a valid release condition because intentional movement and ordinary head bob
    // change the same transforms. Keep the retained frame opaque for the measured pulse interval and
    // require completed destination renders, neither of which can be extended by player input.
    void CellTransitioner::ObservePostHandoffCameraQualification() noexcept
    {
        if (!firstPersonPostHandoffQualifying.load(std::memory_order_acquire)) {
            return;
        }

        const auto now = CurrentTimeMilliseconds();
        const auto qualificationStart =
            firstPersonPostHandoffQualificationStart.load(std::memory_order_acquire);
        const auto elapsed = qualificationStart > 0 ? now - qualificationStart : 0;

        try {
            const auto* playerCamera = RE::PlayerCamera::GetSingleton();
            if (!playerCamera || !playerCamera->IsInFirstPerson()) {
                const auto delay = std::max<std::int64_t>(holdAfterLoad.load(std::memory_order_acquire), 0);
                postLoadFadeStart.store(now - delay, std::memory_order_release);
                firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
                return;
            }

            const auto renderedFrames =
                firstPersonPostHandoffRenderedFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
            const auto configuredHold =
                std::max<std::int64_t>(holdAfterLoad.load(std::memory_order_acquire), 0);
            const auto minimumObservation =
                std::max(firstPersonPostHandoffMinimumObservation, configuredHold);
            const bool observedLongEnough = elapsed >= minimumObservation;
            if (!observedLongEnough || renderedFrames < firstPersonPostHandoffMinimumFrames) {
                return;
            }

            logger::info(
                "post-handoff first-person cover completed after {}ms and {} rendered frames",
                elapsed, renderedFrames);

            // The configured opaque hold has already elapsed inside this longer qualification.
            // Backdate the ordinary fade clock by that hold so the crossfade starts on the next
            // compositor pass instead of creating a second, unobserved waiting interval.
            postLoadFadeStart.store(now - configuredHold, std::memory_order_release);
            firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
        } catch (...) {
            const auto delay = std::max<std::int64_t>(holdAfterLoad.load(std::memory_order_acquire), 0);
            postLoadFadeStart.store(now - delay, std::memory_order_release);
            firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
            logger::warn("post-handoff first-person cover observation failed; beginning the visible fade");
        }
    }

    // Starts the compositor side of a gapless handoff. Both owners sample the same SRV, and the first
    // fade frame has alpha 1, so changing ownership cannot expose a frame of the destination.
    bool CellTransitioner::StartPostLoadFade(bool a_presentFallback)
    {
        if (faderBridgeActive.load(std::memory_order_acquire) &&
            faderBridgeDisplaySerial.load(std::memory_order_acquire) == 0) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug(
                    "holding post-load handoff until opaque FaderMenu completes a UI display pass");
            }
            return false;
        }

        auto expected = TransitionState::waitingForStableRenderer;
        if (!transitionState.compare_exchange_strong(
                expected, TransitionState::compositorHandoff, std::memory_order_acq_rel)) {
            return false;
        }

        // Once the verified image-space path has resumed, keep HDR captures in that path so Community
        // Shaders performs its normal output transform. Sampling an R16G16B16A16 scene texture
        // directly into the final R10 Present surface produces the flat gray transition seen after a
        // save load. Present remains only the timeout/watchdog owner when image-space did not resume.
        const bool presentOwner = a_presentFallback;

        bool qualifyFirstPerson = false;
        try {
            const auto* playerCamera = RE::PlayerCamera::GetSingleton();
            qualifyFirstPerson = playerCamera && playerCamera->IsInFirstPerson();
        } catch (...) {
        }

        const auto handoffTime = CurrentTimeMilliseconds();
        firstPersonPostHandoffQualificationStart.store(
            qualifyFirstPerson ? handoffTime : 0, std::memory_order_release);
        firstPersonPostHandoffRenderedFrames.store(0, std::memory_order_release);
        firstPersonPostHandoffQualifying.store(qualifyFirstPerson, std::memory_order_release);

        postLoadFadePending.store(false, std::memory_order_release);
        postLoadFadeRequestedAt.store(0, std::memory_order_release);
        postLoadPresentFallback.store(presentOwner, std::memory_order_release);
        postLoadFadeStart.store(handoffTime, std::memory_order_release);
        transitionState.store(TransitionState::fadingToLive, std::memory_order_release);
        // The current compositor pass draws the same persistent image fully opaque. Release the
        // native bridge asynchronously; its zero-duration fade-in reveals this pass without a gap.
        if (faderBridgeActive.load(std::memory_order_acquire)) {
            QueueFaderBridge(false);
        }
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug("destination renderer stable; handed persistent transition texture to {} compositor",
                presentOwner ? "Present" : "post-processing");
        }
        return true;
    }

    // Allocates the frozen frame, CPU readback, and Scaleform overlay textures.
    bool CellTransitioner::PrepareFrozenFrame(
        REX::W32::ID3D11Device* a_device, const REX::W32::D3D11_TEXTURE2D_DESC& a_backBufferDesc)
    {
        if (!a_device || a_backBufferDesc.width == 0 || a_backBufferDesc.height == 0) {
            return false;
        }

        if (MatchesFrozenFrame(a_backBufferDesc)) {
            return true;
        }

        ReleaseFrameResources();

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
        loggedFrozenFrame = false;

        return true;
    }

    // Allocates a byte-for-byte snapshot of the renderer's current stable UI target. This resource
    // is deliberately separate from the HDR frozen-frame textures because CS exposes its UI layer as
    // R8G8B8A8_UNORM even when the scene and swap chain use float16/HDR10 formats.
    bool CellTransitioner::PrepareStableUIOverlay(
        REX::W32::ID3D11Device* a_device, const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        if (!a_device || a_desc.width == 0 || a_desc.height == 0 || a_desc.sampleDesc.count != 1) {
            return false;
        }
        if (stableUIOverlay && stableUIOverlayView &&
            stableUIOverlayDesc.width == a_desc.width &&
            stableUIOverlayDesc.height == a_desc.height &&
            stableUIOverlayDesc.format == a_desc.format) {
            return true;
        }

        if (stableUIOverlayView) {
            stableUIOverlayView->Release();
            stableUIOverlayView = nullptr;
        }
        if (stableUIOverlay) {
            stableUIOverlay->Release();
            stableUIOverlay = nullptr;
        }
        stableUIOverlayDesc = {};

        auto desc = a_desc;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = 0;
        desc.miscFlags = 0;
        if (a_device->CreateTexture2D(&desc, nullptr, &stableUIOverlay) < 0 ||
            a_device->CreateShaderResourceView(
                stableUIOverlay, nullptr, &stableUIOverlayView) < 0) {
            if (stableUIOverlayView) {
                stableUIOverlayView->Release();
                stableUIOverlayView = nullptr;
            }
            if (stableUIOverlay) {
                stableUIOverlay->Release();
                stableUIOverlay = nullptr;
            }
            logger::error("could not allocate the stable UI overlay texture");
            return false;
        }
        stableUIOverlayDesc = a_desc;
        return true;
    }

    // Returns true when the captured texture uses a supported BGRA byte layout.
    bool CellTransitioner::IsBgraFormat(REX::W32::DXGI_FORMAT a_format)
    {
        return a_format == REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM ||
               a_format == REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    // Returns true when the captured texture uses a supported RGBA byte layout.
    bool CellTransitioner::IsRgbaFormat(REX::W32::DXGI_FORMAT a_format)
    {
        return a_format == REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM ||
               a_format == REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

    // Builds a reduced RGB histogram from every fourth captured pixel.
    std::array<std::uint32_t, 4096> CellTransitioner::BuildColorHistogram(
        const REX::W32::D3D11_MAPPED_SUBRESOURCE& a_mapped,
        REX::W32::DXGI_FORMAT                     a_format)
    {
        std::array<std::uint32_t, 4096> histogram{};
        if (!a_mapped.data || a_mapped.rowPitch / 4 < frozenFrameDesc.width) {
            return histogram;
        }

        const bool bgra = IsBgraFormat(a_format);
        const bool rgba = IsRgbaFormat(a_format);
        const bool rgb10 = a_format == REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM;
        if (!bgra && !rgba && !rgb10) {
            return histogram;
        }

        constexpr std::uint32_t sampleStep = 4;

        for (std::uint32_t y = 0; y < frozenFrameDesc.height; y += sampleStep) {
            const auto* row = static_cast<const std::uint8_t*>(a_mapped.data) + y * a_mapped.rowPitch;

            for (std::uint32_t x = 0; x < frozenFrameDesc.width; x += sampleStep) {
                const auto*   pixel = row + x * 4;
                std::uint32_t red = 0;
                std::uint32_t green = 0;
                std::uint32_t blue = 0;
                std::uint32_t alpha = 0;

                if (rgb10) {
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

                const auto bin = static_cast<std::size_t>((red >> 4) << 8 | (green >> 4) << 4 | (blue >> 4));
                ++histogram[bin];
            }
        }

        return histogram;
    }

    // Converts the most populated histogram bin back into a packed RGB color.
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

    // Reads the locked source frame and selects the color used by dominant-color transitions.
    void CellTransitioner::UpdateTransitionColor(REX::W32::ID3D11DeviceContext* a_context)
    {
        if (!a_context || !frozenFrame || !dominantColorReadback) {
            logger::warn("using the default transition color because no captured frame is readable");
            return;
        }

        const bool supported = IsBgraFormat(frozenFrameDesc.format) ||
                               IsRgbaFormat(frozenFrameDesc.format) ||
                               frozenFrameDesc.format == REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM;
        if (!supported) {
            logger::warn("using the default transition color for unsupported texture format {}",
                std::to_underlying(frozenFrameDesc.format));
            return;
        }

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

        transitionColor.store(*color, std::memory_order_release);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug("selected captured-frame transition color #{:06X}", transitionColor.load());
        }
    }

    // Converts the packed transition color and caller-supplied alpha for SpriteBatch.
    DirectX::XMVECTOR CellTransitioner::TransitionColor(float a_alpha)
    {
        const auto color = transitionColor.load(std::memory_order_acquire);
        return DirectX::XMVectorSet(static_cast<float>((color >> 16) & 0xFF) / 255.0F,
            static_cast<float>((color >> 8) & 0xFF) / 255.0F,
            static_cast<float>(color & 0xFF) / 255.0F, a_alpha);
    }

    // Selects the configured blur shader or SpriteBatch's unmodified texture shader.
    ::ID3D11PixelShader* CellTransitioner::GetFrozenFrameShader()
    {
        return Settings::GetSingleton().IsBlurEnabled() ? frozenFrameBlurShader : nullptr;
    }

    // Returns a full-screen destination rectangle for the current back buffer.
    RECT CellTransitioner::GetDestinationRect(const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        return { 0, 0, static_cast<LONG>(a_desc.width), static_cast<LONG>(a_desc.height) };
    }

    // Draws one full-screen compositor layer with an explicit pixel shader.
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

        if (a_shader) {
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, a_blendState, a_samplerState, nullptr, nullptr,
                [context, a_shader] { context->PSSetShader(a_shader, nullptr, 0); });
        } else {
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, a_blendState, a_samplerState);
        }
        spriteBatch->Draw(texture, a_destination, a_color);
        spriteBatch->End();
    }

    // Prepares the visual transition exactly once, before volatile world targets are allowed to
    // change. The resulting texture is thereafter read-only and shared by the hold and fade paths.
    bool CellTransitioner::PreparePersistentTransitionFrame(
        REX::W32::ID3D11Device* a_device, REX::W32::ID3D11DeviceContext* a_context,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_targetDesc)
    {
        if (persistentTransitionFrame && persistentTransitionView && persistentTransitionTarget) {
            return true;
        }

        const bool immediateFixedColor =
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color &&
            colorSource.load(std::memory_order_acquire) == Settings::ColorSource::fixed &&
            fadeInDuration.load(std::memory_order_acquire) <= 0;
        if (!a_device || !a_context || !commonStates ||
            (!immediateFixedColor &&
                (!frozenFrameView || frozenFrameDesc.width == 0 || frozenFrameDesc.height == 0 ||
                    frozenFrameDesc.sampleDesc.count != 1))) {
            return false;
        }

        ReleasePersistentTransitionFrame();
        auto desc = immediateFixedColor ? a_targetDesc : frozenFrameDesc;
        if (desc.width == 0 || desc.height == 0) {
            return false;
        }
        // This resource is sampled as Texture2D. Fixed colors have no source detail that requires
        // preserving a multisampled target, so make the persistent surface single-sampled.
        desc.sampleDesc.count = 1;
        desc.sampleDesc.quality = 0;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE | REX::W32::D3D11_BIND_RENDER_TARGET;
        desc.cpuAccessFlags = 0;
        desc.miscFlags = 0;
        if (a_device->CreateTexture2D(&desc, nullptr, &persistentTransitionFrame) < 0 ||
            a_device->CreateShaderResourceView(
                persistentTransitionFrame, nullptr, &persistentTransitionView) < 0 ||
            a_device->CreateRenderTargetView(
                persistentTransitionFrame, nullptr, &persistentTransitionTarget) < 0) {
            ReleasePersistentTransitionFrame();
            return false;
        }
        persistentTransitionDesc = desc;

        if (immediateFixedColor) {
            const auto color = transitionColor.load(std::memory_order_acquire);
            const std::array clearColor{
                static_cast<float>((color >> 16) & 0xFF) / 255.0F,
                static_cast<float>((color >> 8) & 0xFF) / 255.0F,
                static_cast<float>(color & 0xFF) / 255.0F,
                1.0F
            };
            a_context->ClearRenderTargetView(persistentTransitionTarget, clearColor.data());
        } else {
            // Preserve the known-good pre-load capture byte-for-byte. The previous implementation
            // sampled frozenFrame into this render target, which introduced a second color/alpha
            // conversion and could leave the persistent RGB black. Applying the established blur
            // shader when this immutable copy is presented retains the old visual path while keeping
            // ownership independent of Skyrim's volatile render targets.
            a_context->CopyResource(persistentTransitionFrame, frozenFrame);

        }

        transitionState.store(TransitionState::uiHold, std::memory_order_release);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug("prepared immutable {}x{} transition texture for transition ownership",
                desc.width, desc.height);
        }
        return true;
    }

    // Replaces the loading frame with the captured pre-load world image and configured blur.
    void CellTransitioner::PresentSeamlessFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        if (!a_context || !a_backBuffer || !commonStates) {
            return;
        }

        // Every presentation path samples the same immutable pre-Scaleform texture. It is independent
        // of Skyrim's volatile scene, lighting, and swap-chain targets.
        auto* sourceView = persistentTransitionView;
        if (!sourceView) {
            return;
        }
        DrawFullscreenLayer(a_context, sourceView, GetDestinationRect(a_desc),
            commonStates->Opaque(), commonStates->LinearClamp(),
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::blur ?
                GetFrozenFrameShader() : nullptr);

        if (!loggedFrozenPresentation) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("presenting the blurred frozen pre-load frame");
            }
            loggedFrozenPresentation = true;
        }
    }

    // Draws the blurred source frame and then restores Scaleform above it.
    void CellTransitioner::PresentLoadingMenuFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc,
        bool                                  a_separateUI)
    {
        auto*      sourceView = persistentTransitionView;
        const bool opaqueFixedColor =
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color &&
            colorSource.load(std::memory_order_acquire) == Settings::ColorSource::fixed &&
            fadeInDuration.load(std::memory_order_acquire) <= 0;
        if (!a_context || !a_backBuffer || !commonStates || !sourceView ||
            (!a_separateUI && (!loadingOverlay || !loadingOverlayView))) {
            return;
        }

        if (!a_separateUI) {
            // Vanilla renders Scaleform into the scene target. Save it so its widgets can be restored.
            a_context->CopyResource(loadingOverlay, a_backBuffer);
        }

        const auto destination = GetDestinationRect(a_desc);
        if (mainMenuLoadActive.load(std::memory_order_acquire) || opaqueFixedColor) {
            // Immediate fixed-color transitions do not sample the retained texture. This also lets
            // them cover an upscaler's internal loading target when its format differs from the
            // post-processed frame captured before the load.
            DrawFullscreenLayer(a_context, sourceView, destination, commonStates->Opaque(), nullptr,
                solidColorShader, TransitionColor(1.0F));
        } else {
            DrawFullscreenLayer(a_context, sourceView, destination, commonStates->Opaque(),
                commonStates->LinearClamp(),
                transitionType.load(std::memory_order_acquire) == Settings::TransitionType::blur ?
                    GetFrozenFrameShader() : nullptr);
        }

        if (!mainMenuLoadActive.load(std::memory_order_acquire) && !opaqueFixedColor &&
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color) {
            const auto now = CurrentTimeMilliseconds();
            auto       start = loadingTransitionStart.load(std::memory_order_acquire);
            if (start == 0) {
                if (loadingTransitionStart.compare_exchange_strong(
                        start, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    start = now;

                    if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                        logger::debug("started visible color fade-in");
                    }
                }
            }

            const auto elapsed = std::max<std::int64_t>(now - start, 0);
            const auto duration = fadeInDuration.load(std::memory_order_acquire);
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

    // Draws the retained frame over the new cell until the post-load crossfade ends.
    void CellTransitioner::PresentPostLoadFrame(
        REX::W32::ID3D11DeviceContext* a_context,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc, bool a_stableUI)
    {
        if (!a_context || !commonStates) {
            return;
        }

        // Load the qualification gate first. Its release store publishes the corrected fade clock
        // when qualification completes, so the subsequent start load cannot observe the old clock.
        const bool cameraQualifying =
            firstPersonPostHandoffQualifying.load(std::memory_order_acquire);
        const auto fadeStart = postLoadFadeStart.load(std::memory_order_acquire);
        const bool fadePending = postLoadFadePending.load(std::memory_order_acquire);
        if (fadeStart <= 0 && !fadePending) {
            return;
        }

        // While the ordinary image-space compositor has not resumed yet, keep the retained frame opaque.
        // First person also remains covered while its final post-handoff render camera is qualified.
        const auto delay = holdAfterLoad.load(std::memory_order_acquire);
        const auto fadeElapsed =
            (fadePending || cameraQualifying) ? 0 : CurrentTimeMilliseconds() - fadeStart - delay;
        const auto duration = fadeOutDuration.load(std::memory_order_acquire);

        if (!fadePending && !cameraQualifying && fadeElapsed >= duration) {
            postLoadFadeStart.store(0, std::memory_order_release);
            postLoadFadePending.store(false, std::memory_order_release);
            postLoadFadeRequestedAt.store(0, std::memory_order_release);
            postLoadPresentFallback.store(false, std::memory_order_release);
            firstPersonPostHandoffQualifying.store(false, std::memory_order_release);
            firstPersonPostHandoffQualificationStart.store(0, std::memory_order_release);
            firstPersonPostHandoffRenderedFrames.store(0, std::memory_order_release);
            frozenFrameLocked.store(false, std::memory_order_release);
            mainMenuLoadActive.store(false, std::memory_order_release);
            transitionState.store(TransitionState::rollingCapture, std::memory_order_release);
            ReleasePersistentTransitionFrame();
            // HUD movie writes belong on the UI thread, not in Present.
            QueueHUDVisibilitySync();
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("post-load frozen-frame crossfade completed");
            }
            return;
        }

        if (!persistentTransitionView) {
            EnterFaderFallback("persistent transition texture disappeared during the post-load fade");
            return;
        }

        const auto fadeProgress = duration > 0 ?
                                      std::clamp(static_cast<float>(fadeElapsed) / static_cast<float>(duration), 0.0F, 1.0F) :
                                      1.0F;
        const auto alpha = 1.0F - fadeProgress;
        const auto usesColor =
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color;
        const auto color = usesColor ?
                               TransitionColor(alpha) :
                               DirectX::XMVectorSet(1.0F, 1.0F, 1.0F, alpha);

        // CS consumes its separate UI texture as premultiplied SDR. Write that representation
        // directly with opaque GPU state so the alpha channel remains exactly the fade alpha.
        // The ordinary image-space path retains its established straight-alpha blend.
        DrawFullscreenLayer(a_context, persistentTransitionView, GetDestinationRect(a_desc),
            a_stableUI ? commonStates->Opaque() : commonStates->NonPremultiplied(),
            commonStates->LinearClamp(),
            a_stableUI ? (usesColor ? stableUIColorShader : stableUITransitionShader) :
                         (usesColor ? solidColorShader :
                                      (GetFrozenFrameShader() ? GetFrozenFrameShader() : transitionFadeShader)),
            color);
    }

    // Places the post-load fade in the renderer's stable UI target when one is exposed. Community
    // Shaders passes this same target to FidelityFX, so both real and generated frames receive the
    // identical retained layer. Existing Scaleform/HUD pixels are restored above the transition.
    bool CellTransitioner::CompositePostLoadStableUI(
        REX::W32::ID3D11Device* a_device, REX::W32::ID3D11DeviceContext* a_context)
    {
        if (!a_device || !a_context || !commonStates || !persistentTransitionView ||
            !stableUITransitionShader || !stableUIColorShader) {
            stableUICompositeAvailable.store(false, std::memory_order_release);
            return false;
        }

        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) {
            stableUICompositeAvailable.store(false, std::memory_order_release);
            return false;
        }
        auto& framebuffer =
            renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
        auto* uiTargetView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(framebuffer.RTV);
        if (!uiTargetView) {
            stableUICompositeAvailable.store(false, std::memory_order_release);
            return false;
        }

        REX::W32::ComPtr<REX::W32::ID3D11Resource> uiResource;
        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> uiTexture;
        uiTargetView->GetResource(uiResource.GetAddressOf());
        if (!uiResource.Get() || uiResource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                     reinterpret_cast<void**>(uiTexture.GetAddressOf())) < 0 ||
            !uiTexture.Get()) {
            stableUICompositeAvailable.store(false, std::memory_order_release);
            return false;
        }

        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        uiTexture->GetDesc(&desc);
        const auto& window = renderer->GetRuntimeData().renderWindows[0];
        const bool isStableUI =
            desc.width == static_cast<std::uint32_t>(window.windowWidth) &&
            desc.height == static_cast<std::uint32_t>(window.windowHeight) &&
            desc.format == REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc.sampleDesc.count == 1 &&
            (desc.miscFlags & REX::W32::D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0;
        const bool previouslyAvailable =
            stableUICompositeAvailable.exchange(isStableUI, std::memory_order_acq_rel);
        if (isStableUI && !previouslyAvailable &&
            Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug(
                "using shared {}x{} UI target for frame-generation-stable post-load fade",
                desc.width, desc.height);
        }
        if (!isStableUI || !PrepareStableUIOverlay(a_device, desc)) {
            return false;
        }

        REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> previousView;
        REX::W32::ComPtr<REX::W32::ID3D11DepthStencilView> previousDepth;
        a_context->OMGetRenderTargets(1, previousView.GetAddressOf(), previousDepth.GetAddressOf());
        std::array<REX::W32::D3D11_VIEWPORT,
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                  previousViewports{};
        auto viewportCount = static_cast<std::uint32_t>(previousViewports.size());
        a_context->RSGetViewports(&viewportCount, previousViewports.data());

        // Preserve the already-rendered menus, establish a transparent transition layer beneath
        // them, then restore their premultiplied pixels above it.
        a_context->CopyResource(stableUIOverlay, uiTexture.Get());
        constexpr std::array<float, 4> transparent{};
        a_context->ClearRenderTargetView(uiTargetView, transparent.data());
        auto* target = uiTargetView;
        a_context->OMSetRenderTargets(1, &target, nullptr);
        const REX::W32::D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(desc.width), static_cast<float>(desc.height), 0.0F, 1.0F
        };
        a_context->RSSetViewports(1, &viewport);
        PresentPostLoadFrame(a_context, desc, true);
        DrawFullscreenLayer(a_context, stableUIOverlayView, GetDestinationRect(desc),
            commonStates->AlphaBlend(), commonStates->LinearClamp(), nullptr);

        auto* previousTarget = previousView.Get();
        a_context->OMSetRenderTargets(previousTarget ? 1U : 0U,
            previousTarget ? &previousTarget : nullptr, previousDepth.Get());
        if (viewportCount > 0) {
            a_context->RSSetViewports(viewportCount, previousViewports.data());
        }
        return true;
    }

    // Selects the compositor path for the current loading state.
    void CellTransitioner::CompositeLoadingFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc,
        bool                                  a_separateUI)
    {
        if (!a_context || !a_backBuffer || !spriteBatch || !commonStates) {
            return;
        }

        // epochActive is the main loading gate. postLoadFadeStart handles the short tail after it closes.
        const bool loading = preLoadDoorTransitionActive.load(std::memory_order_acquire) ||
                             epochActive.load(std::memory_order_acquire);

        if (loading && transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color &&
            dominantColorPending.exchange(false, std::memory_order_acq_rel)) {
            UpdateTransitionColor(a_context);
        }

        auto state = transitionState.load(std::memory_order_acquire);
        if (loading && state == TransitionState::preparing) {
            auto* device = RE::BSGraphics::Renderer::GetDevice();
            if (!PreparePersistentTransitionFrame(device, a_context, a_desc)) {
                EnterFaderFallback("could not prepare the persistent transition texture");
                if (frozenFrameView && solidColorShader) {
                    DrawFullscreenLayer(a_context, frozenFrameView, GetDestinationRect(a_desc),
                        commonStates->Opaque(), nullptr, solidColorShader,
                        TransitionColor(1.0F));
                }
                return;
            }
            state = TransitionState::uiHold;
        }

        if (state == TransitionState::faderFallback) {
            if (frozenFrameView && solidColorShader) {
                DrawFullscreenLayer(a_context, frozenFrameView, GetDestinationRect(a_desc),
                    commonStates->Opaque(), nullptr, solidColorShader, TransitionColor(1.0F));
            }
            return;
        }

        // EndLoad publishes the successor before releasing epochActive. Accept both adjacent states
        // on either side so a render callback racing the crossover remains fail-closed.
        const bool validLoadingState = state == TransitionState::uiHold ||
                                       state == TransitionState::waitingForStableRenderer;
        const bool validPostLoadState = state == TransitionState::uiHold ||
                                        state == TransitionState::waitingForStableRenderer ||
                                        state == TransitionState::compositorHandoff ||
                                        state == TransitionState::fadingToLive;
        if ((loading && !validLoadingState) || (!loading && !validPostLoadState)) {
            EnterFaderFallback("invalid transition state reached the compositor");
            return;
        }

        if (!loading) {
            PresentPostLoadFrame(a_context, a_desc);
            return;
        }

        if (presentation.load(std::memory_order_acquire) == Presentation::seamless) {
            PresentSeamlessFrame(a_context, a_backBuffer, a_desc);
            return;
        }

        PresentLoadingMenuFrame(a_context, a_backBuffer, a_desc, a_separateUI);
    }

    // Hooks IDXGISwapChain::Present to composite the retained loading frame.
    REX::W32::HRESULT CellTransitioner::PresentFrozenFrame(
        REX::W32::IDXGISwapChain* a_swapChain, std::uint32_t a_syncInterval, std::uint32_t a_flags)
    {
        // Win32 E_POINTER is the only safe result when the hooked COM receiver is unavailable.
        constexpr auto nullPointerResult = static_cast<REX::W32::HRESULT>(0x80004003U);
        if (!a_swapChain || !originalPresent) {
            return nullPointerResult;
        }

        // Community Shaders' D3D12 proxy exposes a shared D3D11 interop texture from GetBuffer(0).
        // Its Present chain fills that texture in ApplyHDR immediately before copying it to the real
        // D3D12 swap chain, so an outer hook must sample it after the downstream Present returns. A
        // native swap-chain buffer has the opposite lifetime and must still be sampled before Present.
        bool captureAfterDownstreamPresent = false;
        const auto captureRollingSwapChainFrame = [&](bool a_afterDownstreamPresent) {
            REX::W32::ComPtr<REX::W32::ID3D11Texture2D> displayedFrame;
            if (a_swapChain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D,
                    reinterpret_cast<void**>(displayedFrame.GetAddressOf())) < 0 ||
                !displayedFrame.Get()) {
                return false;
            }

            REX::W32::D3D11_TEXTURE2D_DESC displayedDesc{};
            displayedFrame->GetDesc(&displayedDesc);
            const bool sharedInteropBuffer =
                (displayedDesc.miscFlags & REX::W32::D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0;
            if (sharedInteropBuffer != a_afterDownstreamPresent ||
                frozenFrameLocked.load(std::memory_order_acquire)) {
                return sharedInteropBuffer;
            }

            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            auto* device = RE::BSGraphics::Renderer::GetDevice();
            auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
            if (device && context && PrepareFrozenFrame(device, displayedDesc)) {
                context->CopyResource(frozenFrame, displayedFrame.Get());
                loggedFrozenPresentation = false;
                if (!loggedFrozenFrame && Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::debug(
                        "capturing rolling {}x{} frames from swap-chain buffer 0 {} downstream Present",
                        displayedDesc.width, displayedDesc.height,
                        a_afterDownstreamPresent ? "after" : "before");
                    loggedFrozenFrame = true;
                }
            }
            return sharedInteropBuffer;
        };

        if (hooksEnabled.load(std::memory_order_acquire)) {
            try {
                ObserveControlRestore();

                // Preserve the last fully composed frame and never let an earlier, unverified render
                // stage overwrite it. Shared interop buffers are deliberately deferred until CS has
                // populated them; ordinary DXGI back buffers are captured at the normal pre-Present
                // boundary.
                if (!compositeAfterPostProcessing &&
                    !frozenFrameLocked.load(std::memory_order_acquire)) {
                    captureAfterDownstreamPresent = captureRollingSwapChainFrame(false);
                }

                // The image-space call is not guaranteed to run for every frame Skyrim presents,
                // particularly while menu/loading render paths are changing. Treat it as the preferred
                // completed-render path, but retain this final compositor as a watchdog for missed frames.
                const bool loading = preLoadDoorTransitionActive.load(std::memory_order_acquire) ||
                                     epochActive.load(std::memory_order_acquire);
                bool fadePending = postLoadFadePending.load(std::memory_order_acquire);
                bool transitionActive = loading || fadePending ||
                                        postLoadFadeStart.load(std::memory_order_acquire) > 0;
                const auto postProcessingPasses = compositeAfterPostProcessing ?
                                                      postProcessingPassesSincePresent.exchange(
                                                          0, std::memory_order_acq_rel) :
                                                      0;
                const bool postProcessingComposited = postProcessingPasses > 0;
                bool       waitingForPostProcessing =
                    compositeAfterPostProcessing && !loading && fadePending;
                if (waitingForPostProcessing) {
                    constexpr std::int64_t postProcessingResumeTimeout = 2000;
                    const auto             requestedAt = postLoadFadeRequestedAt.load(std::memory_order_acquire);
                    if (!postProcessingComposited &&
                        destinationWorldFrames.load(std::memory_order_acquire) >= 2) {
                        stablePipelineFrames.fetch_add(1, std::memory_order_acq_rel);
                    }
                    if (requestedAt > 0 &&
                        CurrentTimeMilliseconds() - requestedAt >= postProcessingResumeTimeout &&
                        stablePipelineFrames.load(std::memory_order_acquire) >= 2 &&
                        StartPostLoadFade(true)) {
                        waitingForPostProcessing = false;
                        fadePending = false;
                        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                            logger::warn(
                                "image-space compositor did not resume within {}ms; using Present for the fade",
                                postProcessingResumeTimeout);
                        }
                    }
                } else if (!compositeAfterPostProcessing && !loading && fadePending &&
                           destinationWorldFrames.load(std::memory_order_acquire) >= 2 &&
                           stablePipelineFrames.fetch_add(1, std::memory_order_acq_rel) + 1 >= 2 &&
                           StartPostLoadFade(true)) {
                    fadePending = false;
                }
                bool stableUIComposited = false;
                if (!loading && transitionActive &&
                    !postLoadPresentFallback.load(std::memory_order_acquire)) {
                    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                    auto* device = RE::BSGraphics::Renderer::GetDevice();
                    auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
                    stableUIComposited = CompositePostLoadStableUI(device, context);
                }
                const bool usePresentFallback = !compositeAfterPostProcessing || loading ||
                                                waitingForPostProcessing ||
                                                postLoadPresentFallback.load(std::memory_order_acquire);
                const bool fallbackNeeded = transitionActive && usePresentFallback &&
                                            !postProcessingComposited && !stableUIComposited;
                if (fallbackNeeded) {
                    REX::W32::ComPtr<REX::W32::ID3D11Texture2D> backBuffer;
                    const auto                                  result = a_swapChain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D,
                                                         reinterpret_cast<void**>(backBuffer.GetAddressOf()));
                    if (result >= 0 && backBuffer.Get()) {
                        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                        auto* device = RE::BSGraphics::Renderer::GetDevice();
                        auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;

                        if (device && context) {
                            REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> boundView;
                            REX::W32::ComPtr<REX::W32::ID3D11DepthStencilView> boundDepth;
                            context->OMGetRenderTargets(
                                1, boundView.GetAddressOf(), boundDepth.GetAddressOf());
                            REX::W32::D3D11_TEXTURE2D_DESC backBufferDesc{};
                            backBuffer->GetDesc(&backBufferDesc);
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
                                    0.0F, 0.0F, static_cast<float>(backBufferDesc.width),
                                    static_cast<float>(backBufferDesc.height), 0.0F, 1.0F
                                };
                                context->RSSetViewports(1, &viewport);

                                // Community Shaders 1.8.4 renders SDR UI directly to kFRAMEBUFFER
                                // when HDR and the D3D12 proxy are inactive. Resource inequality is
                                // not evidence of a private UI texture, so the watchdog always targets
                                // the actual swap buffer and preserves the UI already present there.
                                CompositeLoadingFrame(
                                    context, backBuffer.Get(), backBufferDesc, false);

                                auto* previousTarget = boundView.Get();
                                context->OMSetRenderTargets(
                                    previousTarget ? 1U : 0U,
                                    previousTarget ? &previousTarget : nullptr, boundDepth.Get());
                                if (viewportCount > 0) {
                                    context->RSSetViewports(viewportCount, previousViewports.data());
                                }
                            }
                        }
                    }
                }

                // Keep a hidden HUD hidden for the retained-frame presentation and release it on the
                // UI thread once that presentation ends. Movie writes never happen in Present itself.
                const bool hudTransition =
                    epochActive.load(std::memory_order_acquire) ||
                    preLoadDoorTransitionActive.load(std::memory_order_acquire) ||
                    postLoadFadePending.load(std::memory_order_acquire) ||
                    postLoadFadeStart.load(std::memory_order_acquire) > 0;
                const bool wantsHUDHidden =
                    mainMenuLoadActive.load(std::memory_order_acquire) ||
                    !Settings::GetSingleton().ShowHUDDuringLoading();
                if (hudVisibilityOwned.load(std::memory_order_acquire) ||
                    (hudTransition && wantsHUDHidden && !IsVanilla())) {
                    QueueHUDVisibilitySync();
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in IDXGISwapChain::Present");
            }
        }

        const auto presentResult = originalPresent(a_swapChain, a_syncInterval, a_flags);

        if (hooksEnabled.load(std::memory_order_acquire) && captureAfterDownstreamPresent &&
            !frozenFrameLocked.load(std::memory_order_acquire)) {
            try {
                captureRollingSwapChainFrame(true);
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception capturing the downstream presented frame");
            }
        }

        return presentResult;
    }

    // Logs the world state at the two render milestones used by this experiment.
    void CellTransitioner::LogRenderState(std::string_view a_timing)
    {
        if (!Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            return;
        }

        const auto* player = RE::PlayerCharacter::GetSingleton();
        const auto* cell = player ? player->GetParentCell() : nullptr;
        logger::debug("normal world render {}: cell={:08X} worldRoot={} camera={} player3D={}", a_timing,
            cell ? cell->GetFormID() : 0, RE::Main::WorldRootNode() != nullptr,
            RE::Main::WorldRootCamera() != nullptr, player && player->Get3D() != nullptr);
    }

    // Improved Camera applies NPCEyeBone height after Skyrim updates the first-person camera.
    // During a cell handoff that offset can accumulate while Skyrim's camera anchor stays fixed.
    // Correct only that extra positive displacement, before drawing the destination world. The
    // baseline retains Improved Camera's intended first-person offset; moving the player opts out.
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

        const auto state = transitionState.load(std::memory_order_acquire);
        const bool destinationRendering = state == TransitionState::waitingForStableRenderer ||
                                          state == TransitionState::compositorHandoff ||
                                          state == TransitionState::fadingToLive;
        if (!hooksEnabled.load(std::memory_order_acquire) || !destinationRendering ||
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
            logger::info("Improved Camera transition correction armed: camera offset {:.3f}", baseline);
            return;
        }

        const auto position = player->GetPosition();
        const auto displacement = position - initialPlayerPosition;
        if (displacement.Length() > movementTolerance) {
            abandoned = true;
            logger::info("Improved Camera transition correction released for player movement");
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
            logger::info("Improved Camera transition camera bounce corrected by {:.3f}", excess);
        }
    }

    // Observes completed normal-world renders. Two destination frames are required before the
    // compositor may take ownership from the opaque FaderMenu bridge.
    void CellTransitioner::ObserveRenderWorld(bool a_firstPerson)
    {
        CorrectImprovedCameraTransitionBounce();
        if (originalRenderWorld.address()) {
            originalRenderWorld(a_firstPerson);
        }

        if (hooksEnabled.load(std::memory_order_acquire)) {
            try {
                auto state = renderObservationState.load(std::memory_order_acquire);
                if (state == 1 && renderObservationState.compare_exchange_strong(state, 2)) {
                    LogRenderState("while Loading Menu is open");
                } else if (state == 3 && renderObservationState.compare_exchange_strong(state, 0)) {
                    LogRenderState("for the first time after Loading Menu closed");
                }

                const auto transition = transitionState.load(std::memory_order_acquire);
                if (transition == TransitionState::waitingForStableRenderer) {
                    destinationWorldFrames.fetch_add(1, std::memory_order_acq_rel);
                } else if (transition == TransitionState::fadingToLive &&
                           firstPersonPostHandoffQualifying.load(std::memory_order_acquire)) {
                    ObservePostHandoffCameraQualification();
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in the world-render observer");
            }
        }
    }

    // Copies the currently bound world target into the rolling frozen-frame texture.
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
            // Scaleform may be redirected to a clean UI surface. In that case the target bound by
            // BeginScaleform is intentionally black before any movie is drawn, while Skyrim's
            // kFRAMEBUFFER SRV still owns the completed world image. This is engine renderer state,
            // not a Community Shaders-private target, and was the source selected by the working
            // pre-branch capture path.
            auto& framebuffer =
                renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
            REX::W32::ComPtr<REX::W32::ID3D11Resource>  framebufferResource;
            REX::W32::ComPtr<REX::W32::ID3D11Texture2D> framebufferScene;
            if (framebuffer.SRV) {
                reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(framebuffer.SRV)
                    ->GetResource(framebufferResource.GetAddressOf());
                if (framebufferResource.Get()) {
                    framebufferResource->QueryInterface(
                        REX::W32::IID_ID3D11Texture2D,
                        reinterpret_cast<void**>(framebufferScene.GetAddressOf()));
                }
            }

            auto* captureTarget =
                framebufferScene.Get() && framebufferScene.Get() != boundTarget.Get() ?
                    framebufferScene.Get() :
                    boundTarget.Get();
            REX::W32::D3D11_TEXTURE2D_DESC desc{};
            captureTarget->GetDesc(&desc);

            if (PrepareFrozenFrame(device, desc)) {
                context->CopyResource(frozenFrame, captureTarget);
                loggedFrozenPresentation = false;

                if (!loggedFrozenFrame) {
                    if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                        logger::debug(
                            "capturing rolling {}x{} world frames before Scaleform ({})",
                            desc.width, desc.height,
                            captureTarget == framebufferScene.Get() &&
                                    framebufferScene.Get() != boundTarget.Get() ?
                                "separate kFRAMEBUFFER scene target" :
                                "bound target");
                    }
                    loggedFrozenFrame = true;
                }
            }
        }
    }

    // Captures after Skyrim binds the Scaleform target but before it draws the UI.
    void CellTransitioner::CaptureAfterScaleformBegin(void* a_renderer)
    {
        // The original call must run first because it binds the render target that contains the finished world.
        if (!originalBeginScaleform.address()) {
            return;
        }
        originalBeginScaleform(a_renderer);

        if (hooksEnabled.load(std::memory_order_acquire)) {
            try {
                // Locking preserves the last complete world frame throughout the loading epoch.
                if (!frozenFrameLocked.load(std::memory_order_acquire)) {
                    CaptureBoundWorldTarget();
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in the world-frame capture");
            }
        }
    }

    // Chains the existing image-space call, then composites into the target it leaves bound. This
    // remains valid when Community Shaders features are disabled and avoids private CS UI/HDR targets.
    void CellTransitioner::CompositeAfterPostProcessing(
        RE::ImageSpaceManager* a_manager, std::uint32_t a_3, RE::RENDER_TARGET a_target,
        void* a_4, bool a_5)
    {
        if (originalImageSpacePostProcessing) {
            originalImageSpacePostProcessing(a_manager, a_3, a_target, a_4, a_5);
        }

        if (hooksEnabled.load(std::memory_order_acquire)) {
            try {
                auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                auto* device = RE::BSGraphics::Renderer::GetDevice();
                auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
                if (device && context) {
                    // IDA confirms that r8d at the hooked call is the engine render-target ID.
                    // Read that explicit output instead of guessing from OM state left behind by CS.
                    // The rolling copy occurs only before transition ownership is locked, so loading
                    // and destination frames can never replace the retained source image.
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

                        // This is the exact surface on which the transition compositor is visibly
                        // effective. Capture it after the chained CS image-space pass, rather than
                        // renderTargets[a_target].texture: CS redirects the framebuffer and the
                        // registry's texture member is black in this configuration even though the
                        // bound HDR render target contains the completed scene.
                        const auto& window = renderer->GetRuntimeData().renderWindows[0];
                        const bool fullResolution =
                            desc.width == static_cast<std::uint32_t>(window.windowWidth) &&
                            desc.height == static_cast<std::uint32_t>(window.windowHeight) &&
                            desc.sampleDesc.count == 1;
                        if (!frozenFrameLocked.load(std::memory_order_acquire) && fullResolution &&
                            PrepareFrozenFrame(device, desc)) {
                            context->CopyResource(frozenFrame, targetTexture.Get());
                            loggedFrozenPresentation = false;
                            if (!loggedFrozenFrame &&
                                Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                                logger::debug(
                                    "capturing rolling {}x{} format {} frames from bound image-space output",
                                    desc.width, desc.height, std::to_underlying(desc.format));
                                loggedFrozenFrame = true;
                            }
                        }

                        if (fullResolution &&
                            !epochActive.load(std::memory_order_acquire) &&
                            !postLoadPresentFallback.load(std::memory_order_acquire) &&
                            postLoadFadePending.load(std::memory_order_acquire) &&
                            postProcessingPassesSincePresent.load(std::memory_order_acquire) == 0 &&
                            destinationWorldFrames.load(std::memory_order_acquire) >= 2 &&
                            stablePipelineFrames.fetch_add(1, std::memory_order_acq_rel) + 1 >= 2) {
                            StartPostLoadFade(false);
                        }

                        const bool transitionActive =
                            epochActive.load(std::memory_order_acquire) ||
                            postLoadFadePending.load(std::memory_order_acquire) ||
                            postLoadFadeStart.load(std::memory_order_acquire) > 0;
                        const bool postLoadStableUIOwner =
                            !epochActive.load(std::memory_order_acquire) &&
                            stableUICompositeAvailable.load(std::memory_order_acquire) &&
                            !postLoadPresentFallback.load(std::memory_order_acquire);
                        if (fullResolution && transitionActive && !postLoadStableUIOwner &&
                            !postLoadPresentFallback.load(std::memory_order_acquire)) {
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
                            postProcessingPassesSincePresent.fetch_add(1, std::memory_order_acq_rel);

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
                DisableHooks("unknown exception in image-space transition compositor");
            }
        }
    }

    // Records every engine fast-travel fade completion before its shared callback starts the load.
    void CellTransitioner::FastTravelFadeCallbackRun(void* a_callback)
    {
        if (hooksEnabled.load(std::memory_order_acquire)) {
            const bool useCustomTransition =
                Settings::GetSingleton().UseTransitionsForFastTravel();
            vanillaLoadPending.store(!useCustomTransition, std::memory_order_release);
            fastTravelBlackPending.store(useCustomTransition, std::memory_order_release);
            if (!useCustomTransition) {
                preLoadOwnedFader.store(false, std::memory_order_release);
                loadOwnedFader.store(false, std::memory_order_release);
                loadFaderCloseQueued.store(false, std::memory_order_release);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::debug("fast travel selected Skyrim's vanilla loading presentation");
                }
            }
        }

        if (originalFastTravelFadeCallbackRun) {
            originalFastTravelFadeCallbackRun(a_callback);
        }
    }

    // Records every engine save-load fade completion before its shared callback starts the load.
    void CellTransitioner::SaveLoadFadeCallbackRun(void* a_callback)
    {
        if (hooksEnabled.load(std::memory_order_acquire)) {
            fastTravelBlackPending.store(false, std::memory_order_release);
            const bool useCustomTransition =
                Settings::GetSingleton().UseTransitionsForSaveLoads();
            vanillaLoadPending.store(!useCustomTransition, std::memory_order_release);
            if (!useCustomTransition) {
                preLoadOwnedFader.store(false, std::memory_order_release);
                loadOwnedFader.store(false, std::memory_order_release);
                loadFaderCloseQueued.store(false, std::memory_order_release);
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::debug("save load selected Skyrim's vanilla loading presentation");
                }
            }
        }

        if (originalSaveLoadFadeCallbackRun) {
            originalSaveLoadFadeCallbackRun(a_callback);
        }
    }

    // Queues a real FaderData request instead of manipulating the movie background. IDA evidence for
    // 1.6.1170 shows FaderMenu::ProcessMessage passes these four fields directly to
    // mc_FaderMenu.initFade; a zero-duration request therefore snaps to opaque black or transparent.
    bool CellTransitioner::QueueFaderBridge(bool a_opaque)
    {
        auto* messages = RE::UIMessageQueue::GetSingleton();
        auto* manager = RE::MessageDataFactoryManager::GetSingleton();
        const auto* creator = manager ?
                                  manager->GetCreator<RE::FaderData>(RE::FaderData::CLASS_NAME) :
                                  nullptr;
        auto* data = creator ? creator->Create() : nullptr;
        if (!messages || !data) {
            logger::error("could not create FaderData for the main-menu save-load bridge");
            return false;
        }

        data->unk10 = 0;
        data->unk18 = 0;
        data->pad19 = 0;
        data->pad1A = 0;
        data->minDuration = 0.0F;
        // FaderMenu only calls mc_FaderMenu.updateFade while fadeDuration is positive. A literal
        // zero leaves the movie at its initial transparent frame even though the menu is open.
        data->fadeDuration = a_opaque ? 0.001F : 0.0F;
        data->isFadingOut = a_opaque;
        data->isBlack = true;
        data->unk26 = false;
        data->pausesGame = false;

        if (a_opaque) {
            faderBridgeReleaseQueued.store(false, std::memory_order_release);
            faderBridgeDisplaySerial.store(0, std::memory_order_release);
            faderBridgeActive.store(true, std::memory_order_release);
            messages->AddMessage(
                RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, data);
        } else {
            faderBridgeReleaseQueued.store(true, std::memory_order_release);
            messages->AddMessage(
                RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, data);
        }

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug("queued native FaderMenu bridge {}",
                a_opaque ? "opaque" : "release");
        }
        return true;
    }

    // Restores only the persistent movie state changed by load-fader suppression.
    void CellTransitioner::RestoreFaderPresentation(RE::IMenu* a_menu)
    {
        if (!a_menu || !a_menu->uiMovie ||
            !faderPresentationSuppressed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        // FaderMenu is persistent. Restore the exact presentation state observed before suppression;
        // forcing an opaque Scaleform background here produces a gray frame during StatsMenu tweens.
        a_menu->uiMovie->SetBackgroundAlpha(faderBackgroundAlpha.load(std::memory_order_acquire));
        a_menu->uiMovie->SetVisible(faderWasVisible.load(std::memory_order_acquire));
    }

    // Observes native fade requests, protecting only our internal rolling capture while Skyrim fades out.
    // The original FaderData and menu behavior remain untouched for script-driven fades and image modifiers.
    RE::UI_MESSAGE_RESULTS CellTransitioner::FaderMenuProcessMessage(
        RE::IMenu* a_menu, RE::UIMessage& a_message)
    {
        bool queuePostLoadClose = false;
        bool restorePresentation = false;
        bool releaseBridge = false;
        bool advanceOpaqueBridge = false;

        if (hooksEnabled.load(std::memory_order_acquire)) {
            const bool activeEpoch = epochActive.load(std::memory_order_acquire);
            const bool postLoadTransition =
                postLoadFadePending.load(std::memory_order_acquire) ||
                postLoadFadeStart.load(std::memory_order_acquire) > 0;

            if (a_message.data &&
                (a_message.type == RE::UI_MESSAGE_TYPE::kShow ||
                    a_message.type == RE::UI_MESSAGE_TYPE::kUpdate)) {
                const auto* data = static_cast<const RE::FaderData*>(a_message.data);
                const bool bridgeRequest = faderBridgeActive.load(std::memory_order_acquire) &&
                                           data->isBlack && data->fadeDuration <= 0.001F &&
                                           data->minDuration == 0.0F && !data->pausesGame;
                releaseBridge = bridgeRequest &&
                                faderBridgeReleaseQueued.load(std::memory_order_acquire) &&
                                !data->isFadingOut;
                advanceOpaqueBridge = bridgeRequest && data->isFadingOut;

                // SleepWaitMenu queues a callback-free, non-pausing black fade before advancing game
                // time. Sleeping can open LoadingMenu before that queued request is handled, so do not
                // let the ordinary new-black-fader-during-a-load fallback claim it.
                const auto sleepFadeDeadline =
                    sleepFadeRequestDeadline.load(std::memory_order_acquire);
                const bool sleepFadeRequest =
                    sleepFadeDeadline >= CurrentTimeMilliseconds() && !data->unk10 &&
                    data->isFadingOut && data->isBlack && !data->pausesGame;
                const bool preserveSleepFader =
                    sleepFadeRequest || sleepFaderActive.load(std::memory_order_acquire);
                const auto nativeLoadPath = GetNativeLoadPath(*data);
                const bool nativeLoadFade = nativeLoadPath != NativeLoadPath::none && data->isBlack;
                const bool vanillaLoadFade = nativeLoadFade && !UsesCustomTransition(nativeLoadPath);
                if (nativeLoadFade) {
                    fastTravelBlackPending.store(
                        nativeLoadPath == NativeLoadPath::fastTravel && !vanillaLoadFade,
                        std::memory_order_release);
                }
                auto*      ui = RE::UI::GetSingleton();
                const bool mapMenuFade = !nativeLoadFade && ui &&
                                         ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
                if (vanillaLoadFade && data->isFadingOut) {
                    vanillaLoadPending.store(true, std::memory_order_release);
                    preLoadOwnedFader.store(false, std::memory_order_release);
                    loadOwnedFader.store(false, std::memory_order_release);
                    loadFaderCloseQueued.store(false, std::memory_order_release);
                }

                if (sleepFadeRequest) {
                    sleepFadeRequestDeadline.store(0, std::memory_order_release);
                    sleepFaderActive.store(true, std::memory_order_release);
                }
                if (preserveSleepFader) {
                    preLoadOwnedFader.store(false, std::memory_order_release);
                    loadOwnedFader.store(false, std::memory_order_release);
                    loadFaderCloseQueued.store(false, std::memory_order_release);
                }
                if (mapMenuFade) {
                    // MapMenu uses FaderMenu for its own camera transitions when opening and closing.
                    // Do not let stale or fallback load ownership hide that menu-owned fade. A real
                    // fast-travel request remains load-owned because it has a native load callback.
                    preLoadOwnedFader.store(false, std::memory_order_release);
                    loadOwnedFader.store(false, std::memory_order_release);
                    loadFaderCloseQueued.store(false, std::memory_order_release);
                }

                if (newGameTransitionActive.load(std::memory_order_acquire)) {
                    if (!data->isFadingOut && data->isBlack && data->fadeDuration > 0.0F) {
                        newGameFadeRequestSeen.store(true, std::memory_order_release);
                    }
                } else if (!bridgeRequest && !preserveSleepFader && !mapMenuFade) {
                    // Static xrefs show that native load fades carry one of four dedicated completion
                    // callbacks. Papyrus FadeOutGame uses the separate callback-free builder, so this
                    // claims the initiating fader without suppressing arbitrary scripted fades.
                    if (nativeLoadFade && !vanillaLoadFade) {
                        vanillaLoadPending.store(false, std::memory_order_release);
                        loadOwnedFader.store(true, std::memory_order_release);
                        if (!activeEpoch && !postLoadTransition) {
                            preLoadOwnedFader.store(true, std::memory_order_release);
                            if (nativeLoadPath == NativeLoadPath::door) {
                                bool expected = false;
                                if (preLoadDoorCaptureLocked.compare_exchange_strong(
                                        expected, true, std::memory_order_acq_rel)) {
                                    // The door callback can detach or disable a carried/dynamic light
                                    // before LoadingMenu opens. Retain the last fully presented frame
                                    // while the light is still visible, rather than locking several
                                    // frames later during LoadingMenu construction.
                                    frozenFrameLocked.store(true, std::memory_order_release);
                                    presentation.store(ChoosePresentation(), std::memory_order_release);
                                    ReleasePersistentTransitionFrame();
                                    transitionState.store(
                                        TransitionState::preparing, std::memory_order_release);
                                    destinationWorldFrames.store(0, std::memory_order_release);
                                    stablePipelineFrames.store(0, std::memory_order_release);
                                    preLoadDoorTransitionActive.store(true, std::memory_order_release);
                                }
                            }
                        }
                    }

                    // Skyrim can enqueue its control-blocking load fader a few milliseconds after
                    // LoadingMenu closes. Keep ownership through our post-load crossfade and close it
                    // immediately; non-pausing script fades outside this transition window are untouched.
                    if (!vanillaLoadFade && data->isBlack && data->pausesGame &&
                        (activeEpoch || postLoadTransition)) {
                        loadOwnedFader.store(true, std::memory_order_release);
                        if (postLoadTransition &&
                            !loadFaderCloseQueued.exchange(true, std::memory_order_acq_rel)) {
                            queuePostLoadClose = true;
                        }
                    }

                    if (!vanillaLoadFade && activeEpoch &&
                        !faderPresentAtLoadStart.load(std::memory_order_acquire) &&
                        data->isBlack) {
                        loadOwnedFader.store(true, std::memory_order_release);
                    }
                }

                // A prior load can leave this persistent movie hidden. Restore only for a request
                // outside the load-suppression window (or for explicitly preserved native flows).
                restorePresentation = bridgeRequest || preserveSleepFader || mapMenuFade || vanillaLoadFade ||
                                      newGameTransitionActive.load(std::memory_order_acquire) ||
                                      (!activeEpoch && !postLoadTransition && !nativeLoadFade);
            } else if (a_message.type == RE::UI_MESSAGE_TYPE::kHide) {
                sleepFadeRequestDeadline.store(0, std::memory_order_release);
                sleepFaderActive.store(false, std::memory_order_release);
                if (!activeEpoch) {
                    // FadeThenFastTravelCallback/FadeThenLoadCallback finish before LoadingMenu
                    // opens. Keep their decision latched across this native fader hide so
                    // PrepareForLoad can consume it when the actual load begins.
                    preLoadOwnedFader.store(false, std::memory_order_release);
                    loadOwnedFader.store(false, std::memory_order_release);
                    loadFaderCloseQueued.store(false, std::memory_order_release);
                    fastTravelBlackActive.store(false, std::memory_order_release);
                }
            }
        }

        // The original receives the unmodified message and FaderData. Skyrim remains responsible for
        // timing, the fade curve, menu lifetime, pause behavior, and TitleSequence layering.
        const auto result = originalFaderProcessMessage ?
                                originalFaderProcessMessage(a_menu, a_message) :
                                RE::UI_MESSAGE_RESULTS::kPassOn;

        if (restorePresentation) {
            RestoreFaderPresentation(a_menu);
            if (faderBridgeActive.load(std::memory_order_acquire) && a_menu && a_menu->uiMovie) {
                a_menu->uiMovie->SetVisible(true);
            }
        }

        if (advanceOpaqueBridge && originalFaderAdvanceMovie) {
            // ProcessMessage initialized the SWF on the UI thread. Advance the native movie once so
            // its 1 ms fade reaches the opaque endpoint before LoadingMenu can be removed.
            originalFaderAdvanceMovie(a_menu, 1.0F, 0);
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("advanced native FaderMenu bridge to opaque");
            }
        }

        if (releaseBridge) {
            faderBridgeReleaseQueued.store(false, std::memory_order_release);
            faderBridgeActive.store(false, std::memory_order_release);
            if (auto* messages = RE::UIMessageQueue::GetSingleton()) {
                messages->AddMessage(
                    RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::debug("released native FaderMenu bridge");
            }
        }

        if (queuePostLoadClose) {
            if (auto* messages = RE::UIMessageQueue::GetSingleton()) {
                messages->AddMessage(RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }

        return result;
    }

    // Advances every FaderMenu normally and hides only the fade-in owned by the active ordinary load.
    void CellTransitioner::FaderMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
    {
        if (originalFaderAdvanceMovie) {
            originalFaderAdvanceMovie(a_menu, a_interval, a_currentTime);
        }

        if (!hooksEnabled.load(std::memory_order_acquire)) {
            return;
        }

        try {
            if (a_menu && a_menu->uiMovie) {
                if (newGameTransitionActive.load(std::memory_order_acquire)) {
                    // Keep Skyrim's native black fade at menu depth 3, below TitleSequenceMenu at depth 4.
                    RestoreFaderPresentation(a_menu);
                    a_menu->uiMovie->SetVisible(true);

                    const bool requestSeen = newGameFadeRequestSeen.load(std::memory_order_acquire);
                    const bool fadeFinished =
                        requestSeen && !static_cast<RE::FaderMenu*>(a_menu)->GetRuntimeData().isActive;
                    if (fadeFinished) {
                        CancelNewGameTransition();
                        logger::debug(
                            "new-game native fade-in completed; restored custom transition suppression");
                    }
                    return;
                }

                if (sleepFaderActive.load(std::memory_order_acquire)) {
                    // A load-owned request may have left this persistent movie hidden. Sleeping owns
                    // the complete native fade-out/fade-in lifetime, so explicitly restore it.
                    RestoreFaderPresentation(a_menu);
                    a_menu->uiMovie->SetVisible(true);
                    return;
                }

                if (fastTravelBlackActive.load(std::memory_order_acquire)) {
                    // Map fast travel keeps Skyrim's native FaderMenu above the loading presentation.
                    // Once loading ends, allow the same native movie to perform its destination fade-in.
                    RestoreFaderPresentation(a_menu);
                    if (epochActive.load(std::memory_order_acquire)) {
                        a_menu->uiMovie->SetVisible(true);
                    }
                    return;
                }

                if (faderBridgeActive.load(std::memory_order_acquire)) {
                    RestoreFaderPresentation(a_menu);
                    a_menu->uiMovie->SetVisible(true);
                    return;
                }

                if (transitionState.load(std::memory_order_acquire) ==
                    TransitionState::faderFallback) {
                    bool expected = false;
                    if (faderPresentationSuppressed.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        faderWasVisible.store(
                            a_menu->uiMovie->GetVisible(), std::memory_order_release);
                        faderBackgroundAlpha.store(
                            a_menu->uiMovie->GetBackgroundAlpha(), std::memory_order_release);
                    }
                    // This is the ultimate fail-closed layer. Keep it opaque until an actual native
                    // fade has been observed active and then completed.
                    a_menu->uiMovie->SetBackgroundAlpha(1.0F);
                    a_menu->uiMovie->SetVisible(true);
                    const bool nativeFadeActive =
                        static_cast<RE::FaderMenu*>(a_menu)->GetRuntimeData().isActive;
                    if (nativeFadeActive) {
                        fallbackFaderActiveSeen.store(true, std::memory_order_release);
                    }
                    const bool nativeFadeComplete =
                        !epochActive.load(std::memory_order_acquire) &&
                        fallbackFaderActiveSeen.load(std::memory_order_acquire) &&
                        !nativeFadeActive;
                    if (nativeFadeComplete) {
                        postLoadFadePending.store(false, std::memory_order_release);
                        frozenFrameLocked.store(false, std::memory_order_release);
                        transitionState.store(
                            TransitionState::rollingCapture, std::memory_order_release);
                        ReleasePersistentTransitionFrame();
                        RestoreFaderPresentation(a_menu);
                        QueueHUDVisibilitySync();
                    }
                    return;
                }

                const bool transitionWindow = epochActive.load(std::memory_order_acquire) ||
                                              postLoadFadePending.load(std::memory_order_acquire) ||
                                              postLoadFadeStart.load(std::memory_order_acquire) > 0;
                // Native door, fast-travel, and save-load callbacks submit their fader before
                // LoadingMenu opens. Hide that already-identified load cover immediately so it cannot
                // flash black in the short gap before the captured-frame transition takes over.
                const bool suppressLoadFader =
                    loadOwnedFader.load(std::memory_order_acquire) &&
                    (transitionWindow || preLoadOwnedFader.load(std::memory_order_acquire)) &&
                    !(fastTravelBlackPending.load(std::memory_order_acquire) && !transitionWindow) &&
                    !(fastTravelBlackActive.load(std::memory_order_acquire) &&
                        (epochActive.load(std::memory_order_acquire) ||
                            postLoadFadePending.load(std::memory_order_acquire)));
                if (suppressLoadFader) {
                    bool expected = false;
                    if (faderPresentationSuppressed.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        faderWasVisible.store(a_menu->uiMovie->GetVisible(), std::memory_order_release);
                        faderBackgroundAlpha.store(
                            a_menu->uiMovie->GetBackgroundAlpha(), std::memory_order_release);
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

    // Records the actual UI display boundary for the black bridge. ProcessMessage and AdvanceMovie
    // only mutate the SWF; this callback runs from Skyrim's menu display loop after the FaderMenu has
    // been submitted to the Scaleform renderer.
    void CellTransitioner::FaderMenuPostDisplay(RE::IMenu* a_menu)
    {
        if (originalFaderPostDisplay) {
            originalFaderPostDisplay(a_menu);
        }

        if (!hooksEnabled.load(std::memory_order_acquire) ||
            !faderBridgeActive.load(std::memory_order_acquire) || !a_menu || !a_menu->uiMovie ||
            !a_menu->uiMovie->GetVisible()) {
            return;
        }

        const auto previous = faderBridgeDisplaySerial.fetch_add(1, std::memory_order_acq_rel);
        if (previous == 0 && Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::debug("opaque FaderMenu bridge completed its first UI display pass");
        }
    }

    // Suppresses MistMenu presentation only while our loading compositor owns the screen. AdvanceMovie
    // remains completely native: showMist and showLoadScreen are initialization guards, not visibility flags.
    void CellTransitioner::MistMenuPostDisplay(RE::IMenu* a_menu)
    {
        const bool suppressPresentation =
            hooksEnabled.load(std::memory_order_acquire) &&
            transitionState.load(std::memory_order_acquire) != TransitionState::faderFallback &&
            (epochActive.load(std::memory_order_acquire) ||
                postLoadFadePending.load(std::memory_order_acquire) ||
                postLoadFadeStart.load(std::memory_order_acquire) > 0 ||
                fastTravelBlackActive.load(std::memory_order_acquire) ||
                newGameTransitionActive.load(std::memory_order_acquire));
        if (!suppressPresentation && originalMistPostDisplay) {
            originalMistPostDisplay(a_menu);
        }
    }

    // Closes only the FaderMenu claimed by this load, plus the load-specific MistMenu.
    void CellTransitioner::CloseResidualLoadingMenus(bool a_preserveFader)
    {
        auto* ui = RE::UI::GetSingleton();
        auto* messages = RE::UIMessageQueue::GetSingleton();
        if (!ui || !messages) {
            logger::warn("could not close residual loading menus because UI services were unavailable");
            return;
        }

        const bool closeFader = !a_preserveFader &&
                                loadOwnedFader.exchange(false, std::memory_order_acq_rel);
        preLoadOwnedFader.store(false, std::memory_order_release);
        if (!a_preserveFader) {
            faderPresentAtLoadStart.store(false, std::memory_order_release);
        }
        if (closeFader && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME) &&
            !loadFaderCloseQueued.exchange(true, std::memory_order_acq_rel)) {
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
                // IDA confirms slots 4, 5, and 6 are ProcessMessage, AdvanceMovie, and PostDisplay.
                constexpr std::size_t           processMessageIndex = 0x04;
                constexpr std::size_t           advanceMovieIndex = 0x05;
                constexpr std::size_t           postDisplayIndex = 0x06;
                REL::Relocation<std::uintptr_t> vtable{ RE::FaderMenu::VTABLE[0] };
                if (!vtable.address()) {
                    throw std::runtime_error("could not resolve the FaderMenu vtable");
                }

                const auto originalProcessAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + processMessageIndex * sizeof(std::uintptr_t));
                const auto originalAdvanceAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + advanceMovieIndex * sizeof(std::uintptr_t));
                const auto originalPostDisplayAddress = *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() + postDisplayIndex * sizeof(std::uintptr_t));
                if (!CellTransitioner::IsExecutableAddress(originalProcessAddress) ||
                    !CellTransitioner::IsExecutableAddress(originalAdvanceAddress) ||
                    !CellTransitioner::IsExecutableAddress(originalPostDisplayAddress)) {
                    throw std::runtime_error("FaderMenu hooks had no original function");
                }

                CellTransitioner::originalFaderProcessMessage =
                    reinterpret_cast<decltype(CellTransitioner::originalFaderProcessMessage)>(
                        originalProcessAddress);
                CellTransitioner::originalFaderAdvanceMovie =
                    reinterpret_cast<CellTransitioner::AdvanceMovie_t>(originalAdvanceAddress);
                CellTransitioner::originalFaderPostDisplay =
                    reinterpret_cast<CellTransitioner::PostDisplay_t>(originalPostDisplayAddress);
                vtable.write_vfunc(processMessageIndex, CellTransitioner::FaderMenuProcessMessage);
                vtable.write_vfunc(advanceMovieIndex, CellTransitioner::FaderMenuAdvanceMovie);
                vtable.write_vfunc(postDisplayIndex, CellTransitioner::FaderMenuPostDisplay);
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
                    "installed chained image-space transition compositor at {:X}; target {:X}",
                    callSite, currentTarget);
            }

            // Installs the final SDR watchdog and creates the shaders/state reused by every presented frame.
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

                if (Settings::GetSingleton().IsBlurEnabled()) {
                    if (!CellTransitioner::CreateFrozenFrameBlurShader(reinterpret_cast<::ID3D11Device*>(device))) {
                        throw std::runtime_error("could not create the frozen-frame blur shader");
                    }
                } else {
                    logger::info("frozen-frame blur is disabled");
                }

                if (!CellTransitioner::CreateSolidColorShader(reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the solid-color shader");
                }

                if (!CellTransitioner::CreateTransitionFadeShader(reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the transition-fade shader");
                }

                if (!CellTransitioner::CreateStableUITransitionShaders(
                        reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the stable UI transition shaders");
                }

                if (!CellTransitioner::CreateLoadingOverlayShader(reinterpret_cast<::ID3D11Device*>(device))) {
                    throw std::runtime_error("could not create the loading overlay shader");
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
                vtable.write_vfunc(presentIndex, CellTransitioner::PresentFrozenFrame);

                logger::info("installed frozen-frame swap-chain Present hook");
            }

        }

        // Installs every renderer and visual-transition hook.
        void InstallHooks()
        {
            // Construct the transition controller before any callback can reach it.
            CellTransitioner::GetSingleton();

            InstallRenderObservationHook();
            InstallCommunityShadersCompositeHook();
            InstallFrozenFrameHook();
            InstallFastTravelFadeCallbackHook();
            InstallSaveLoadFadeCallbackHook();
            InstallFaderMenuHook();
            InstallMistMenuHooks();
            CellTransitioner::hooksEnabled.store(true, std::memory_order_release);
        }
    }
}
