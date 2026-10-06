#pragma once

#include <array>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace load_progress
{
    // HDROutputCS's published PerFrame interface uses three float4s, but CS's
    // ConstantBufferDesc rounds that 48-byte payload up to a 64-byte allocation.
    // Copy the full allocation and preserve its fourth register as opaque data.
    // A retained scene must keep its scene encoding and AutoHDR treatment even
    // when Effects11 did not run on the current loading frame. UI/FG policy,
    // brightness and display settings continue to come from the native frame.
    class RetainedHdrConversion
    {
        template <class T>
        using Ptr = Microsoft::WRL::ComPtr<T>;
        static constexpr UINT nativeBufferBytes = 64;

    public:
        using Dispatch = void (*)(ID3D11DeviceContext*, UINT, UINT, UINT);

        struct DiagnosticState
        {
            const ID3D11Texture2D* capturedSource{};
            HRESULT initialization = E_PENDING;
            UINT constantBytes{};
            bool compatible{};
        };

        /**
         * @brief Inspects captured-source and constant-buffer compatibility without GPU synchronization.
         *
         * @note Callers must gate inspection on debugging.loading.
         */
        DiagnosticState InspectDiagnostics(ID3D11Buffer* constants) const noexcept
        {
            DiagnosticState result{ source.Get(), status };
            if (constants) {

                D3D11_BUFFER_DESC desc{};
                constants->GetDesc(&desc);
                result.constantBytes = desc.ByteWidth;
                result.compatible = desc.ByteWidth == nativeBufferBytes &&
                    (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
            }

            return result;
        }

        /**
         * @brief Discards the captured scene association so retained conversion cannot be applied.
         */
        void Invalidate() noexcept
        {
            source.Reset();
        }

        /**
         * @brief Creates conversion buffers and the merge shader for the supplied device.
         *
         * @return Cached initialization status, or E_POINTER for a null device.
         */
        HRESULT Initialize(ID3D11Device* currentDevice) noexcept
        {
            if (!currentDevice) {
                return E_POINTER;
            }

            if (device.Get() != currentDevice) {

                Invalidate();
                device = currentDevice;
                captured.Reset();
                merged.Reset();
                scratch.Reset();
                scratchView.Reset();
                shader.Reset();
                status = E_PENDING;
            }

            if (status != E_PENDING) {
                return status;
            }

            // A constant buffer (CB) supplies small read-only parameter blocks to a shader.
            // We retain one copy and prepare a separate merged CB, leaving native storage untouched.
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = nativeBufferBytes;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            status = device->CreateBuffer(&desc, nullptr, captured.GetAddressOf());
            if (FAILED(status)) return status;
            status = device->CreateBuffer(&desc, nullptr, merged.GetAddressOf());
            if (FAILED(status)) return status;
            // Compute shaders write through an unordered-access view (UAV), not a CB binding.
            // The scratch buffer is writable; its finished bytes are copied into the read-only merged CB.
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            status = device->CreateBuffer(&desc, nullptr, scratch.GetAddressOf());
            if (FAILED(status)) return status;
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = nativeBufferBytes / sizeof(UINT);
            view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            status = device->CreateUnorderedAccessView(scratch.Get(), &view, scratchView.GetAddressOf());
            if (FAILED(status)) return status;
            // HLSL register names are binding slots: b0 = live CB, b1 = captured CB, u0 = scratch UAV.
            // Each float4 occupies 16 bytes. The shader replaces only isSceneLinear (c1.y) and
            // applyAutoHDR (c2.y); asuint preserves float bit patterns rather than converting values.
            // One GPU thread writes all four registers, including the opaque fourth-register padding.
            constexpr char code[] = R"(
cbuffer Live : register(b0) { float4 live0; float4 live1; float4 live2; uint4 livePadding; }
cbuffer Photograph : register(b1) { float4 photo0; float4 photo1; float4 photo2; }
RWByteAddressBuffer Output : register(u0);
[numthreads(1, 1, 1)] void main(uint3 p : SV_DispatchThreadID)
{
    Output.Store4(0, asuint(live0));
    Output.Store4(16, asuint(float4(live1.x, photo1.y, live1.zw)));
    Output.Store4(32, asuint(float4(live2.x, photo2.y, live2.zw)));
    Output.Store4(48, livePadding);
}
)";
            // D3DCompile produces shader bytecode; CreateComputeShader makes a device-owned object.
            // cs_5_0 selects a Shader Model 5 compute shader, which runs through Dispatch rather than Draw.
            Ptr<ID3DBlob> compiled;
            status = D3DCompile(code, sizeof(code) - 1, "RetainedHdrConversion", nullptr, nullptr,
                "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, compiled.GetAddressOf(), nullptr);
            if (SUCCEEDED(status)) {
                status = device->CreateComputeShader(compiled->GetBufferPointer(), compiled->GetBufferSize(),
                    nullptr, shader.GetAddressOf());
            }

            return status;
        }

        /**
         * @brief Captures compatible native HDR constants and associates them with the retained scene.
         *
         * @return S_OK on capture, S_FALSE for incompatible inputs, or an initialization failure.
         */
        HRESULT Capture(ID3D11DeviceContext* context, ID3D11Buffer* constants, ID3D11Texture2D* scene) noexcept
        {
            if (!Compatible(constants) || !scene) {

                Invalidate();
                return S_FALSE;
            }

            Ptr<ID3D11Device> currentDevice;
            context->GetDevice(currentDevice.GetAddressOf());
            auto result = Initialize(currentDevice.Get());
            if (FAILED(result)) return result;
            // CopyResource queues a GPU copy; it does not download constants to CPU memory.
            // Remembering the scene identity prevents applying this capture to a different photograph.
            context->CopyResource(captured.Get(), constants);
            source = scene;
            return S_OK;
        }

        /**
         * @brief Merges retained scene encoding with live display settings and binds the merged constants
         * at CB0.
         *
         * @return S_OK when CB0 is overridden, or S_FALSE when inputs do not match the capture.
         * @note Restore the original CB0 immediately after the native HDR dispatch.
         */
        HRESULT Apply(ID3D11DeviceContext* context, ID3D11Buffer* constants,
            ID3D11Texture2D* scene, Dispatch dispatch) noexcept
        {
            if (!source || source.Get() != scene || !Compatible(constants) || !dispatch) {
                return S_FALSE;
            }

            // The device context is shared mutable engine state. Save each binding our merge pass
            // touches so the next native dispatch sees its original shader, UAV, and CB1.
            Ptr<ID3D11ComputeShader> savedShader;
            std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
            UINT count = static_cast<UINT>(instances.size());
            context->CSGetShader(savedShader.GetAddressOf(), instances.data(), &count);
            Ptr<ID3D11UnorderedAccessView> savedOutput;
            Ptr<ID3D11Buffer> savedPhotograph;
            context->CSGetUnorderedAccessViews(0, 1, savedOutput.GetAddressOf());
            context->CSGetConstantBuffers(1, 1, savedPhotograph.GetAddressOf());
            ID3D11Buffer* inputs[]{ constants, captured.Get() };
            auto* output = scratchView.Get();
            context->CSSetConstantBuffers(0, 2, inputs);
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CSSetShader(shader.Get(), nullptr, 0);
            // Dispatch arguments count thread groups. With numthreads(1,1,1), this executes one thread.
            // Use the chained native dispatch entry so our dispatch observer is not called recursively.
            dispatch(context, 1, 1, 1);
            // Unbind the writable UAV before copying its output into the constant buffer.
            // The merged CB0 deliberately remains bound for the caller's next native HDR dispatch;
            // the caller must restore native CB0 immediately afterward.
            output = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CopyResource(merged.Get(), scratch.Get());
            inputs[0] = merged.Get();
            inputs[1] = savedPhotograph.Get();
            context->CSSetConstantBuffers(0, 2, inputs);
            output = savedOutput.Get();
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->CSSetShader(savedShader.Get(), instances.data(), count);
            // CSGetShader also returns owned references to any dynamic shader class instances.
            // Restoring their bindings does not release those references, so balance them explicitly.
            for (UINT i = 0; i < count; ++i) {
                if (instances[i]) instances[i]->Release();
            }
            return S_OK;
        }

    private:
        static bool Compatible(ID3D11Buffer* constants) noexcept
        {
            if (!constants) return false;
            D3D11_BUFFER_DESC desc{};
            constants->GetDesc(&desc);
            return desc.ByteWidth == nativeBufferBytes && (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
        }

        Ptr<ID3D11Device> device;
        Ptr<ID3D11Buffer> captured;
        Ptr<ID3D11Buffer> merged;
        Ptr<ID3D11Buffer> scratch;
        Ptr<ID3D11UnorderedAccessView> scratchView;
        Ptr<ID3D11ComputeShader> shader;
        Ptr<ID3D11Texture2D> source;
        HRESULT status = E_PENDING;
    };
}
