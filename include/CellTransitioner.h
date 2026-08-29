// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include "Settings.h"

namespace load_progress
{
    class CellTransitioner final
    {
    public:
        enum class Presentation : std::uint8_t
        {
            loadingMenu,
            seamless
        };

        struct ControlState
        {
            std::uint32_t enabled;
            std::uint32_t stored;
            bool          blockInput;
            bool          paused;
            bool          faderOpen;
            bool          mistOpen;

            bool operator==(const ControlState&) const = default;
        };

        using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
        using RenderWorld_t = void (*)(bool);
        using BeginScaleform_t = void (*)(void*);
        using Present_t = REX::W32::HRESULT (*)(REX::W32::IDXGISwapChain*, std::uint32_t, std::uint32_t);
        using PostDisplay_t = void (*)(RE::IMenu*);
        using ApplyImageSpaceModifier_t = void (*)(RE::ImageSpaceModifierInstance*);

        static CellTransitioner& GetSingleton();
        static Presentation      PrepareForLoad(RE::IMenu* a_menu);
        static void              BeginLoad();
        static void              EndLoad();
        static bool              IsSeamless();
        static void              DisableHooks(std::string_view) noexcept;
        static bool              IsExecutableAddress(std::uintptr_t) noexcept;
        static std::uintptr_t    FindUniqueRelativeCall(
            REL::RelocationID, REL::RelocationID, std::string_view);

        static bool                            CreatePixelShader(::ID3D11Device*, std::string_view, std::string_view, ::ID3D11PixelShader**);
        static bool                            CreateLoadingOverlayShader(::ID3D11Device*);
        static bool                            CreateSolidColorShader(::ID3D11Device*);
        static std::int64_t                    CurrentTimeMilliseconds();
        static bool                            CreateFrozenFrameBlurShader(::ID3D11Device*);
        static RE::TESObjectCELL*              GetQueuedDestinationCell();
        static Presentation                    ChoosePresentation();
        static std::optional<ControlState>     GetControlState();
        static void                            ObserveControlRestore();
        static bool                            MatchesFrozenFrame(const REX::W32::D3D11_TEXTURE2D_DESC&);
        static void                            ReleaseFrameResources();
        static bool                            PrepareFrozenFrame(REX::W32::ID3D11Device*, const REX::W32::D3D11_TEXTURE2D_DESC&);
        static bool                            IsBgraFormat(REX::W32::DXGI_FORMAT);
        static bool                            IsRgbaFormat(REX::W32::DXGI_FORMAT);
        static std::array<std::uint32_t, 4096> BuildColorHistogram(
            const REX::W32::D3D11_MAPPED_SUBRESOURCE&, bool);
        static std::optional<std::uint32_t> SelectDominantColor(const std::array<std::uint32_t, 4096>&);
        static void                         UpdateTransitionColor(REX::W32::ID3D11DeviceContext*);
        static DirectX::XMVECTOR            TransitionColor(float);
        static ::ID3D11PixelShader*         GetFrozenFrameShader();
        static RECT                         GetDestinationRect(const REX::W32::D3D11_TEXTURE2D_DESC&);
        static void                         DrawFullscreenLayer(REX::W32::ID3D11DeviceContext*, REX::W32::ID3D11ShaderResourceView*,
                                    const RECT&, ::ID3D11BlendState*, ::ID3D11SamplerState*, ::ID3D11PixelShader*,
                                    DirectX::XMVECTOR = DirectX::Colors::White);
        static void                         PresentSeamlessFrame(
                                    REX::W32::ID3D11DeviceContext*, REX::W32::ID3D11Texture2D*, const REX::W32::D3D11_TEXTURE2D_DESC&);
        static void PresentLoadingMenuFrame(
            REX::W32::ID3D11DeviceContext*, REX::W32::ID3D11Texture2D*, const REX::W32::D3D11_TEXTURE2D_DESC&);
        static void PresentPostLoadFrame(REX::W32::ID3D11DeviceContext*, const REX::W32::D3D11_TEXTURE2D_DESC&);
        static void CompositeLoadingFrame(
            REX::W32::ID3D11DeviceContext*, REX::W32::ID3D11Texture2D*, const REX::W32::D3D11_TEXTURE2D_DESC&);
        static REX::W32::HRESULT PresentFrozenFrame(REX::W32::IDXGISwapChain*, std::uint32_t, std::uint32_t);
        static void              LogRenderState(std::string_view);
        static void              ObserveRenderWorld(bool);
        static void              CaptureBoundWorldTarget();
        static void              CaptureAfterScaleformBegin(void*);
        static void              FaderMenuAdvanceMovie(RE::IMenu*, float, std::uint32_t);
        static void              MistMenuAdvanceMovie(RE::IMenu*, float, std::uint32_t);
        static void              DisableMistMenuPostDisplay(RE::IMenu*);
        static void              DisableImageSpaceModifier(RE::ImageSpaceModifierInstance*);
        static void              CloseResidualLoadingMenus();

