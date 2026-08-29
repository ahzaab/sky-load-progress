// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "FormResolver.h"

namespace load_progress
{
    // Finds powerofthree's optional Editor-ID service without creating a plugin dependency.
    FormResolver::GetFormEditorID_t FormResolver::ResolveEditorIDExport() noexcept
    {
        // Credit to powerofthree for exposing this lookup to other SKSE plugins. We discover the
        // service dynamically so Skyrim Load Progress never links against or requires Tweaks.
        static const auto editorIDExport = []() noexcept -> GetFormEditorID_t {
            const auto module = REX::W32::GetModuleHandleW(L"po3_Tweaks.dll");
            if (!module) {
                return nullptr;
            }

            return reinterpret_cast<GetFormEditorID_t>(
                REX::W32::GetProcAddress(module, "GetFormEditorID"));
        }();

        return editorIDExport;
    }

    // Uses Skyrim's retained Editor ID first, then the optional exported lookup when available.
    std::string FormResolver::GetEditorID(const RE::TESForm* a_form)
    {
        if (!a_form) {
            return {};
        }

        const auto* nativeEditorID = a_form->GetFormEditorID();
        if (nativeEditorID && nativeEditorID[0] != '\0') {
            return nativeEditorID;
        }

        const auto editorIDExport = ResolveEditorIDExport();
        if (!editorIDExport) {
            return {};
        }

        const auto* externalEditorID = editorIDExport(a_form->GetFormID());
        return externalEditorID ? externalEditorID : "";
    }

    // Returns a useful display name even when the form has no retained Editor ID.
    std::string FormResolver::GetName(const RE::TESForm* a_form)
    {
        if (!a_form) {
            return {};
        }

        const char* name = nullptr;
        if (const auto* reference = a_form->As<RE::TESObjectREFR>()) {
            name = reference->GetName();
        } else {
            name = a_form->GetName();
        }

        return name ? name : "";
    }

    // Builds a compact log section while omitting unavailable optional text fields.
    std::string FormResolver::Describe(
        std::uint32_t a_formID, const RE::TESForm* a_form)
    {
        const auto formID = a_form ? a_form->GetFormID() : a_formID;
        auto       description = fmt::format("{:08X}", formID);
        if (!a_form) {
            return description;
        }

        const auto type = RE::FormTypeToString(a_form->GetFormType());
        if (!type.empty()) {
            fmt::format_to(std::back_inserter(description), " {}", type);
        }

        const auto editorID = GetEditorID(a_form);
        if (!editorID.empty()) {
            fmt::format_to(std::back_inserter(description), " editorID='{}'", editorID);
        }

        const auto name = GetName(a_form);
        if (!name.empty()) {
            fmt::format_to(std::back_inserter(description), " name='{}'", name);
        }

        return description;
    }

    // Reports whether the optional provider was present when the resolver first checked.
    bool FormResolver::HasExternalEditorIDProvider() noexcept
    {
        return ResolveEditorIDExport() != nullptr;
    }
}
