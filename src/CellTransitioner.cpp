// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"

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
// 3. InstallFrozenFrameHook replaces IDXGISwapChain::Present. The hook runs after Skyrim finishes the
//    frame, composites the selected transition into the back buffer, and then calls the real Present.
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
        bool IsNativeLoadFade(const RE::FaderData& a_data)
        {
            if (!a_data.unk10) {
                return false;
            }

            const auto callbackVtable =
                *reinterpret_cast<const std::uintptr_t*>(a_data.unk10);
            static REL::Relocation<std::uintptr_t> normalDoor{
                RE::VTABLE___NormalDoorFadeCallback[0] };
            static REL::Relocation<std::uintptr_t> autoDoor{
                RE::VTABLE___AutoDoorFadeCallback[0] };
            static REL::Relocation<std::uintptr_t> fastTravel{
                RE::VTABLE___FadeThenFastTravelCallback[0] };
            static REL::Relocation<std::uintptr_t> loadSave{
                RE::VTABLE___FadeThenLoadCallback[0] };

            return callbackVtable == normalDoor.address() ||
                   callbackVtable == autoDoor.address() ||
                   callbackVtable == fastTravel.address() ||
                   callbackVtable == loadSave.address();
        }
    }

    // Returns the singleton that owns all cell-transition state.
    CellTransitioner& CellTransitioner::GetSingleton()
    {
        static CellTransitioner singleton;
        return singleton;
    }

    // Disables transition behavior while preserving pass-through calls to Skyrim.
    void CellTransitioner::DisableHooks(std::string_view a_reason) noexcept
    {
        hooksEnabled.store(false, std::memory_order_release);
        epochActive.store(false, std::memory_order_release);
        frozenFrameLocked.store(false, std::memory_order_release);
        preLoadOwnedFader.store(false, std::memory_order_release);
        loadOwnedFader.store(false, std::memory_order_release);
        loadFaderCloseQueued.store(false, std::memory_order_release);
        awaitingControlRestore.store(false, std::memory_order_release);
        newGameTransitionActive.store(false, std::memory_order_release);
        newGameFadeRequestSeen.store(false, std::memory_order_release);

        if (!failureLogged.exchange(true, std::memory_order_acq_rel)) {
            try {
                logger::critical("cell-transition hooks disabled: {}", a_reason);
            } catch (...) {
                REX::W32::OutputDebugStringA("Skyrim Load Progress: cell-transition hooks disabled\n");
            }
        }
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
        std::ptrdiff_t     a_verifiedOffset,
        std::string_view   a_name)
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

        // This is the capture gate. Once closed, the rolling texture remains the last pre-load world frame.
        frozenFrameLocked.store(true, std::memory_order_release);
        postLoadFadeStart.store(0, std::memory_order_release);
        // Menu construction and renderer suspension can consume the configured fade before the first
        // loading frame is presented. Start the visible color fade from Present instead.
        loadingTransitionStart.store(0, std::memory_order_release);

        const bool selectCapturedColor =
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color &&
            colorSource.load(std::memory_order_acquire) == Settings::ColorSource::dominant;
        dominantColorPending.store(selectCapturedColor, std::memory_order_release);

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
        renderObservationState.store(1, std::memory_order_release);
    }

    // Starts the retained-frame fade and post-load control diagnostics.
    void CellTransitioner::EndLoad()
    {
        // MQ101 owns its first-gameplay fade. Its native FaderMenu remains below TitleSequenceMenu, so release
        // our loading cover without adding the ordinary post-load compositor above those title cards.
        const bool newGame = newGameTransitionActive.load(std::memory_order_acquire);

        // Opening the post-load gate lets Present composite over the destination cell as soon as it returns.
        epochActive.store(false, std::memory_order_release);
        postLoadFadeStart.store(newGame ? 0 : CurrentTimeMilliseconds(), std::memory_order_release);
        if (newGame) {
            frozenFrameLocked.store(false, std::memory_order_release);
            logger::info("handed the new-game transition from the loading compositor to Skyrim's FaderMenu");
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
        if (!newGame) {
            CloseResidualLoadingMenus();
        }
    }

    // Arms the one-time black loading presentation before SKSE lets Skyrim create the new game.
    void CellTransitioner::BeginNewGameTransition()
    {
        newGameFadeRequestSeen.store(false, std::memory_order_release);
        newGameTransitionActive.store(true, std::memory_order_release);
        logger::info("armed the new-game black/title-sequence transition");
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

    // Returns whether the current transition suppresses LoadingMenu Scaleform.
    bool CellTransitioner::IsSeamless()
    {
        return presentation.load(std::memory_order_acquire) == Presentation::seamless;
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

    // Returns a monotonic timestamp for transition timing.
    std::int64_t CellTransitioner::CurrentTimeMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
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
    return pixel * color;
}
)";

        constexpr std::string_view token = "$BLUR_AMOUNT$";
        const auto                 amount = fmt::format("{:.3f}", Settings::GetSingleton().GetBlurAmount());
        for (auto position = source.find(token); position != std::string::npos; position = source.find(token)) {
            source.replace(position, token.size(), amount);
        }

        return CreatePixelShader(a_device, source, "frozen frame blur", &frozenFrameBlurShader);
    }

    // Resolves the queued fast-travel or door destination to its cell record.
    RE::TESObjectCELL* CellTransitioner::GetQueuedDestinationCell()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        const auto& target = player->GetPlayerRuntimeData().queuedTargetLoc;
        if (!target.isValid) {
            return nullptr;
        }
        if (target.interior) {
            return target.interior;
        }
        if (!target.world) {
            return nullptr;
        }

        // Exterior cells use signed 4096-unit grid coordinates.
        const auto x = static_cast<std::int16_t>(std::floor(target.location.x / 4096.0F));
        const auto y = static_cast<std::int16_t>(std::floor(target.location.y / 4096.0F));
        const auto it = target.world->cellMap.find(RE::CellID(y, x));
        return it != target.world->cellMap.end() ? it->second : nullptr;
    }

    // Chooses the warm or cold presentation before the Loading Menu opens.
    CellTransitioner::Presentation CellTransitioner::ChoosePresentation()
    {
        mainMenuLoadActive.store(false, std::memory_order_release);

        if (newGameTransitionActive.load(std::memory_order_acquire)) {
            mainMenuLoadPending.store(false, std::memory_order_release);
            // The loading compositor supplies opaque black beneath LoadingMenu. Once loading ends, Skyrim's
            // native FaderMenu takes over so TitleSequenceMenu retains its higher UI depth.
            transitionType.store(Settings::TransitionType::color, std::memory_order_release);
            colorSource.store(Settings::ColorSource::fixed, std::memory_order_release);
            transitionColor.store(0x000000, std::memory_order_release);
            fadeInDuration.store(0, std::memory_order_release);
            holdAfterLoad.store(0, std::memory_order_release);
            fadeOutDuration.store(0, std::memory_order_release);

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("selected the opaque new-game loading presentation");
            }
            return Presentation::loadingMenu;
        }

        auto*                  cell = GetQueuedDestinationCell();
        const bool             resident = cell && cell->GetRuntimeData().loadedData;
        const auto*            editorIDText = cell ? cell->GetFormEditorID() : nullptr;
        const std::string_view editorID = editorIDText ? editorIDText : "";

        auto*      ui = RE::UI::GetSingleton();
        const bool fromMainMenu = mainMenuLoadPending.exchange(false, std::memory_order_acq_rel) ||
                                  (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME));
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
                logger::info(
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
            logger::info(
                "loading destination: cell={:08X} editorID='{}' loadedData={} attached={} presentation={} transition={} "
                "fadeIn={}ms hold={}ms fadeOut={}ms",
                cell ? cell->GetFormID() : 0, editorID, resident,
                cell && cell->IsAttached(), selected == Presentation::seamless ? "seamless" : "loading-menu",
                type == Settings::TransitionType::color ? "color" : "blur", fadeInDuration.load(),
                holdAfterLoad.load(), fadeOutDuration.load());

            if (type == Settings::TransitionType::color) {
                logger::info("color transition: source={} fallback=#{:06X}",
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
                logger::info(
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
                logger::info("post-load gameplay controls are ready");
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
                const auto* pixel = row + x * 4;
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
            logger::info("selected captured-frame transition color #{:06X}", transitionColor.load());
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

    // Replaces the loading frame with the unmodified pre-load world image.
    void CellTransitioner::PresentSeamlessFrame(
        REX::W32::ID3D11DeviceContext*        a_context,
        REX::W32::ID3D11Texture2D*            a_backBuffer,
        const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        if (!a_context || !a_backBuffer || !MatchesFrozenFrame(a_desc)) {
            return;
        }

        a_context->CopyResource(a_backBuffer, frozenFrame);

        if (!loggedFrozenPresentation) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("presenting the frozen pre-load frame");
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
        if (!a_context || !a_backBuffer || !commonStates || !MatchesFrozenFrame(a_desc) ||
            !frozenFrameView || (!a_separateUI && (!loadingOverlay || !loadingOverlayView))) {
            return;
        }

        if (!a_separateUI) {
            // Vanilla renders Scaleform into the scene target. Save it so its widgets can be restored.
            a_context->CopyResource(loadingOverlay, a_backBuffer);
        }

        const auto destination = GetDestinationRect(a_desc);
        if (mainMenuLoadActive.load(std::memory_order_acquire)) {
            // Never expose the captured Main Menu movie or its text. The solid shader ignores the
            // retained texture and supplies the same black cover used for the post-load fade.
            DrawFullscreenLayer(a_context, frozenFrameView, destination, commonStates->Opaque(), nullptr,
                solidColorShader, TransitionColor(1.0F));
        } else {
            DrawFullscreenLayer(a_context, frozenFrameView, destination, commonStates->Opaque(),
                commonStates->LinearClamp(), GetFrozenFrameShader());
        }

        if (!mainMenuLoadActive.load(std::memory_order_acquire) &&
            transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color) {
            const auto now = CurrentTimeMilliseconds();
            auto       start = loadingTransitionStart.load(std::memory_order_acquire);
            if (start == 0) {
                if (loadingTransitionStart.compare_exchange_strong(
                        start, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    start = now;

                    if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                        logger::info("started visible color fade-in");
                    }
                }
            }

            const auto elapsed = std::max<std::int64_t>(now - start, 0);
            const auto duration = fadeInDuration.load(std::memory_order_acquire);
            const auto colorAlpha = duration > 0 ?
                                        std::clamp(static_cast<float>(elapsed) / static_cast<float>(duration), 0.0F, 1.0F) :
                                        1.0F;

            // Color transitions move from the captured world toward their fixed or sampled color.
            DrawFullscreenLayer(a_context, frozenFrameView, destination, commonStates->NonPremultiplied(), nullptr,
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
        REX::W32::ID3D11DeviceContext* a_context, const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
    {
        if (!a_context || !commonStates) {
            return;
        }

        const auto fadeStart = postLoadFadeStart.load(std::memory_order_acquire);
        if (fadeStart <= 0) {
            return;
        }

        // Cold loads briefly hold the cover after the menu closes. Warm loads begin blending immediately.
        const auto delay = holdAfterLoad.load(std::memory_order_acquire);
        const auto fadeElapsed = CurrentTimeMilliseconds() - fadeStart - delay;
        const auto duration = fadeOutDuration.load(std::memory_order_acquire);

        if (fadeElapsed >= duration) {
            postLoadFadeStart.store(0, std::memory_order_release);
            frozenFrameLocked.store(false, std::memory_order_release);
            mainMenuLoadActive.store(false, std::memory_order_release);
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("post-load frozen-frame crossfade completed");
            }
            return;
        }

        if (!MatchesFrozenFrame(a_desc) || !frozenFrameView) {
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

        // Blur transitions fade the retained frame; color transitions fade their solid color.
        DrawFullscreenLayer(a_context, frozenFrameView, GetDestinationRect(a_desc),
            commonStates->NonPremultiplied(), commonStates->LinearClamp(),
            usesColor ? solidColorShader : GetFrozenFrameShader(), color);
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
        const bool loading = epochActive.load(std::memory_order_acquire);

        if (loading && transitionType.load(std::memory_order_acquire) == Settings::TransitionType::color &&
            dominantColorPending.exchange(false, std::memory_order_acq_rel)) {
            UpdateTransitionColor(a_context);
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

        if (hooksEnabled.load(std::memory_order_acquire)) {
            try {
                ObserveControlRestore();

                REX::W32::ComPtr<REX::W32::ID3D11Texture2D> backBuffer;
                const auto                                  result = a_swapChain->GetBuffer(0, REX::W32::IID_ID3D11Texture2D,
                                                     reinterpret_cast<void**>(backBuffer.GetAddressOf()));
                if (result >= 0 && backBuffer.Get()) {
                    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                    auto* device = RE::BSGraphics::Renderer::GetDevice();
                    auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;

                    if (device && context) {
                        const bool transitionActive =
                            epochActive.load(std::memory_order_acquire) ||
                            postLoadFadeStart.load(std::memory_order_acquire) > 0;

                        auto& framebuffer = renderer->GetRuntimeData().renderTargets[
                            RE::RENDER_TARGET::kFRAMEBUFFER];

                        REX::W32::ComPtr<REX::W32::ID3D11RenderTargetView> boundView;
                        REX::W32::ComPtr<REX::W32::ID3D11DepthStencilView> boundDepth;
                        REX::W32::ComPtr<REX::W32::ID3D11Resource> boundResource;
                        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> boundTexture;
                        REX::W32::ComPtr<REX::W32::ID3D11Resource> sceneResource;
                        REX::W32::ComPtr<REX::W32::ID3D11Texture2D> sceneTexture;
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
                                framebuffer.SRV)->GetResource(sceneResource.GetAddressOf());
                            if (sceneResource.Get()) {
                                sceneResource->QueryInterface(
                                    REX::W32::IID_ID3D11Texture2D,
                                    reinterpret_cast<void**>(sceneTexture.GetAddressOf()));
                            }
                        }

                        // Community Shaders redirects Scaleform to a separate transparent UI target while
                        // retaining the scene in kFRAMEBUFFER.SRV. Composite into the scene so its
                        // HDR/frame-generation present chain can combine our transition with that UI.
                        const bool separateUI =
                            transitionActive && sceneTexture.Get() && boundTexture.Get() &&
                            sceneTexture.Get() != boundTexture.Get();
                        if (separateUI) {
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
                                    static_cast<float>(sceneDesc.height), 0.0F, 1.0F };
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
                            // Vanilla path: the completed back buffer contains both scene and Scaleform.
                            REX::W32::D3D11_TEXTURE2D_DESC desc{};
                            backBuffer->GetDesc(&desc);
                            CompositeLoadingFrame(context, backBuffer.Get(), desc);
                        }
                    }
                }
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in IDXGISwapChain::Present");
            }
        }

        return originalPresent(a_swapChain, a_syncInterval, a_flags);
    }

    // Logs the world state at the two render milestones used by this experiment.
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

    // Observes when Skyrim stops and resumes its normal world-render call.
    void CellTransitioner::ObserveRenderWorld(bool a_firstPerson)
    {
        if (hooksEnabled.load(std::memory_order_acquire)) {
            try {
                auto state = renderObservationState.load(std::memory_order_acquire);
                if (state == 1 && renderObservationState.compare_exchange_strong(state, 2)) {
                    LogRenderState("while Loading Menu is open");
                } else if (state == 3 && renderObservationState.compare_exchange_strong(state, 0)) {
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
            auto& framebuffer = renderer->GetRuntimeData().renderTargets[
                RE::RENDER_TARGET::kFRAMEBUFFER];
            REX::W32::ComPtr<REX::W32::ID3D11Resource> framebufferResource;
            REX::W32::ComPtr<REX::W32::ID3D11Texture2D> framebufferScene;
            if (framebuffer.SRV) {
                reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(
                    framebuffer.SRV)->GetResource(framebufferResource.GetAddressOf());
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

            if (PrepareFrozenFrame(device, desc)) {
                // With a redirected UI target, kFRAMEBUFFER.SRV retains the completed scene.
                context->CopyResource(frozenFrame, renderTarget);
                loggedFrozenPresentation = false;

                if (!loggedFrozenFrame) {
                    if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                        logger::info(
                            "capturing rolling {}x{} world frames before Scaleform ({})",
                            desc.width, desc.height,
                            renderTarget == framebufferScene.Get() &&
                                    framebufferScene.Get() != boundTarget.Get() ?
                                "separate scene target" :
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

    // Observes native fade requests, protecting only our internal rolling capture while Skyrim fades out.
    // The original FaderData and menu behavior remain untouched for script-driven fades and image modifiers.
    RE::UI_MESSAGE_RESULTS CellTransitioner::FaderMenuProcessMessage(
        RE::IMenu* a_menu, RE::UIMessage& a_message)
    {
        bool queuePostLoadClose = false;

        if (hooksEnabled.load(std::memory_order_acquire)) {
            const bool activeEpoch = epochActive.load(std::memory_order_acquire);
            const bool postLoadTransition =
                postLoadFadeStart.load(std::memory_order_acquire) > 0;

            if (a_message.data &&
                (a_message.type == RE::UI_MESSAGE_TYPE::kShow ||
                    a_message.type == RE::UI_MESSAGE_TYPE::kUpdate)) {
                const auto* data = static_cast<const RE::FaderData*>(a_message.data);

                if (newGameTransitionActive.load(std::memory_order_acquire)) {
                    if (!data->isFadingOut && data->isBlack && data->fadeDuration > 0.0F) {
                        newGameFadeRequestSeen.store(true, std::memory_order_release);
                    }
                } else {
                    // Static xrefs show that native load fades carry one of four dedicated completion
                    // callbacks. Papyrus FadeOutGame uses the separate callback-free builder, so this
                    // claims the initiating fader without suppressing arbitrary scripted fades.
                    if (IsNativeLoadFade(*data) && data->isBlack) {
                        loadOwnedFader.store(true, std::memory_order_release);
                        if (!activeEpoch && !postLoadTransition) {
                            preLoadOwnedFader.store(true, std::memory_order_release);
                        }
                    }

                    // Skyrim can enqueue its control-blocking load fader a few milliseconds after
                    // LoadingMenu closes. Keep ownership through our post-load crossfade and close it
                    // immediately; non-pausing script fades outside this transition window are untouched.
                    if (data->isBlack && data->pausesGame &&
                        (activeEpoch || postLoadTransition)) {
                        loadOwnedFader.store(true, std::memory_order_release);
                        if (postLoadTransition &&
                            !loadFaderCloseQueued.exchange(true, std::memory_order_acq_rel)) {
                            queuePostLoadClose = true;
                        }
                    }

                    if (activeEpoch &&
                        !faderPresentAtLoadStart.load(std::memory_order_acquire) &&
                        data->isBlack) {
                        loadOwnedFader.store(true, std::memory_order_release);
                    }
                }
            } else if (a_message.type == RE::UI_MESSAGE_TYPE::kHide && !activeEpoch) {
                preLoadOwnedFader.store(false, std::memory_order_release);
                loadOwnedFader.store(false, std::memory_order_release);
                loadFaderCloseQueued.store(false, std::memory_order_release);
            }
        }

        // The original receives the unmodified message and FaderData. Skyrim remains responsible for
        // timing, the fade curve, menu lifetime, pause behavior, and TitleSequence layering.
        const auto result = originalFaderProcessMessage ?
                                originalFaderProcessMessage(a_menu, a_message) :
                                RE::UI_MESSAGE_RESULTS::kPassOn;

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
                    a_menu->uiMovie->SetVisible(true);

                    const bool requestSeen = newGameFadeRequestSeen.load(std::memory_order_acquire);
                    const bool fadeFinished =
                        requestSeen && !static_cast<RE::FaderMenu*>(a_menu)->GetRuntimeData().isActive;
                    if (fadeFinished) {
                        CancelNewGameTransition();
                        logger::info(
                            "new-game native fade-in completed; restored custom transition suppression");
                    }
                    return;
                }

                const bool transitionWindow = epochActive.load(std::memory_order_acquire) ||
                                              postLoadFadeStart.load(std::memory_order_acquire) > 0;
                if (transitionWindow && loadOwnedFader.load(std::memory_order_acquire)) {
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

    // Suppresses MistMenu presentation only while our loading compositor owns the screen. AdvanceMovie
    // remains completely native: showMist and showLoadScreen are initialization guards, not visibility flags.
    void CellTransitioner::MistMenuPostDisplay(RE::IMenu* a_menu)
    {
        const bool suppressPresentation =
            hooksEnabled.load(std::memory_order_acquire) &&
            (epochActive.load(std::memory_order_acquire) ||
                postLoadFadeStart.load(std::memory_order_acquire) > 0 ||
                newGameTransitionActive.load(std::memory_order_acquire));
        if (!suppressPresentation && originalMistPostDisplay) {
            originalMistPostDisplay(a_menu);
        }
    }

    // Closes only the FaderMenu claimed by this load, plus the load-specific MistMenu.
    void CellTransitioner::CloseResidualLoadingMenus()
    {
        auto* ui = RE::UI::GetSingleton();
        auto* messages = RE::UIMessageQueue::GetSingleton();
        if (!ui || !messages) {
            logger::warn("could not close residual loading menus because UI services were unavailable");
            return;
        }

        const bool closeFader = loadOwnedFader.exchange(false, std::memory_order_acq_rel);
        preLoadOwnedFader.store(false, std::memory_order_release);
        faderPresentAtLoadStart.store(false, std::memory_order_release);
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
            InstallWorldCaptureHook();
            InstallFrozenFrameHook();
            InstallFaderMenuHook();
            InstallMistMenuHooks();
            CellTransitioner::hooksEnabled.store(true, std::memory_order_release);
        }
    }
}
