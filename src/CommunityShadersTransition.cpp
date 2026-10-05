#include "PCH.h"
#include "CommunityShadersTransition.h"
#include "Settings.h"
#include "TransitionGenerationPolicy.h"

#include <Windows.h>
#include "NearCallRelay.h"
#include <bcrypt.h>
#include <fstream>

namespace load_progress::CommunityShadersTransition
{
    namespace
    {
        // IDA confirms RCX=this, RDX=the caller-owned 48-byte result, RAX=result.
        // This explicit lowered ABI avoids guessing C++ aggregate-return conventions.
        using BuildHDRData = void* (*)(void*, void*);
        using ScaleUI = void (*)(void*);
        using FidelityPresent = void (*)(void*, bool, bool);
        BuildHDRData originalBuild{};
        ScaleUI originalScale{};
        FidelityPresent originalFidelity{};
        SKSE::Trampoline trampoline{ "CS transition generation" };
        bool installed{};
        thread_local bool suppress{};
        thread_local bool inPresent{};
        thread_local bool diagnosticsEnabled{};
        thread_local TransitionGenerationPolicy policy;
        thread_local unsigned suppressedFrames{};
        thread_local unsigned requestedGenerationFrames{};
        thread_local bool hdrDataComposed{};
        thread_local unsigned proxyDecisionFrames{};
        thread_local unsigned hdrOverrides{};

        bool GateProxyGeneration(bool requested)
        {
            if (inPresent && suppress && diagnosticsEnabled) {
                ++proxyDecisionFrames;
                requestedGenerationFrames += requested;
                if (proxyDecisionFrames == 1) {
                    logger::info("actual CS proxy generation decision intercepted: requested={} output=false thread={} HDRDataOverrides={}",
                        requested, ::GetCurrentThreadId(), hdrOverrides);
                }
            }
            return requested && !(inPresent && suppress);
        }

        bool GateLimiter(void* self)
        {
            const auto module = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(L"CommunityShaders.dll"));
            const bool requested = reinterpret_cast<bool (*)(void*)>(module + 0x4F3610)(self);
            return requested && !(inPresent && suppress);
        }

        // The real COM proxy inlines FidelityFX::Present. Its decision is in SIL.
        // Preserve Win64 volatile GPRs, flags and XMM0..5 around the policy call,
        // then replay the two displaced instructions, relocating their RIP operand.
        void BuildProxyDecisionStub(Xbyak::CodeGenerator& code, std::uintptr_t hdrLoaded, std::uintptr_t resume)
        {
            using namespace Xbyak::util;
            code.pushfq();
            code.push(rax); code.push(rcx); code.push(rdx);
            code.push(r8); code.push(r9); code.push(r10); code.push(r11);
            code.sub(rsp, 0x80);
            for (int i = 0; i < 6; ++i) {
                code.movdqu(ptr[rsp + 0x20 + i * 16], Xbyak::Xmm(i));
            }
            code.movzx(ecx, sil);
            code.mov(rax, reinterpret_cast<std::uintptr_t>(GateProxyGeneration));
            code.call(rax);
            code.mov(sil, al);
            for (int i = 0; i < 6; ++i) {
                code.movdqu(Xbyak::Xmm(i), ptr[rsp + 0x20 + i * 16]);
            }
            code.add(rsp, 0x80);
            code.pop(r11); code.pop(r10); code.pop(r9); code.pop(r8);
            code.pop(rdx); code.pop(rcx); code.pop(rax); code.popfq();
            code.mov(rax, r15);
            code.push(rax);
            code.mov(rax, hdrLoaded);
            code.cmp(Xbyak::util::byte[rax], 0);
            code.pop(rax);
            code.jmp(ptr[rip]);
            code.dq(resume);
            code.ready();
        }

