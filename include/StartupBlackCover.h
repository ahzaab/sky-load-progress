#pragma once

#include "TransitionUiOverlay.h"

namespace load_progress
{
    // Startup can present before the image-space hook publishes its final scene.
    // Cover the verified native HDR input until the ordinary compositor resumes.
    inline HRESULT CoverPendingStartupScene(ID3D11DeviceContext* context,
        const TransitionUiOverlay::Binding& binding, bool pending,
        ID3D11Texture2D* compositedScene) noexcept
    {
        if (!pending || !context || binding.pass != TransitionUiOverlay::Pass::display ||
            !binding.scene || binding.scene.Get() == compositedScene) {
            return S_FALSE;
        }

        D3D11_TEXTURE2D_DESC desc{};
        binding.scene->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            desc.Width != binding.desc.Width || desc.Height != binding.desc.Height ||
            desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
            return S_FALSE;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        context->GetDevice(device.GetAddressOf());
        const auto result = device->CreateRenderTargetView(binding.scene.Get(), nullptr, target.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }

        // Clear does not bind the RTV or touch compute/graphics state. Preserve
        // native SRV0/SRV1, UAV0, CB0 and shader identity for the chained dispatch.
        constexpr float black[]{ 0.0F, 0.0F, 0.0F, 1.0F };
        context->ClearRenderTargetView(target.Get(), black);
        return S_OK;
    }
}
