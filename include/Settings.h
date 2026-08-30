// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include <toml.hpp>

namespace load_progress
{
    class Settings final
    {
    public:
        enum class TransitionType : std::uint8_t
        {
            blur,
            color
        };
        enum class ColorSource : std::uint8_t
        {
            fixed,
            dominant
        };

        struct WarmTransition
        {
            std::chrono::milliseconds holdAfterLoad{ 0 };
            std::chrono::milliseconds fadeOut{ 1000 };
        };

        struct ColdTransition
        {
            TransitionType            type{ TransitionType::blur };
            ColorSource               colorSource{ ColorSource::dominant };
            std::uint32_t             color{ 0xFFFFFF };
            std::chrono::milliseconds fadeIn{ 750 };
            std::chrono::milliseconds holdAfterLoad{ 250 };
            std::chrono::milliseconds fadeOut{ 1000 };
        };

        struct CellRule
        {
            std::string    pattern;
            ColdTransition transition;
        };

        struct ProgressBar
        {
            bool   enabled{ true };
            double xPercent{ 50.0 };
            double yPercent{ 25.0 };
            double widthPercent{ 100.0 };
        };

        struct LoadedEntryLogging
        {
            bool objectReferences{ false };
            bool transferredReferences{ false };
            bool distantReferences{ false };
        };

        static Settings& GetSingleton();

        void Load();

        [[nodiscard]] const WarmTransition& GetWarmTransition() const;
        [[nodiscard]] const ColdTransition& GetDefaultColdTransition() const;
        [[nodiscard]] const ColdTransition& GetColdTransition(std::string_view a_editorID) const;
        [[nodiscard]] bool                  IsBlurEnabled() const;
        [[nodiscard]] float                 GetBlurAmount() const;
        [[nodiscard]] const ProgressBar&    GetProgressBar() const;
        [[nodiscard]] bool                  IsLoadingLoggingEnabled() const;
        [[nodiscard]] bool                  IsVerboseQueueLoggingEnabled() const;
        [[nodiscard]] const LoadedEntryLogging& GetLoadedEntryLogging() const;
        [[nodiscard]] bool                      IsLoadedEntryLoggingEnabled() const;

    private:
        // Limits keep malformed configuration from holding the transition indefinitely.
        static constexpr std::int64_t maximumDurationMilliseconds = 600000;
        static constexpr float        defaultBlurAmount = 2.0F;
        static constexpr float        maximumBlurAmount = 20.0F;
        static constexpr double       maximumMeterWidthPercent = 1000.0;

        static std::filesystem::path     GetConfigPath();
        static std::chrono::milliseconds ReadDuration(
            const toml::value& a_table, std::string_view a_key, std::chrono::milliseconds a_default);
        static ColdTransition ReadColdTransition(const toml::value& a_table, ColdTransition a_default);
        static std::uint32_t  ReadColor(std::string_view a_value);
        static double         ReadPercent(
                    const toml::value& a_table, std::string_view a_key, double a_default, double a_maximum = 100.0);
        static bool        MatchesPattern(std::string_view a_text, std::string_view a_pattern);
        static char        ToLower(char a_character);
        static std::string ToLower(std::string a_value);
        void               ReadCellRules(const toml::value& a_document);

        WarmTransition        warmTransition;
        ColdTransition        defaultColdTransition;
        std::vector<CellRule> cellRules;
        bool                  blurEnabled{ true };
        float                 blurAmount{ defaultBlurAmount };
        ProgressBar           progressBar;
        bool                  loadingLoggingEnabled{ false };
        bool                  verboseQueueLoggingEnabled{ false };
        LoadedEntryLogging    loadedEntryLogging;

        Settings() = default;
        Settings(const Settings&) = delete;
        Settings(Settings&&) = delete;
        Settings& operator=(const Settings&) = delete;
        Settings& operator=(Settings&&) = delete;
    };
}