        inline static std::atomic_bool                      epochActive{ false };
        inline static std::atomic_bool                      hooksEnabled{ false };
        inline static std::atomic_bool                      failureLogged{ false };
        inline static std::atomic_bool                      frozenFrameLocked{ false };
        inline static std::atomic_int64_t                   postLoadFadeStart{};
        inline static std::atomic<Presentation>             presentation{ Presentation::loadingMenu };
        inline static std::atomic<Settings::TransitionType> transitionType{ Settings::TransitionType::blur };
        inline static std::atomic<Settings::ColorSource>    colorSource{ Settings::ColorSource::dominant };
        inline static std::atomic_int64_t                   fadeInDuration{ 750 };
        inline static std::atomic_int64_t                   holdAfterLoad{ 250 };
        inline static std::atomic_int64_t                   fadeOutDuration{ 1000 };
        inline static std::atomic_int64_t                   loadingTransitionStart{};
        inline static std::atomic_bool                      dominantColorPending{ false };
        inline static std::atomic_uint32_t                  transitionColor{ 0xFFFFFF };
        inline static std::atomic_uint8_t                   renderObservationState{};
        inline static std::atomic_bool                      awaitingControlRestore{ false };
        inline static std::optional<ControlState>           lastControlState;
        inline static std::mutex                            controlStateLock;

        inline static AdvanceMovie_t                         originalFaderAdvanceMovie{};
        inline static AdvanceMovie_t                         originalMistAdvanceMovie{};
        inline static PostDisplay_t                          originalMistPostDisplay{};
        inline static ApplyImageSpaceModifier_t              originalImageSpaceModifierApply{};
        inline static REL::Relocation<RenderWorld_t>         originalRenderWorld;
        inline static REL::Relocation<BeginScaleform_t>      originalBeginScaleform;
        inline static Present_t                              originalPresent{};
        inline static REX::W32::ID3D11Texture2D*             frozenFrame{};
        inline static REX::W32::ID3D11ShaderResourceView*    frozenFrameView{};
        inline static REX::W32::ID3D11Texture2D*             dominantColorReadback{};
        inline static REX::W32::ID3D11Texture2D*             loadingOverlay{};
        inline static REX::W32::ID3D11ShaderResourceView*    loadingOverlayView{};
        inline static REX::W32::D3D11_TEXTURE2D_DESC         frozenFrameDesc{};
        inline static std::unique_ptr<DirectX::SpriteBatch>  spriteBatch;
        inline static std::unique_ptr<DirectX::CommonStates> commonStates;
        inline static ::ID3D11PixelShader*                   frozenFrameBlurShader{};
        inline static ::ID3D11PixelShader*                   solidColorShader{};
        inline static ::ID3D11PixelShader*                   loadingOverlayShader{};
        inline static bool                                   loggedFrozenFrame{};
        inline static bool                                   loggedFrozenPresentation{};

    private:
        CellTransitioner() = default;
        CellTransitioner(const CellTransitioner&) = delete;
        CellTransitioner(CellTransitioner&&) = delete;
        CellTransitioner& operator=(const CellTransitioner&) = delete;
        CellTransitioner& operator=(CellTransitioner&&) = delete;
    };

    namespace transitions
    {
        // Installs renderer and visual-transition hooks.
        void InstallHooks();
    }
}
