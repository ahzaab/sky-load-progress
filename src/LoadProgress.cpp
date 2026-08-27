// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "LoadProgress.h"

namespace load_progress
{
    namespace
    {
        constexpr auto queueNames = std::to_array<std::string_view>({
            "critical-refs", "refs", "distant-refs", "background", "tasks", "post-processing"
        });

        constexpr std::size_t queueCount = static_cast<std::size_t>(Queue::count);
        std::array<std::atomic_uint64_t, queueCount> liveRemaining{};
        Aggregator aggregator;
        std::atomic_bool epochActive{ false };
        enum class Presentation : std::uint8_t
        {
            loadingMenu,
            seamless
        };
        std::atomic<Presentation> presentation{ Presentation::loadingMenu };
        std::atomic_uint32_t displayedBasisPoints{};
        std::atomic_uint8_t renderObservationState{};
        std::atomic_bool awaitingControlRestore{ false };
        std::mutex stateLock;
        Progress lastLogged{};

        using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
        AdvanceMovie_t originalAdvanceMovie{};
        AdvanceMovie_t originalFaderAdvanceMovie{};
        AdvanceMovie_t originalMistAdvanceMovie{};

        using RenderWorld_t = void (*)(bool);
        REL::Relocation<RenderWorld_t> originalRenderWorld;

        using Present_t = REX::W32::HRESULT (*)(
            REX::W32::IDXGISwapChain*, std::uint32_t, std::uint32_t);
        Present_t originalPresent{};
        using ClearRenderTargetView_t = void (*)(
            REX::W32::ID3D11DeviceContext*, REX::W32::ID3D11RenderTargetView*, const float[4]);
        ClearRenderTargetView_t originalClearRenderTargetView{};
        REX::W32::ID3D11Texture2D* frozenFrame{};
        REX::W32::D3D11_TEXTURE2D_DESC frozenFrameDesc{};
        bool loggedFrozenFrame{};
        bool loggedFrozenPresentation{};

        RE::TESObjectCELL* GetQueuedDestinationCell()
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

        Presentation ChoosePresentation()
        {
            auto* cell = GetQueuedDestinationCell();
            const bool resident = cell && cell->GetRuntimeData().loadedData;
            logger::info("loading destination: cell={:08X} editorID='{}' loadedData={} attached={} presentation={}",
                cell ? cell->GetFormID() : 0, cell ? cell->GetFormEditorID() : "", resident,
                cell && cell->IsAttached(), resident ? "seamless" : "loading-menu");
            return resident ? Presentation::seamless : Presentation::loadingMenu;
        }
        struct ControlState
        {
            std::uint32_t enabled;
            std::uint32_t stored;
            bool blockInput;
            bool paused;
            bool faderOpen;
            bool mistOpen;

            bool operator==(const ControlState&) const = default;
        };

        std::optional<ControlState> lastControlState;

        ControlState GetControlState()
        {
            auto* controls = RE::ControlMap::GetSingleton();
            auto* playerControls = RE::PlayerControls::GetSingleton();
            auto* ui = RE::UI::GetSingleton();
            std::uint32_t enabled = 0;
            std::uint32_t stored = 0;
            controls->GetControlsState(enabled, stored);
            return { enabled, stored, playerControls->blockPlayerInput, ui->GameIsPaused(),
                ui->IsMenuOpen(RE::FaderMenu::MENU_NAME), ui->IsMenuOpen(RE::MistMenu::MENU_NAME) };
        }

        void ObserveControlRestore()
        {
            if (!awaitingControlRestore.load(std::memory_order_acquire)) {
                return;
            }

            const auto state = GetControlState();
            if (!lastControlState || state != *lastControlState) {
                logger::info(
                    "post-load controls: enabled={:08X} stored={:08X} blockInput={} paused={} fader={} mist={}",
                    state.enabled, state.stored, state.blockInput, state.paused, state.faderOpen, state.mistOpen);
                lastControlState = state;
            }

            auto* controls = RE::ControlMap::GetSingleton();
            const bool gameplayReady = controls->IsMovementControlsEnabled() && controls->IsLookingControlsEnabled() &&
                                       controls->IsActivateControlsEnabled() && !state.blockInput && !state.paused;
            if (gameplayReady) {
                logger::info("post-load gameplay controls are ready");
                awaitingControlRestore.store(false, std::memory_order_release);
            }
        }

