// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

namespace load_progress
{
    // Resolves optional Editor IDs and user-facing names without requiring another SKSE plugin.
    class FormResolver final
    {
    public:
        [[nodiscard]] static std::string GetEditorID(const RE::TESForm* a_form);
        [[nodiscard]] static std::string GetName(const RE::TESForm* a_form);
        [[nodiscard]] static std::string Describe(
            std::uint32_t a_formID, const RE::TESForm* a_form);
        [[nodiscard]] static bool HasExternalEditorIDProvider() noexcept;

    private:
        using GetFormEditorID_t = const char* (*)(std::uint32_t);

        [[nodiscard]] static GetFormEditorID_t ResolveEditorIDExport() noexcept;

        FormResolver() = delete;
    };
}
