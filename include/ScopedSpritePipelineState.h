#pragma once

#include <array>
#include <d3d11.h>
#include <wrl/client.h>

namespace load_progress
{
    // DirectXTK SpriteBatch deliberately leaves its bindings installed. Preserve
    // every binding it changes so engine/Scaleform state caches remain accurate.
    class ScopedSpritePipelineState
    {
        template <class T>
        struct ShaderBinding
        {
            Microsoft::WRL::ComPtr<T> shader;
            std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
            UINT count = static_cast<UINT>(instances.size());

            ~ShaderBinding()
            {
                for (UINT i = 0; i < count; ++i) {
                    if (instances[i]) {
                        instances[i]->Release();
                    }
                }
            }
        };

    public:
        explicit ScopedSpritePipelineState(ID3D11DeviceContext* a_context) noexcept : context(a_context)
        {
            context->OMGetBlendState(blend.GetAddressOf(), blendFactor.data(), &sampleMask);
            context->OMGetDepthStencilState(depth.GetAddressOf(), &stencilRef);
            context->RSGetState(rasterizer.GetAddressOf());
            context->IAGetPrimitiveTopology(&topology);
            context->IAGetInputLayout(layout.GetAddressOf());
            context->IAGetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &vertexStride, &vertexOffset);
            context->IAGetIndexBuffer(indexBuffer.GetAddressOf(), &indexFormat, &indexOffset);
            context->VSGetShader(vertex.shader.GetAddressOf(), vertex.instances.data(), &vertex.count);
            context->PSGetShader(pixel.shader.GetAddressOf(), pixel.instances.data(), &pixel.count);
            context->GSGetShader(geometry.shader.GetAddressOf(), geometry.instances.data(), &geometry.count);
            context->HSGetShader(hull.shader.GetAddressOf(), hull.instances.data(), &hull.count);
            context->DSGetShader(domain.shader.GetAddressOf(), domain.instances.data(), &domain.count);
            context->VSGetConstantBuffers(0, 1, vertexConstants.GetAddressOf());
            context->PSGetSamplers(0, 1, sampler.GetAddressOf());
            context->PSGetShaderResources(0, 1, texture.GetAddressOf());

            // SpriteBatch supplies VS/PS only; inherited world shaders must not
            // consume its triangles. Restore these stages with the other state.
            context->GSSetShader(nullptr, nullptr, 0);
            context->HSSetShader(nullptr, nullptr, 0);
            context->DSSetShader(nullptr, nullptr, 0);
        }

        ~ScopedSpritePipelineState() noexcept
        {
            context->OMSetBlendState(blend.Get(), blendFactor.data(), sampleMask);
            context->OMSetDepthStencilState(depth.Get(), stencilRef);
            context->RSSetState(rasterizer.Get());
            context->IASetPrimitiveTopology(topology);
            context->IASetInputLayout(layout.Get());
            auto* vertexBinding = vertexBuffer.Get();
            context->IASetVertexBuffers(0, 1, &vertexBinding, &vertexStride, &vertexOffset);
            context->IASetIndexBuffer(indexBuffer.Get(), indexFormat, indexOffset);
            context->VSSetShader(vertex.shader.Get(), vertex.instances.data(), vertex.count);
            context->PSSetShader(pixel.shader.Get(), pixel.instances.data(), pixel.count);
            context->GSSetShader(geometry.shader.Get(), geometry.instances.data(), geometry.count);
            context->HSSetShader(hull.shader.Get(), hull.instances.data(), hull.count);
            context->DSSetShader(domain.shader.Get(), domain.instances.data(), domain.count);
            auto* constantsBinding = vertexConstants.Get();
            context->VSSetConstantBuffers(0, 1, &constantsBinding);
            auto* samplerBinding = sampler.Get();
            context->PSSetSamplers(0, 1, &samplerBinding);
            auto* textureBinding = texture.Get();
            context->PSSetShaderResources(0, 1, &textureBinding);
        }

        ScopedSpritePipelineState(const ScopedSpritePipelineState&) = delete;
        ScopedSpritePipelineState& operator=(const ScopedSpritePipelineState&) = delete;

    private:
        ID3D11DeviceContext* context;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
        std::array<float, 4> blendFactor{};
        UINT sampleMask{};
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth;
        UINT stencilRef{};
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        UINT vertexStride{}, vertexOffset{};
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        DXGI_FORMAT indexFormat{};
        UINT indexOffset{};
        ShaderBinding<ID3D11VertexShader> vertex;
        ShaderBinding<ID3D11PixelShader> pixel;
        ShaderBinding<ID3D11GeometryShader> geometry;
        ShaderBinding<ID3D11HullShader> hull;
        ShaderBinding<ID3D11DomainShader> domain;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexConstants;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
    };
}