        bool MatchesFrozenFrame(const REX::W32::D3D11_TEXTURE2D_DESC& a_desc)
        {
            return frozenFrame && frozenFrameDesc.width == a_desc.width && frozenFrameDesc.height == a_desc.height &&
                   frozenFrameDesc.format == a_desc.format && frozenFrameDesc.sampleDesc.count == a_desc.sampleDesc.count &&
                   frozenFrameDesc.sampleDesc.quality == a_desc.sampleDesc.quality;
        }

        bool PrepareFrozenFrame(
            REX::W32::ID3D11Device* a_device, const REX::W32::D3D11_TEXTURE2D_DESC& a_backBufferDesc)
        {
            if (MatchesFrozenFrame(a_backBufferDesc)) {
                return true;
            }
            if (frozenFrame) {
                frozenFrame->Release();
                frozenFrame = nullptr;
            }

            auto desc = a_backBufferDesc;
            desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            desc.bindFlags = 0;
            desc.cpuAccessFlags = 0;
            desc.miscFlags = 0;
            if (a_device->CreateTexture2D(&desc, nullptr, &frozenFrame) < 0) {
                logger::error("could not allocate the frozen loading frame texture");
                return false;
            }
            frozenFrameDesc = a_backBufferDesc;
            loggedFrozenFrame = false;
            return true;
        }

        REX::W32::HRESULT PresentFrozenFrame(
            REX::W32::IDXGISwapChain* a_swapChain, std::uint32_t a_syncInterval, std::uint32_t a_flags)
        {
            ObserveControlRestore();
            REX::W32::ID3D11Texture2D* backBuffer = nullptr;
            const auto getBufferResult = a_swapChain->GetBuffer(
                0, REX::W32::IID_ID3D11Texture2D, reinterpret_cast<void**>(&backBuffer));
            if (getBufferResult >= 0 && backBuffer) {
                auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                auto* device = RE::BSGraphics::Renderer::GetDevice();
                auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
                REX::W32::D3D11_TEXTURE2D_DESC desc{};
                backBuffer->GetDesc(&desc);
                if (device && context) {
                    if (epochActive.load(std::memory_order_acquire) &&
                        presentation.load(std::memory_order_acquire) == Presentation::seamless) {
                        if (MatchesFrozenFrame(desc)) {
                            context->CopyResource(backBuffer, frozenFrame);
                            if (!loggedFrozenPresentation) {
                                logger::info("presenting the frozen pre-load frame");
                                loggedFrozenPresentation = true;
                            }
                        }
                    } else if (PrepareFrozenFrame(device, desc)) {
                        context->CopyResource(frozenFrame, backBuffer);
                        if (!loggedFrozenFrame) {
                            logger::info("captured a {}x{} pre-load frame", desc.width, desc.height);
                            loggedFrozenFrame = true;
                        }
                        loggedFrozenPresentation = false;
                    }
                }
                backBuffer->Release();
            }
            return originalPresent(a_swapChain, a_syncInterval, a_flags);
        }

        void RestoreFrozenFrameBeforeUI(
            REX::W32::ID3D11DeviceContext* a_context, REX::W32::ID3D11RenderTargetView* a_view,
            const float a_color[4])
        {
            if (!epochActive.load(std::memory_order_acquire) ||
                presentation.load(std::memory_order_acquire) != Presentation::loadingMenu || !frozenFrame) {
                originalClearRenderTargetView(a_context, a_view, a_color);
                return;
            }

            REX::W32::ID3D11Resource* resource = nullptr;
            a_view->GetResource(&resource);
            REX::W32::ID3D11Texture2D* backBuffer = nullptr;
            auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
            if (window && window->swapChain) {
                window->swapChain->GetBuffer(
                    0, REX::W32::IID_ID3D11Texture2D, reinterpret_cast<void**>(&backBuffer));
            }

            if (resource && backBuffer && resource == backBuffer) {
                // Replace the loading-loop clear, then let Scaleform draw over the captured frame.
                a_context->CopyResource(backBuffer, frozenFrame);
            } else {
                originalClearRenderTargetView(a_context, a_view, a_color);
            }
            if (backBuffer) {
                backBuffer->Release();
            }
            if (resource) {
                resource->Release();
            }
        }

        std::uint64_t GetLiveRemaining()
        {
            std::uint64_t remaining = 0;
            for (const auto& queue : liveRemaining) {
                remaining += queue.load(std::memory_order_relaxed);
            }
            return remaining;
        }

