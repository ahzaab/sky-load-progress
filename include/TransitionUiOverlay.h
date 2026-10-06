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

        /**
         * @brief Invalidates the paired UI-conversion snapshot for the next presentation.
         */
        void BeginPresent() noexcept
        {
            ready = false;
            nativeUi.Reset();
            nativeConstants.Reset();
        }

        /**
         * @brief Releases the retained display snapshot and its native UI associations.
         */
        void ReleaseTextures() noexcept
        {
            BeginPresent();
            snapshot.Reset();
            snapshotView.Reset();
        }

        /**
         * @brief Releases all overlay resources and clears cached shader initialization status.
         */
        void Reset() noexcept
        {
            ReleaseTextures();
            shader.Reset();
            device.Reset();
            shaderStatus = E_PENDING;
        }

        /**
         * @brief Compiles and creates the overlay compute shader for the supplied device.
         *
         * @return Cached shader initialization status, or E_POINTER for a null device.
         */
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

            // t0 is a read-only shader-resource view (SRV) of the captured display image; u0 is the
            // read/write UAV for native UI. Each 8x8 thread group covers 64 output pixels.
            // SV_DispatchThreadID supplies the global pixel coordinate; the bounds test protects
            // edge groups when width or height is not a multiple of eight.
            // Native UI RGB is already premultiplied by its alpha: UI + scene * (1 - UI alpha)
            // places the photograph underneath it. Output alpha 1 makes that cover fully opaque.
            // Both inputs already use native display encoding; this shader performs no HDR conversion.
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

        /**
         * @brief Classifies the bound compute resources as a matching native display or UI conversion
         * pass.
         */
        Binding Inspect(ID3D11DeviceContext* context, UINT width, UINT height) const noexcept
        {
            // Identify passes from the resources currently bound to the compute stage (CS).
            // A UAV is a writable view, while its underlying texture owns the actual image storage.
            // Unknown layouts return Pass::none so unrelated compute work passes through untouched.
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

            // Matching dimensions alone are insufficient: other compute passes use similar textures.
            // Require both the captured UI texture and native CB0 identity within this same Present.
            if (ready && result.texture.Get() == nativeUi.Get() &&
                result.constants.Get() == nativeConstants.Get() &&
                result.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {

                result.pass = Pass::ui;
                return result;
            }

            // The native display output uses 10-bit RGB; its input scene uses 16-bit float RGBA,
            // and the separate UI texture uses 8-bit normalized RGBA. These formats identify this
            // integration's resource contract; formats alone do not describe an image's color space.
            if (result.desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
                return result;
            }

            std::array<Ptr<ID3D11ShaderResourceView>, 2> inputs;
            std::array<ID3D11ShaderResourceView*, 2> raw{};
            context->CSGetShaderResources(0, 2, raw.data());
            for (size_t i = 0; i < raw.size(); ++i) {
                // CSGetShaderResources adds COM references. Attach adopts those references without
                // adding another one; ComPtr then releases them when this inspection ends.
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

        /**
         * @brief Snapshots a native display conversion for composition by its paired UI conversion.
         *
         * @return S_OK on capture, S_FALSE for a non-display pass, or a resource creation failure.
         */
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
                // The snapshot keeps the display output's dimensions and format but only needs SRV access.
                // DEFAULT means GPU-owned storage here; CPUAccessFlags=0 avoids CPU mapping/readback.
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

            // Temporarily remove the writable binding while copying the completed native output.
            // Restore it immediately; CopyResource duplicates bytes and performs no format conversion.
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

        /**
         * @brief Composites the captured display beneath native UI and restores the compute pipeline
         * bindings.
         *
         * @return S_OK on composition, or S_FALSE when no matching paired UI conversion is available.
         */
        HRESULT Compose(ID3D11DeviceContext* context, const Binding& binding, Dispatch dispatch) noexcept
        {
            if (!ready || binding.pass != Pass::ui || !dispatch ||
                binding.texture.Get() != nativeUi.Get() || binding.constants.Get() != nativeConstants.Get()) {
                return S_FALSE;
            }

            // Consume this pairing once. Reusing it for another dispatch could cover a different frame
            // or apply the display snapshot to an unrelated UI conversion.
            ready = false;
            Ptr<ID3D11ComputeShader> savedShader;
            std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
            UINT count = static_cast<UINT>(instances.size());
            context->CSGetShader(savedShader.GetAddressOf(), instances.data(), &count);
            Ptr<ID3D11ShaderResourceView> savedInput;
            Ptr<ID3D11UnorderedAccessView> savedOutput;
            context->CSGetShaderResources(0, 1, savedInput.GetAddressOf());
            context->CSGetUnorderedAccessViews(0, 1, savedOutput.GetAddressOf());
            // Bind our input at SRV0/t0 and output at UAV0/u0, matching the HLSL register declarations.
            // The native constant buffers are not used or changed by this composition shader.
            auto* input = snapshotView.Get();
            auto* output = binding.output.Get();
            context->CSSetShaderResources(0, 1, &input);
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CSSetShader(shader.Get(), nullptr, 0);
            // Use the chained native method so the dispatch observer cannot recurse.
            // Round group counts upward to cover every pixel; the shader rejects out-of-range threads.
            // Restore the saved shader and resource views after dispatch so native rendering can continue.
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