        std::string FileSHA256(HMODULE module)
        {
            std::array<wchar_t, 32768> path{};
            const auto length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) {
                return {};
            }
            std::ifstream file(std::filesystem::path(path.data()), std::ios::binary);
            if (!file) {
                return {};
            }
            BCRYPT_ALG_HANDLE algorithm{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
                return {};
            }
            BCRYPT_HASH_HANDLE hash{};
            std::array<unsigned char, 32> digest{};
            bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
            std::array<char, 65536> chunk{};
            while (ok && file) {
                file.read(chunk.data(), chunk.size());
                const auto size = file.gcount();
                if (size) {
                    ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()), static_cast<ULONG>(size), 0) >= 0;
                }
            }
            ok = ok && !file.bad() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
            if (hash) {
                BCryptDestroyHash(hash);
            }
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (!ok) {
                return {};
            }
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result;
            for (auto byte : digest) {
                result += hex[byte >> 4];
                result += hex[byte & 15];
            }
            return result;
        }

        std::uintptr_t VerifyCall(std::uintptr_t base, std::uintptr_t site, std::uintptr_t target)
        {
            const auto address = base + site;
            std::int32_t displacement{};
            std::memcpy(&displacement, reinterpret_cast<const void*>(address + 1), sizeof(displacement));
            if (*reinterpret_cast<const std::uint8_t*>(address) != 0xE8 || address + 5 + displacement != base + target) {
                throw std::runtime_error("matched CS call was already modified; coordinated FG hooks not installed");
            }
            return address;
        }

        template <std::size_t N>
        void VerifyLoadedBytes(std::uintptr_t address, const std::array<std::uint8_t, N>& expected)
        {
            if (std::memcmp(reinterpret_cast<const void*>(address), expected.data(), N) != 0) {
                throw std::runtime_error("loaded CS instructions do not match the verified binary");
            }
        }

        void* BuildForDisplay(void* self, void* output)
        {
            auto* result = originalBuild(self, output);
            if (inPresent && suppress) {
                // IDA and the pinned shader/source agree: skipUIComposite is float +0x0C.
                // Change only this frame's returned data, never settings or private CS state.
                constexpr float composeUI = 0.0F;
                std::memcpy(static_cast<std::byte*>(result) + 12, &composeUI, sizeof(composeUI));
                if (diagnosticsEnabled) {
                    hdrDataComposed = true;
                    ++hdrOverrides;
                }
            }
            return result;
        }

        void ScaleForDisplay(void* self)
        {
            // HDR output already consumed gamma UI. Do not PQ-convert it for FFX.
            if (!(inPresent && suppress)) {
                originalScale(self);
            }
        }

        void PresentForDisplay(void* self, bool requested, bool hdr)
        {
            if (inPresent && suppress && diagnosticsEnabled) {
                ++suppressedFrames;
                requestedGenerationFrames += requested;
                if (!hdrDataComposed && suppressedFrames == 1) {
                    logger::warn("CS transition FG gate did not observe HDR display composition on its first frame");
                }
            }
            // CS handles disable/re-enable configuration and UI resource registration.
            // HDR, frame IDs, fences, swap-chain ownership, and Present all remain native.
            originalFidelity(self, requested && !(inPresent && suppress), hdr);
        }
    }

    void Install()
    {
        const auto module = ::GetModuleHandleW(L"CommunityShaders.dll");
        if (!module) {
            return;
        }
        constexpr std::string_view supportedHash = "BDF655FB2CCA157C3BD8DE9306C9B70F25120BA1E2777F60843C6389BF2BB88F";
        if (FileSHA256(module) != supportedHash) {
            logger::warn("CS transition FG gate unavailable: CommunityShaders.dll is not the verified 1.8.4 binary");
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        // Validate ALL sites before changing any. The private-module trampoline must be near CS.
        std::uintptr_t build{}, scale{}, present{};
        try {
            VerifyLoadedBytes(base + 0x44AF30, std::array<std::uint8_t, 16>{ 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x05, 0xC6, 0x6D });
            VerifyLoadedBytes(base + 0x4F4164, std::array<std::uint8_t, 10>{ 0x49, 0x8B, 0xC7, 0x80, 0x3D, 0x9A, 0x83, 0xEA, 0x00, 0x00 });
            VerifyCall(base, 0x4F4BB0, 0x4F3610);
            VerifyLoadedBytes(base + 0x44B330, std::array<std::uint8_t, 16>{ 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xF1, 0x48, 0x8B });
            VerifyLoadedBytes(base + 0x4F7210, std::array<std::uint8_t, 16>{ 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8 });
            build = VerifyCall(base, 0x44CCFE, 0x44AF30);
            scale = VerifyCall(base, 0x4F53A2, 0x44B330);
            present = VerifyCall(base, 0x4F569F, 0x4F7210);
        } catch (const std::exception& error) {
            logger::warn("CS transition FG gate unavailable: {}", error.what());
            return;
        }
        originalBuild = reinterpret_cast<BuildHDRData>(base + 0x44AF30);
        originalScale = reinterpret_cast<ScaleUI>(base + 0x44B330);
        originalFidelity = reinterpret_cast<FidelityPresent>(base + 0x4F7210);
        constexpr std::size_t relaySize = 2048;
        const auto buildEntry = base + 0x44AF30;
        const auto proxyDecision = base + 0x4F4164;
        const auto limiter = base + 0x4F4BB0;
        const std::array sites{ buildEntry, scale, present, proxyDecision, limiter };
        auto* relay = AllocateNearCallRelay(sites, relaySize);
        if (!relay) {
            logger::warn("CS transition FG gate unavailable: no shared reachable relay allocation");
            return;
        }
        const auto relayAddress = reinterpret_cast<std::uintptr_t>(relay);
        for (auto site : sites) {
            if (!CallRelayInRange(site, relayAddress) || !CallRelayInRange(site, relayAddress + relaySize - 1)) {
                ::VirtualFree(relay, 0, MEM_RELEASE);
                logger::warn("CS transition FG gate unavailable: relay failed displacement validation");
                return;
            }
            logger::info("CS relay validated: site={:X} allocation={:X} displacement={}",
                site, relayAddress, static_cast<std::int64_t>(relayAddress) - static_cast<std::int64_t>(site + 5));
        }
        trampoline.set_trampoline(relay, relaySize, [](void* memory, std::size_t) { ::VirtualFree(memory, 0, MEM_RELEASE); });
        Xbyak::CodeGenerator originalBuildCode;
        originalBuildCode.db(reinterpret_cast<const std::uint8_t*>(buildEntry), 5);
        originalBuildCode.jmp(Xbyak::util::ptr[Xbyak::util::rip]);
        originalBuildCode.dq(buildEntry + 5);
        originalBuildCode.ready();
        originalBuild = reinterpret_cast<BuildHDRData>(trampoline.allocate(originalBuildCode));
        Xbyak::CodeGenerator decisionCode;
        BuildProxyDecisionStub(decisionCode, base + 0x139C508, proxyDecision + 10);
        const auto decisionStub = reinterpret_cast<std::uintptr_t>(trampoline.allocate(decisionCode));
        trampoline.write_branch<5>(buildEntry, BuildForDisplay);
        trampoline.write_call<5>(scale, ScaleForDisplay);
        trampoline.write_call<5>(present, PresentForDisplay);
        // Ten verified bytes are two whole instructions; the remaining five are
        // deliberately NOP-filled. CommonLib's five-byte boundary test cannot
        // represent this full replacement, so use our verified ten-byte span.
        trampoline.write_branch<5>(proxyDecision, decisionStub, true);
        constexpr std::array<std::uint8_t, 5> nops{ 0x90, 0x90, 0x90, 0x90, 0x90 };
        REL::safe_write(proxyDecision + 5, nops.data(), nops.size());
        trampoline.write_call<5>(limiter, GateLimiter);
        ::FlushInstructionCache(::GetCurrentProcess(), nullptr, 0);
        installed = true;
        logger::info("installed CS 1.8.4 actual COM proxy generation gate and shared HDR-data hook; three fresh presentations before generation resumes");
    }

    void BeginPresent(bool enabled, bool ownsImage, bool freshWorld)
    {
        diagnosticsEnabled = Settings::GetSingleton().IsLoadingLoggingEnabled();
        const bool next = policy.Begin(installed && enabled, ownsImage, freshWorld);
        if (next != suppress && diagnosticsEnabled) {
            logger::info("CS transition FG suppression={}; retainedImage={} freshWorld={} standaloneFrames={} requestedFGFrames={} proxyDecisionFrames={} HDRDataOverrides={} thread={}",
                next, ownsImage, freshWorld, suppressedFrames, requestedGenerationFrames, proxyDecisionFrames, hdrOverrides, ::GetCurrentThreadId());
        }
        if (diagnosticsEnabled && next && !suppress) {
            suppressedFrames = 0;
            requestedGenerationFrames = 0;
            proxyDecisionFrames = 0;
            hdrOverrides = 0;
        }
        suppress = next;
        inPresent = true;
        if (diagnosticsEnabled) {
            hdrDataComposed = false;
        }
    }

    void EndPresent(bool presented) noexcept
    {
        policy.Finish(presented);
        inPresent = false;
        // Retain 'suppress' until the next Begin solely for change diagnostics.
    }
}