        void LogRenderState(std::string_view a_timing)
        {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            const auto* cell = player ? player->GetParentCell() : nullptr;
            logger::info(
                "normal world render {}: cell={:08X} worldRoot={} camera={} player3D={} liveRemaining={}",
                a_timing, cell ? cell->GetFormID() : 0, RE::Main::WorldRootNode() != nullptr,
                RE::Main::WorldRootCamera() != nullptr, player && player->Get3D() != nullptr, GetLiveRemaining());
        }

        void ObserveRenderWorld(bool a_firstPerson)
        {
            auto state = renderObservationState.load(std::memory_order_acquire);
            if (state == 1 && renderObservationState.compare_exchange_strong(state, 2)) {
                LogRenderState("while Loading Menu is open");
            } else if (state == 3 && renderObservationState.compare_exchange_strong(state, 0)) {
                LogRenderState("for the first time after Loading Menu closed");
            }
            originalRenderWorld(a_firstPerson);
        }

        void SetNumber(RE::GFxValue& a_object, const char* a_name, double a_value)
        {
            RE::GFxValue value;
            value.SetNumber(a_value);
            a_object.SetMember(a_name, value);
        }

        bool GetNumber(const RE::GFxValue& a_object, const char* a_name, double& a_value)
        {
            RE::GFxValue value;
            if (!a_object.GetMember(a_name, &value) || !value.IsNumber()) {
                return false;
            }
            a_value = value.GetNumber();
            return true;
        }

        bool CreateProgressBar(RE::GFxMovieView* a_view)
        {
            RE::GFxValue meterRect;
            RE::GFxValue source;
            if (!a_view->GetVariable(&meterRect, "_root.Menu_mc.LevelMeterRect") || !meterRect.IsObject() ||
                !meterRect.GetMember("LevelProgressBar", &source) || !source.IsObject()) {
                logger::warn("could not find _root.Menu_mc.LevelMeterRect.LevelProgressBar");
                return false;
            }

            // Duplicate the active skin instead of supplying our own meter artwork.
            RE::GFxValue args[2];
            args[0].SetString("SkyrimLoadProgress");
            meterRect.Invoke("getNextHighestDepth", &args[1], nullptr, 0);
            RE::GFxValue ignored;
            if (!source.Invoke("duplicateMovieClip", &ignored, args, 2)) {
                logger::warn("LevelProgressBar.duplicateMovieClip failed");
                return false;
            }

            RE::GFxValue progressBar;
            if (!meterRect.GetMember("SkyrimLoadProgress", &progressBar) || !progressBar.IsObject()) {
                logger::warn("duplicated level progress bar was not addressable");
                return false;
            }

            double x = 0.0;
            double y = 0.0;
            double height = 0.0;
            GetNumber(source, "_x", x);
            GetNumber(source, "_y", y);
            GetNumber(source, "_height", height);
            SetNumber(progressBar, "_x", x);
            SetNumber(progressBar, "_y", y + height + 6.0);

            RE::GFxValue frameArgument;
            frameArgument.SetString("Empty");
            progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1);
            double emptyFrame = 1.0;
            GetNumber(progressBar, "_currentframe", emptyFrame);
            frameArgument.SetString("Full");
            progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1);
            double fullFrame = emptyFrame;
            GetNumber(progressBar, "_currentframe", fullFrame);
            // Some loading menu skins place Full before Empty on the timeline.
            SetNumber(progressBar, "_slpEmptyFrame", emptyFrame);
            SetNumber(progressBar, "_slpFullFrame", fullFrame);

