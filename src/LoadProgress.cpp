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
        constexpr auto postLoadFadeDelay = std::chrono::milliseconds(250);
        constexpr auto postLoadFadeDuration = std::chrono::milliseconds(1000);
        constexpr auto lightTransitionDuration = std::chrono::milliseconds(750);
        std::array<std::atomic_uint64_t, queueCount> liveRemaining{};
        Aggregator aggregator;
        std::atomic_bool epochActive{ false };
        std::atomic_bool frozenFrameLocked{ false };
        std::atomic_int64_t postLoadFadeStart{};
        enum class Presentation : std::uint8_t
        {
            loadingMenu,
            seamless
        };
        enum class TransitionProfile : std::uint8_t
        {
            standard,
            light
        };
        std::atomic<Presentation> presentation{ Presentation::loadingMenu };
        std::atomic<TransitionProfile> transitionProfile{ TransitionProfile::standard };
        std::atomic_int64_t loadingTransitionStart{};
        std::atomic_uint32_t displayedBasisPoints{};
        std::atomic_uint8_t renderObservationState{};
        std::atomic_bool awaitingControlRestore{ false };
        std::mutex stateLock;
        Progress lastLogged{};

        using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
        using ProcessMessage_t = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
        ProcessMessage_t originalLoadingProcessMessage{};
        AdvanceMovie_t originalAdvanceMovie{};
        AdvanceMovie_t originalFaderAdvanceMovie{};
        AdvanceMovie_t originalMistAdvanceMovie{};

        using RenderWorld_t = void (*)(bool);
        REL::Relocation<RenderWorld_t> originalRenderWorld;
        using BeginScaleform_t = void (*)(void*);
        REL::Relocation<BeginScaleform_t> originalBeginScaleform;

        using Present_t = REX::W32::HRESULT (*)(
            REX::W32::IDXGISwapChain*, std::uint32_t, std::uint32_t);
        Present_t originalPresent{};
        REX::W32::ID3D11Texture2D* frozenFrame{};
        REX::W32::ID3D11ShaderResourceView* frozenFrameView{};
        REX::W32::ID3D11Texture2D* loadingOverlay{};
        REX::W32::ID3D11ShaderResourceView* loadingOverlayView{};
        REX::W32::D3D11_TEXTURE2D_DESC frozenFrameDesc{};
        std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
        std::unique_ptr<DirectX::CommonStates> commonStates;
        ::ID3D11PixelShader* frozenFrameBlurShader{};
        ::ID3D11PixelShader* solidColorShader{};
        ::ID3D11PixelShader* loadingOverlayShader{};
        bool loggedFrozenFrame{};
        bool loggedFrozenPresentation{};

        bool CreateLoadingOverlayShader(::ID3D11Device* a_device)
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

            REX::W32::ID3DBlob* bytecode = nullptr;
            REX::W32::ID3DBlob* errors = nullptr;
            const auto result = REX::W32::D3DCompile(source.data(), source.size(), "LoadingOverlay", nullptr, nullptr,
                "main", "ps_5_0", 0, 0, &bytecode, &errors);
            if (FAILED(result)) {
                logger::error("could not compile the loading overlay shader: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
                if (errors) {
                    errors->Release();
                }
                return false;
            }
            if (errors) {
                errors->Release();
            }
            const auto createResult = a_device->CreatePixelShader(
                bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &loadingOverlayShader);
            bytecode->Release();
            return SUCCEEDED(createResult);
        }

        bool CreateSolidColorShader(::ID3D11Device* a_device)
        {
            constexpr std::string_view source = R"(
float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    return color;
}
)";

            REX::W32::ID3DBlob* bytecode = nullptr;
            REX::W32::ID3DBlob* errors = nullptr;
            const auto result = REX::W32::D3DCompile(source.data(), source.size(), "SolidColor", nullptr, nullptr,
                "main", "ps_5_0", 0, 0, &bytecode, &errors);
            if (FAILED(result)) {
                logger::error("could not compile the solid-color shader: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
                if (errors) {
                    errors->Release();
                }
                return false;
            }
            if (errors) {
                errors->Release();
            }
            const auto createResult = a_device->CreatePixelShader(
                bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &solidColorShader);
            bytecode->Release();
            return SUCCEEDED(createResult);
        }

        std::int64_t CurrentTimeMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        bool CreateFrozenFrameBlurShader(::ID3D11Device* a_device)
        {
            constexpr std::string_view source = R"(
Texture2D frozenTexture : register(t0);
SamplerState frozenSampler : register(s0);

float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    uint width;
    uint height;
    frozenTexture.GetDimensions(width, height);
    float2 texel = float2(2.0 / width, 2.0 / height);

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

            REX::W32::ID3DBlob* bytecode = nullptr;
            REX::W32::ID3DBlob* errors = nullptr;
            const auto result = REX::W32::D3DCompile(source.data(), source.size(), "FrozenFrameBlur", nullptr, nullptr,
                "main", "ps_5_0", 0, 0, &bytecode, &errors);
            if (FAILED(result)) {
                logger::error("could not compile the frozen-frame blur shader: {}",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
                if (errors) {
                    errors->Release();
                }
                return false;
            }
            if (errors) {
                errors->Release();
            }
            const auto createResult = a_device->CreatePixelShader(
                bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &frozenFrameBlurShader);
            bytecode->Release();
            return SUCCEEDED(createResult);
        }

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
            const std::string_view editorID = cell ? cell->GetFormEditorID() : "";
            const bool usesLightTransition =
                editorID.starts_with("DLC2Book") || editorID.starts_with("DLC2POIBook");
            const auto profile = usesLightTransition ? TransitionProfile::light : TransitionProfile::standard;
            const auto selected = usesLightTransition || !resident ? Presentation::loadingMenu : Presentation::seamless;
            transitionProfile.store(profile, std::memory_order_release);
            logger::info(
                "loading destination: cell={:08X} editorID='{}' loadedData={} attached={} presentation={} transition={}",
                cell ? cell->GetFormID() : 0, cell ? cell->GetFormEditorID() : "", resident,
                cell && cell->IsAttached(), selected == Presentation::seamless ? "seamless" : "loading-menu",
                profile == TransitionProfile::light ? "light" : "standard");
            return selected;
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
            if (frozenFrameView) {
                frozenFrameView->Release();
                frozenFrameView = nullptr;
            }
            if (loadingOverlayView) {
                loadingOverlayView->Release();
                loadingOverlayView = nullptr;
            }
            if (loadingOverlay) {
                loadingOverlay->Release();
                loadingOverlay = nullptr;
            }

            auto desc = a_backBufferDesc;
            desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.cpuAccessFlags = 0;
            desc.miscFlags = 0;
            if (a_device->CreateTexture2D(&desc, nullptr, &frozenFrame) < 0 ||
                a_device->CreateShaderResourceView(frozenFrame, nullptr, &frozenFrameView) < 0) {
                logger::error("could not allocate the frozen loading frame texture");
                return false;
            }
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            if (a_device->CreateTexture2D(&desc, nullptr, &loadingOverlay) < 0 ||
                a_device->CreateShaderResourceView(loadingOverlay, nullptr, &loadingOverlayView) < 0) {
                logger::error("could not allocate the loading-menu overlay texture");
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
                    } else if (epochActive.load(std::memory_order_acquire) &&
                        presentation.load(std::memory_order_acquire) == Presentation::loadingMenu &&
                        MatchesFrozenFrame(desc) && frozenFrameView && loadingOverlay && loadingOverlayView) {
                        // Preserve Scaleform, blur the world snapshot, then add the UI over it.
                        context->CopyResource(loadingOverlay, backBuffer);
                        RECT destination{ 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };
                        spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->Opaque(),
                            commonStates->LinearClamp(), nullptr, nullptr,
                            [thisContext = reinterpret_cast<::ID3D11DeviceContext*>(context)] {
                                thisContext->PSSetShader(frozenFrameBlurShader, nullptr, 0);
                            });
                        spriteBatch->Draw(
                            reinterpret_cast<::ID3D11ShaderResourceView*>(frozenFrameView), destination);
                        spriteBatch->End();
                        if (transitionProfile.load(std::memory_order_acquire) == TransitionProfile::light) {
                            const auto elapsed = CurrentTimeMilliseconds() -
                                                 loadingTransitionStart.load(std::memory_order_acquire);
                            const auto whiteAlpha = std::clamp(static_cast<float>(elapsed) /
                                                                   static_cast<float>(
                                                                       lightTransitionDuration.count()),
                                0.0F, 1.0F);
                            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred,
                                commonStates->NonPremultiplied(), nullptr, nullptr, nullptr,
                                [thisContext = reinterpret_cast<::ID3D11DeviceContext*>(context)] {
                                    thisContext->PSSetShader(solidColorShader, nullptr, 0);
                                });
                            spriteBatch->Draw(reinterpret_cast<::ID3D11ShaderResourceView*>(frozenFrameView),
                                destination, DirectX::XMVectorSet(1.0F, 1.0F, 1.0F, whiteAlpha));
                            spriteBatch->End();
                        }
                        spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->NonPremultiplied(),
                            nullptr, nullptr, nullptr, [thisContext = reinterpret_cast<::ID3D11DeviceContext*>(context)] {
                                thisContext->PSSetShader(loadingOverlayShader, nullptr, 0);
                            });
                        spriteBatch->Draw(
                            reinterpret_cast<::ID3D11ShaderResourceView*>(loadingOverlayView), destination);
                        spriteBatch->End();
                    } else if (const auto fadeStart = postLoadFadeStart.load(std::memory_order_acquire);
                               fadeStart > 0) {
                        const auto elapsed = CurrentTimeMilliseconds() - fadeStart;
                        const auto delay = presentation.load(std::memory_order_acquire) == Presentation::loadingMenu ?
                                               postLoadFadeDelay.count() :
                                               0;
                        const auto fadeElapsed = elapsed - delay;
                        if (fadeElapsed >= postLoadFadeDuration.count()) {
                            postLoadFadeStart.store(0, std::memory_order_release);
                            frozenFrameLocked.store(false, std::memory_order_release);
                            logger::info("post-load frozen-frame crossfade completed");
                        } else if (MatchesFrozenFrame(desc) && frozenFrameView) {
                            const auto fadeProgress = std::clamp(static_cast<float>(fadeElapsed) /
                                                                     static_cast<float>(postLoadFadeDuration.count()),
                                0.0F, 1.0F);
                            const auto alpha = 1.0F - fadeProgress;
                            RECT destination{ 0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height) };
                            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->NonPremultiplied(),
                                commonStates->LinearClamp(), nullptr, nullptr,
                                [thisContext = reinterpret_cast<::ID3D11DeviceContext*>(context)] {
                                    thisContext->PSSetShader(
                                        transitionProfile.load(std::memory_order_acquire) == TransitionProfile::light ?
                                            solidColorShader :
                                            frozenFrameBlurShader,
                                        nullptr, 0);
                                });
                            spriteBatch->Draw(reinterpret_cast<::ID3D11ShaderResourceView*>(frozenFrameView),
                                destination, DirectX::XMVectorSet(1.0F, 1.0F, 1.0F, alpha));
                            spriteBatch->End();
                        }
                    }
                }
                backBuffer->Release();
            }
            return originalPresent(a_swapChain, a_syncInterval, a_flags);
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

        void CaptureAfterScaleformBegin(void* a_renderer)
        {
            originalBeginScaleform(a_renderer);
            if (!frozenFrameLocked.load(std::memory_order_acquire)) {
                auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                auto* device = RE::BSGraphics::Renderer::GetDevice();
                auto* context = renderer ? renderer->GetRuntimeData().context : nullptr;
                REX::W32::ID3D11RenderTargetView* renderTargetView = nullptr;
                if (device && context) {
                    context->OMGetRenderTargets(1, &renderTargetView, nullptr);
                }
                if (renderTargetView) {
                    REX::W32::ID3D11Resource* resource = nullptr;
                    REX::W32::ID3D11Texture2D* renderTarget = nullptr;
                    renderTargetView->GetResource(&resource);
                    if (resource && resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                        reinterpret_cast<void**>(&renderTarget)) >= 0 &&
                        renderTarget) {
                        REX::W32::D3D11_TEXTURE2D_DESC desc{};
                        renderTarget->GetDesc(&desc);
                        if (PrepareFrozenFrame(device, desc)) {
                            context->CopyResource(frozenFrame, renderTarget);
                            if (!loggedFrozenFrame) {
                                logger::info("capturing rolling {}x{} world frames before Scaleform", desc.width,
                                    desc.height);
                                loggedFrozenFrame = true;
                            }
                            loggedFrozenPresentation = false;
                        }
                        renderTarget->Release();
                    } else {
                        logger::warn("bound UI render target was not a texture");
                    }
                    if (resource) {
                        resource->Release();
                    }
                    renderTargetView->Release();
                } else {
                    logger::warn("no render target was bound before Scaleform rendering");
                }
            }
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

        RE::UI_MESSAGE_RESULTS LoadingMenuProcessMessage(RE::IMenu* a_menu, RE::UIMessage& a_message)
        {
            if (a_message.type == RE::UI_MESSAGE_TYPE::kShow) {
                const auto selected = ChoosePresentation();
                presentation.store(selected, std::memory_order_release);
                // Keep the last normal world frame; the loading render path is already underway.
                frozenFrameLocked.store(true, std::memory_order_release);
                postLoadFadeStart.store(0, std::memory_order_release);
                loadingTransitionStart.store(CurrentTimeMilliseconds(), std::memory_order_release);
                if (selected == Presentation::loadingMenu) {
                    // UI captures the current frame before presenting a freeze-background menu.
                    a_menu->menuFlags.set(
                        RE::UI_MENU_FLAGS::kFreezeFrameBackground,
                        RE::UI_MENU_FLAGS::kUsesBlurredBackground);
                } else {
                    a_menu->menuFlags.reset(
                        RE::UI_MENU_FLAGS::kFreezeFrameBackground,
                        RE::UI_MENU_FLAGS::kUsesBlurredBackground);
                }
            }
            return originalLoadingProcessMessage(a_menu, a_message);
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
            constexpr std::size_t processMessageIndex = 0x04;
            constexpr std::size_t advanceMovieIndex = 0x05;
            REL::Relocation<std::uintptr_t> vtable{ RE::LoadingMenu::VTABLE[0] };
            const auto originalProcess = vtable.write_vfunc(processMessageIndex, LoadingMenuProcessMessage);
            originalLoadingProcessMessage = reinterpret_cast<ProcessMessage_t>(originalProcess);
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

        void InstallWorldCaptureHook()
        {
            constexpr std::ptrdiff_t beginScaleformCallOffset = 0x18A;
            const auto callSite =
                REL::Relocation<std::uintptr_t>(REL::ID(82084)).address() + beginScaleformCallOffset;
            if (*reinterpret_cast<const std::uint8_t*>(callSite) != 0xE8) {
                throw std::runtime_error("Scaleform-begin call site did not match Skyrim 1.7.99");
            }
            originalBeginScaleform = SKSE::GetTrampoline().write_call<5>(callSite, CaptureAfterScaleformBegin);
            logger::info("installed world-only capture hook after Scaleform target binding at {:X}", callSite);
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
            spriteBatch = std::make_unique<DirectX::SpriteBatch>(
                reinterpret_cast<::ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context));
            commonStates = std::make_unique<DirectX::CommonStates>(
                reinterpret_cast<::ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()));
            if (!CreateFrozenFrameBlurShader(
                    reinterpret_cast<::ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()))) {
                throw std::runtime_error("could not create the frozen-frame blur shader");
            }
            if (!CreateSolidColorShader(
                    reinterpret_cast<::ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()))) {
                throw std::runtime_error("could not create the solid-color shader");
            }
            if (!CreateLoadingOverlayShader(
                    reinterpret_cast<::ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice()))) {
                throw std::runtime_error("could not create the loading overlay shader");
            }
            logger::info("installed frozen-frame swap-chain Present hook");

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
                    postLoadFadeStart.store(CurrentTimeMilliseconds(), std::memory_order_release);
                    logger::info("post-load frozen-frame crossfade began with {} ms delay",
                        presentation.load(std::memory_order_acquire) == Presentation::loadingMenu ?
                            postLoadFadeDelay.count() :
                            0);
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
        InstallWorldCaptureHook();
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
