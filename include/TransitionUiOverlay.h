#pragma once

#include <array>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace load_progress
{
    // Native HDR conversion has already produced premultiplied, display-encoded
    // UI. Cover the generated world in that same color space, below native UI.
    // No CPU readback, CS-private layout, or frame-generation control is needed.
    class TransitionUiOverlay
    {
        template <class T>
        using Ptr = Microsoft::WRL::ComPtr<T>;

    public:
        using Dispatch = void (*)(ID3D11DeviceContext*, UINT, UINT, UINT);
        enum class Pass { none, display, ui };
        struct Binding
        {
            Pass pass = Pass::none;
            Ptr<ID3D11UnorderedAccessView> output;
            Ptr<ID3D11Texture2D> texture;
            Ptr<ID3D11Texture2D> scene;
            Ptr<ID3D11Texture2D> ui;
            Ptr<ID3D11Buffer> constants;
            D3D11_TEXTURE2D_DESC desc{};
        };

        // A snapshot is usable only by the paired UI conversion in this Present.
        void BeginPresent() noexcept
        {
            ready = false;
            nativeUi.Reset();
            nativeConstants.Reset();
        }

        void ReleaseTextures() noexcept
        {
            BeginPresent();
            snapshot.Reset();
            snapshotView.Reset();
        }

        void Reset() noexcept
        {
            ReleaseTextures();
            shader.Reset();
            device.Reset();
            shaderStatus = E_PENDING;
        }

        HRESULT Initialize(ID3D11Device* currentDevice) noexcept
        {
            if (!currentDevice) {
                return E_POINTER;
            }
            if (device.Get() != currentDevice) {
                Reset();
                device = currentDevice;
            }
            if (shaderStatus != E_PENDING) {
                return shaderStatus;
            }
            constexpr char source[] = R"(
Texture2D<float4> Scene : register(t0);
RWTexture2D<float4> UI : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 p : SV_DispatchThreadID)
{
    uint w, h;
    UI.GetDimensions(w, h);
    if (p.x >= w || p.y >= h) return;
    float4 ui = UI[p.xy];
    UI[p.xy] = float4(ui.rgb + Scene.Load(int3(p.xy, 0)).rgb * (1 - saturate(ui.a)), 1);
}
)";
            Ptr<ID3DBlob> code;
            shaderStatus = D3DCompile(source, sizeof(source) - 1, "TransitionUiOverlay", nullptr, nullptr,
                "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), nullptr);
            if (SUCCEEDED(shaderStatus)) {
                shaderStatus = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                    shader.GetAddressOf());
            }
            return shaderStatus;
        }

        Binding Inspect(ID3D11DeviceContext* context, UINT width, UINT height) const noexcept
        {
            Binding result;
            context->CSGetUnorderedAccessViews(0, 1, result.output.GetAddressOf());
            if (!result.output) {
                return result;
            }
            Ptr<ID3D11Resource> resource;
            result.output->GetResource(resource.GetAddressOf());
            if (FAILED(resource.As(&result.texture))) {
                return result;
            }
            result.texture->GetDesc(&result.desc);
            if (result.desc.Width != width || result.desc.Height != height ||
                result.desc.SampleDesc.Count != 1 || result.desc.MipLevels != 1 || result.desc.ArraySize != 1) {
                return result;
            }
            context->CSGetConstantBuffers(0, 1, result.constants.GetAddressOf());
            if (!result.constants) {
                return result;
            }
            if (ready && result.texture.Get() == nativeUi.Get() &&
                result.constants.Get() == nativeConstants.Get() &&
                result.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                result.pass = Pass::ui;
                return result;
            }
            if (result.desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
                return result;
            }
            std::array<Ptr<ID3D11ShaderResourceView>, 2> inputs;
            std::array<ID3D11ShaderResourceView*, 2> raw{};
            context->CSGetShaderResources(0, 2, raw.data());
            for (size_t i = 0; i < raw.size(); ++i) {
                inputs[i].Attach(raw[i]);
                if (!inputs[i]) {
                    return result;
                }
            }
            inputs[0]->GetResource(resource.ReleaseAndGetAddressOf());
            if (FAILED(resource.As(&result.scene))) {
                return result;
            }
            D3D11_TEXTURE2D_DESC sceneDesc{};
            result.scene->GetDesc(&sceneDesc);
            if (sceneDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
                sceneDesc.Width != width || sceneDesc.Height != height) {
                return result;
            }
            inputs[1]->GetResource(resource.ReleaseAndGetAddressOf());
            if (FAILED(resource.As(&result.ui))) {
                return result;
            }
            D3D11_TEXTURE2D_DESC uiDesc{};
            result.ui->GetDesc(&uiDesc);
            if (uiDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && uiDesc.Width == width && uiDesc.Height == height &&
                uiDesc.SampleDesc.Count == 1 && uiDesc.MipLevels == 1 && uiDesc.ArraySize == 1 &&
                result.ui.Get() != result.texture.Get()) {
                result.pass = Pass::display;
            }
            return result;
        }

        HRESULT Capture(ID3D11DeviceContext* context, const Binding& binding) noexcept
        {
            BeginPresent();
            if (binding.pass != Pass::display) {
                return S_FALSE;
            }
            Ptr<ID3D11Device> currentDevice;
            context->GetDevice(currentDevice.GetAddressOf());
            const auto initialized = Initialize(currentDevice.Get());
            if (FAILED(initialized)) {
                return initialized;
            }
            D3D11_TEXTURE2D_DESC existing{};
            if (snapshot) {
                snapshot->GetDesc(&existing);
            }
            if (!snapshot || existing.Width != binding.desc.Width || existing.Height != binding.desc.Height) {
                snapshotView.Reset();
                snapshot.Reset();
                auto desc = binding.desc;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                auto result = device->CreateTexture2D(&desc, nullptr, snapshot.GetAddressOf());
                if (FAILED(result)) {
                    return result;
                }
                result = device->CreateShaderResourceView(snapshot.Get(), nullptr, snapshotView.GetAddressOf());
                if (FAILED(result)) {
                    snapshot.Reset();
                    return result;
                }
            }
            ID3D11UnorderedAccessView* nullOutput{};
            context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
            context->CopyResource(snapshot.Get(), binding.texture.Get());
            auto* restore = binding.output.Get();
            context->CSSetUnorderedAccessViews(0, 1, &restore, nullptr);
            nativeUi = binding.ui;
            nativeConstants = binding.constants;
            ready = true;
            return S_OK;
        }

        HRESULT Compose(ID3D11DeviceContext* context, const Binding& binding, Dispatch dispatch) noexcept
        {
            if (!ready || binding.pass != Pass::ui || !dispatch ||
                binding.texture.Get() != nativeUi.Get() || binding.constants.Get() != nativeConstants.Get()) {
                return S_FALSE;
            }
            ready = false;
            Ptr<ID3D11ComputeShader> savedShader;
            std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
            UINT count = static_cast<UINT>(instances.size());
            context->CSGetShader(savedShader.GetAddressOf(), instances.data(), &count);
            Ptr<ID3D11ShaderResourceView> savedInput;
            Ptr<ID3D11UnorderedAccessView> savedOutput;
            context->CSGetShaderResources(0, 1, savedInput.GetAddressOf());
            context->CSGetUnorderedAccessViews(0, 1, savedOutput.GetAddressOf());
            auto* input = snapshotView.Get();
            auto* output = binding.output.Get();
            context->CSSetShaderResources(0, 1, &input);
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CSSetShader(shader.Get(), nullptr, 0);
            // Use the chained native method so the dispatch observer cannot recurse.
            dispatch(context, (binding.desc.Width + 7) / 8, (binding.desc.Height + 7) / 8, 1);
            input = savedInput.Get();
            output = savedOutput.Get();
            context->CSSetShaderResources(0, 1, &input);
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CSSetShader(savedShader.Get(), instances.data(), count);
            for (UINT i = 0; i < count; ++i) {
                if (instances[i]) {
                    instances[i]->Release();
                }
            }
            return S_OK;
        }

    private:
        Ptr<ID3D11Device> device;
        Ptr<ID3D11Texture2D> snapshot;
        Ptr<ID3D11ShaderResourceView> snapshotView;
        Ptr<ID3D11ComputeShader> shader;
        Ptr<ID3D11Texture2D> nativeUi;
        Ptr<ID3D11Buffer> nativeConstants;
        HRESULT shaderStatus = E_PENDING;
        bool ready{};
    };
}