            logger::info(
                "duplicated Loading Menu level meter at ({:.1f}, {:.1f}); height={:.1f}, frames empty={:.0f} full={:.0f}",
                x, y + height + 6.0, height, emptyFrame, fullFrame);
            return true;
        }

        bool SetMeterPercent(RE::GFxValue& a_meter, double a_percent)
        {
            double emptyFrame = 0.0;
            double fullFrame = 0.0;
            if (!GetNumber(a_meter, "_slpEmptyFrame", emptyFrame) ||
                !GetNumber(a_meter, "_slpFullFrame", fullFrame) || fullFrame == emptyFrame) {
                return false;
            }

            const auto percent = std::clamp(a_percent, 0.0, 100.0);
            const auto frame = std::floor(emptyFrame + (fullFrame - emptyFrame) * percent / 100.0);
            RE::GFxValue ignored;
            RE::GFxValue argument;
            argument.SetNumber(frame);
            return a_meter.Invoke("gotoAndStop", &ignored, &argument, 1);
        }

        void UpdateProgressBar(RE::IMenu* a_menu)
        {
            if (!a_menu || !a_menu->uiMovie) {
                return;
            }

            RE::GFxValue progressBar;
            if (!a_menu->uiMovie->GetVariable(
                    &progressBar, "_root.Menu_mc.LevelMeterRect.SkyrimLoadProgress") ||
                !progressBar.IsObject()) {
                if (!CreateProgressBar(a_menu->uiMovie.get())) {
                    return;
                }
                if (!a_menu->uiMovie->GetVariable(
                        &progressBar, "_root.Menu_mc.LevelMeterRect.SkyrimLoadProgress") ||
                    !progressBar.IsObject()) {
                    return;
                }
            }

            const auto basisPoints = displayedBasisPoints.load(std::memory_order_acquire);
            if (!SetMeterPercent(progressBar, static_cast<double>(basisPoints) / 100.0)) {
                static bool warned = false;
                if (!warned) {
                    logger::warn("duplicated level meter has invalid Empty/Full frame labels");
                    warned = true;
                }
            }
        }

        void LoadingMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
        {
            originalAdvanceMovie(a_menu, a_interval, a_currentTime);
            if (a_menu && a_menu->uiMovie) {
                const bool seamless = presentation.load(std::memory_order_acquire) == Presentation::seamless;
                a_menu->uiMovie->SetBackgroundAlpha(0.0F);
                a_menu->uiMovie->SetVisible(!seamless);
                if (!seamless) {
                    UpdateProgressBar(a_menu);
                }
            }
        }

        void FaderMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
        {
            originalFaderAdvanceMovie(a_menu, a_interval, a_currentTime);
            if (a_menu && a_menu->uiMovie) {
                // FaderMenu supplies the black or white transition over the world.
                a_menu->uiMovie->SetBackgroundAlpha(0.0F);
                a_menu->uiMovie->SetVisible(false);
            }
        }

        void MistMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
        {
            originalMistAdvanceMovie(a_menu, a_interval, a_currentTime);
            auto* mistMenu = static_cast<RE::MistMenu*>(a_menu);
            auto& runtimeData = mistMenu->GetRuntimeData();
            runtimeData.showMist = false;
            runtimeData.showLoadScreen = false;
        }

        void DisableMistMenuPostDisplay(RE::IMenu*)
        {}

        void InstallLoadingMenuHook()
        {
            constexpr std::size_t advanceMovieIndex = 0x05;
            REL::Relocation<std::uintptr_t> vtable{ RE::LoadingMenu::VTABLE[0] };
            const auto original = vtable.write_vfunc(advanceMovieIndex, LoadingMenuAdvanceMovie);
            originalAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(original);
            logger::info("installed hidden LoadingMenu::AdvanceMovie experiment hook");
        }

        void InstallFaderMenuHook()
        {
            constexpr std::size_t advanceMovieIndex = 0x05;
            REL::Relocation<std::uintptr_t> vtable{ RE::FaderMenu::VTABLE[0] };
            const auto original = vtable.write_vfunc(advanceMovieIndex, FaderMenuAdvanceMovie);
            originalFaderAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(original);
            logger::info("installed hidden FaderMenu::AdvanceMovie experiment hook");
        }

        void InstallMistMenuHooks()
        {
            constexpr std::size_t advanceMovieIndex = 0x05;
            constexpr std::size_t postDisplayIndex = 0x06;
            REL::Relocation<std::uintptr_t> vtable{ RE::MistMenu::VTABLE[0] };
            const auto original = vtable.write_vfunc(advanceMovieIndex, MistMenuAdvanceMovie);
            originalMistAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(original);
            vtable.write_vfunc(postDisplayIndex, DisableMistMenuPostDisplay);
            logger::info("disabled MistMenu mist, background, and load-screen model rendering");
        }

        void DisableImageSpaceModifier(RE::ImageSpaceModifierInstance*)
        {}

        void InstallImageSpaceModifierHook()
        {
            constexpr std::size_t applyIndex = 0x26;
            REL::Relocation<std::uintptr_t> vtable{ RE::ImageSpaceModifierInstanceForm::VTABLE[0] };
            vtable.write_vfunc(applyIndex, DisableImageSpaceModifier);
            logger::info("disabled form-backed image-space modifiers for the experiment");
        }

        void LogProgress(const Progress& a_progress)
        {
            const auto candidate = a_progress.total ?
                static_cast<std::uint32_t>(std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) : 0u;
            auto displayed = displayedBasisPoints.load(std::memory_order_relaxed);
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

        void OnEnqueue(Queue a_queue)
        {
            liveRemaining[static_cast<std::size_t>(a_queue)].fetch_add(1, std::memory_order_relaxed);
            if (!epochActive.load(std::memory_order_acquire)) {
                return;
            }
            std::scoped_lock lock(stateLock);
            aggregator.Enqueue(a_queue);
            LogProgress(aggregator.Current());
        }

        void OnComplete(Queue a_queue)
        {
            auto& live = liveRemaining[static_cast<std::size_t>(a_queue)];
            auto value = live.load(std::memory_order_relaxed);
            while (value != 0 && !live.compare_exchange_weak(value, value - 1, std::memory_order_relaxed)) {}
            if (!epochActive.load(std::memory_order_acquire)) {
                return;
            }
            std::scoped_lock lock(stateLock);
            aggregator.Complete(a_queue);
            LogProgress(aggregator.Current());
        }

        void CriticalEnqueue(CONTEXT&) { OnEnqueue(Queue::criticalReferences); }
        void CriticalComplete(CONTEXT&) { OnComplete(Queue::criticalReferences); }
        void ReferenceEnqueue(CONTEXT&) { OnEnqueue(Queue::references); }
        void ReferenceComplete(CONTEXT&) { OnComplete(Queue::references); }
        void DistantEnqueue(CONTEXT&) { OnEnqueue(Queue::distantReferences); }
        void DistantComplete(CONTEXT&) { OnComplete(Queue::distantReferences); }

        struct MutationHook
        {
            REL::ID id;
            std::ptrdiff_t offset;
            void (*callback)(CONTEXT&);
            std::string_view name;
        };

        const auto mutationHooks = std::to_array<MutationHook>({
            { REL::ID(19155), 0x07, CriticalEnqueue, "critical enqueue" },
            { REL::ID(19156), 0x0C, CriticalComplete, "critical completion" },
            { REL::ID(19151), 0x07, ReferenceEnqueue, "reference enqueue" },
            { REL::ID(19152), 0x0C, ReferenceComplete, "reference completion" },
            { REL::ID(19159), 0x4E, DistantEnqueue, "distant enqueue" },
            { REL::ID(19160), 0x69, DistantComplete, "distant completion" }
        });

        void InstallMutationHooks()
        {
            SKSE::AllocTrampoline(4096);
            for (const auto& hook : mutationHooks) {
                const auto address = REL::Relocation<std::uintptr_t>(hook.id).address() + hook.offset;
                // Each target is a seven-byte lock inc/dec instruction.
                if (!SKSE::stl::install_context_hook(address, 7, hook.callback, 7)) {
                    throw std::runtime_error(fmt::format("could not install {} hook at {:X}", hook.name, address));
                }
                logger::info("installed direct {} hook at {:X}", hook.name, address);
            }
        }

        void InstallRenderObservationHook()
        {
            constexpr std::ptrdiff_t renderWorldCallOffset = 0x85E;
            const auto callSite = REL::Relocation<std::uintptr_t>(REL::ID(36559)).address() + renderWorldCallOffset;
            if (*reinterpret_cast<const std::uint8_t*>(callSite) != 0xE8) {
                throw std::runtime_error("normal-world-render call site did not match Skyrim 1.7.99");
            }
            originalRenderWorld = SKSE::GetTrampoline().write_call<5>(callSite, ObserveRenderWorld);
            logger::info("installed passive normal-world-render observation hook at {:X}", callSite);
        }

        void InstallFrozenFrameHook()
        {
            constexpr std::size_t presentIndex = 0x08;
            auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
            if (!window || !window->swapChain) {
                throw std::runtime_error("could not find Skyrim's swap chain");
            }
            const auto vtableAddress = *reinterpret_cast<std::uintptr_t*>(window->swapChain);
            REL::Relocation<std::uintptr_t> vtable{ vtableAddress };
            const auto original = vtable.write_vfunc(presentIndex, PresentFrozenFrame);
            originalPresent = reinterpret_cast<Present_t>(original);
            logger::info("installed frozen-frame swap-chain Present hook");

            constexpr std::size_t clearRenderTargetViewIndex = 0x32;
            auto* context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
            const auto contextVtableAddress = *reinterpret_cast<std::uintptr_t*>(context);
            REL::Relocation<std::uintptr_t> contextVtable{ contextVtableAddress };
            const auto originalClear = contextVtable.write_vfunc(clearRenderTargetViewIndex, RestoreFrozenFrameBeforeUI);
            originalClearRenderTargetView = reinterpret_cast<ClearRenderTargetView_t>(originalClear);
            logger::info("installed frozen-frame render-target clear hook");
        }

        void CloseResidualLoadingMenus()
        {
            auto* ui = RE::UI::GetSingleton();
            auto* messages = RE::UIMessageQueue::GetSingleton();
            if (ui->IsMenuOpen(RE::FaderMenu::MENU_NAME)) {
                messages->AddMessage(RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            if (ui->IsMenuOpen(RE::MistMenu::MENU_NAME)) {
                messages->AddMessage(RE::MistMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            logger::info("queued immediate closure of residual loading menus");
        }

        class Events final :
            public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
            public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
        public:
            static Events& GetSingleton()
            {
                static Events singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                if (a_event->opening) {
                    std::scoped_lock lock(stateLock);
                    presentation.store(ChoosePresentation(), std::memory_order_release);
                    renderObservationState.store(1, std::memory_order_release);
                    displayedBasisPoints.store(0, std::memory_order_release);
                    aggregator.Begin();
                    // Work may already be queued before the Loading Menu opens.
                    for (std::size_t i = 0; i < queueCount; ++i) {
                        const auto baseline = liveRemaining[i].load(std::memory_order_relaxed);
                        for (std::uint64_t n = 0; n < baseline; ++n) {
                            aggregator.Enqueue(static_cast<Queue>(i));
                        }
                    }
                    lastLogged = {};
                    epochActive.store(true, std::memory_order_release);
                    logger::info("loading epoch began: Loading Menu opened; baseline remaining={}",
                        aggregator.Current().remaining);
                    LogProgress(aggregator.Current());
                } else {
                    std::scoped_lock lock(stateLock);
                    epochActive.store(false, std::memory_order_release);
                    const auto final = aggregator.Current();
                    logger::info("loading epoch ended: Loading Menu closed; completed={} remaining={} total={}",
                        final.completed, final.remaining, final.total);
                    aggregator.End();
                    const auto renderState = renderObservationState.load(std::memory_order_acquire);
                    if (renderState == 1 || renderState == 2) {
                        renderObservationState.store(3, std::memory_order_release);
                    }
                    lastControlState.reset();
                    awaitingControlRestore.store(true, std::memory_order_release);
                    CloseResidualLoadingMenus();
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellFullyLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
            {
                if (renderObservationState.load(std::memory_order_acquire) != 0 && a_event && a_event->cell) {
                    logger::info("cell fully loaded: formID={:08X} editorID='{}' menuOpen={} liveRemaining={}",
                        a_event->cell->GetFormID(), a_event->cell->GetFormEditorID(),
                        epochActive.load(std::memory_order_acquire), GetLiveRemaining());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Aggregator::Begin()
    {
        remaining_.fill(0);
        total_.fill(0);
        progress_ = {};
        active_ = true;
    }

    void Aggregator::Enqueue(Queue a_queue)
    {
        if (!active_) {
            return;
        }
        const auto index = static_cast<std::size_t>(a_queue);
        ++remaining_[index];
        ++total_[index];
        logger::debug("queue '{}' enqueued one item", queueNames[index]);
        Recalculate();
    }

    void Aggregator::Complete(Queue a_queue)
    {
        if (!active_) {
            return;
        }
        const auto index = static_cast<std::size_t>(a_queue);
        if (remaining_[index] == 0) {
            ++total_[index];
            logger::debug("queue '{}' completed an unobserved item", queueNames[index]);
        } else {
            --remaining_[index];
            logger::debug("queue '{}' completed one item", queueNames[index]);
        }
        Recalculate();
    }

    void Aggregator::Recalculate()
    {
        progress_.total = 0;
        progress_.remaining = 0;
        for (std::size_t i = 0; i < queueCount; ++i) {
            progress_.total += total_[i];
            progress_.remaining += remaining_[i];
        }
        progress_.completed = progress_.total - progress_.remaining;
        progress_.fraction = progress_.total ?
            static_cast<double>(progress_.completed) / static_cast<double>(progress_.total) : 0.0;
    }

    Progress Aggregator::Current() const { return progress_; }
    void Aggregator::End() { active_ = false; }

    void Install()
    {
        InstallMutationHooks();
        InstallRenderObservationHook();
        InstallFrozenFrameHook();
        InstallLoadingMenuHook();
        InstallFaderMenuHook();
        InstallMistMenuHooks();
        InstallImageSpaceModifierHook();
        auto& events = Events::GetSingleton();
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&events);
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
        logger::info("installed loading-menu and cell-fully-loaded event sinks");
    }
}
