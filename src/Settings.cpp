// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "Settings.h"

namespace load_progress
{
    // Returns the settings instance loaded before the transition hooks are installed.
    Settings& Settings::GetSingleton()
    {
        static Settings singleton;
        return singleton;
    }

    // Loads the user configuration, retaining safe defaults when the file or a value is invalid.
    void Settings::Load()
    {
        warmTransition = {};
        defaultColdTransition = {};
        cellRules.clear();
        blurEnabled = true;
        blurAmount = defaultBlurAmount;
        progressBar = {};
        loadingLoggingEnabled = false;
        verboseQueueLoggingEnabled = false;
        loadedEntryLogging = {};

        const auto      path = GetConfigPath();
        std::error_code pathError;
        const bool      fileExists = std::filesystem::exists(path, pathError);
        if (pathError) {
            logger::error("could not inspect settings path {}: {}; using defaults", path.string(), pathError.message());
            return;
        }
        if (!fileExists) {
            logger::warn("settings file not found at {}; using defaults", path.string());
            return;
        }

        try {
            const auto document = toml::parse(path);

            if (document.contains("logging")) {
                const auto& logging = toml::find(document, "logging");
                loadingLoggingEnabled =
                    toml::find_or<bool>(logging, "loading", loadingLoggingEnabled);
                verboseQueueLoggingEnabled =
                    toml::find_or<bool>(logging, "verbose_queues", verboseQueueLoggingEnabled);

                if (logging.contains("loaded_entries")) {
                    const auto& entries = toml::find(logging, "loaded_entries");
                    loadedEntryLogging.objectReferences = toml::find_or<bool>(
                        entries, "object_references", loadedEntryLogging.objectReferences);
                    loadedEntryLogging.transferredReferences = toml::find_or<bool>(
                        entries, "transferred_references", loadedEntryLogging.transferredReferences);
                    loadedEntryLogging.distantReferences = toml::find_or<bool>(
                        entries, "distant_references", loadedEntryLogging.distantReferences);
                }
            }

            if (document.contains("blur")) {
                const auto& blur = toml::find(document, "blur");
                blurEnabled = toml::find_or<bool>(blur, "enabled", blurEnabled);
                blurAmount = std::clamp(
                    toml::find_or<float>(blur, "amount", blurAmount), 0.0F, maximumBlurAmount);
            }

            if (document.contains("progress_bar")) {
                const auto& meter = toml::find(document, "progress_bar");
                progressBar.enabled = toml::find_or<bool>(meter, "enabled", progressBar.enabled);
                progressBar.xPercent = ReadPercent(meter, "x_percent", progressBar.xPercent);
                progressBar.yPercent = ReadPercent(meter, "y_percent", progressBar.yPercent);
                progressBar.widthPercent = std::max(1.0,
                    ReadPercent(meter, "width_percent", progressBar.widthPercent, maximumMeterWidthPercent));
            }

            if (document.contains("warm")) {
                const auto& warm = toml::find(document, "warm");
                warmTransition.holdAfterLoad =
                    ReadDuration(warm, "hold_after_load_ms", warmTransition.holdAfterLoad);
                warmTransition.fadeOut = ReadDuration(warm, "fade_out_ms", warmTransition.fadeOut);
            }

            if (document.contains("cold")) {
                defaultColdTransition =
                    ReadColdTransition(toml::find(document, "cold"), defaultColdTransition);
            }

            ReadCellRules(document);
            logger::info("loaded settings from {} with {} cell transition rule(s); blur={} amount={:.2f}",
                path.string(), cellRules.size(), IsBlurEnabled(), blurAmount);
            logger::info("loading meter: enabled={} x={:.1f}% y={:.1f}% width={:.1f}%",
                progressBar.enabled, progressBar.xPercent, progressBar.yPercent,
                progressBar.widthPercent);
            logger::info("loading diagnostics: enabled={} verbose queues={}",
                loadingLoggingEnabled, IsVerboseQueueLoggingEnabled());
            logger::info("loaded-entry diagnostics: objects={} transfers={} distant={}",
                loadingLoggingEnabled && loadedEntryLogging.objectReferences,
                loadingLoggingEnabled && loadedEntryLogging.transferredReferences,
                loadingLoggingEnabled && loadedEntryLogging.distantReferences);
        } catch (const std::exception& error) {
            logger::error("could not load settings from {}: {}; using defaults", path.string(), error.what());
            warmTransition = {};
            defaultColdTransition = {};
            cellRules.clear();
            blurEnabled = true;
            blurAmount = defaultBlurAmount;
            progressBar = {};
            loadingLoggingEnabled = false;
            verboseQueueLoggingEnabled = false;
            loadedEntryLogging = {};
        }
    }

    // Returns the global timing used for resident-cell transitions.
    const Settings::WarmTransition& Settings::GetWarmTransition() const
    {
        return warmTransition;
    }

    // Returns the fallback used by cold cells that do not match a rule.
    const Settings::ColdTransition& Settings::GetDefaultColdTransition() const
    {
        return defaultColdTransition;
    }

    // Returns the first cold transition rule matching the destination editor ID.
    const Settings::ColdTransition& Settings::GetColdTransition(std::string_view a_editorID) const
    {
        for (const auto& rule : cellRules) {
            if (MatchesPattern(a_editorID, rule.pattern)) {
                if (IsLoadingLoggingEnabled()) {
                    logger::info(
                        "cell transition rule matched: editorID='{}' pattern='{}'", a_editorID, rule.pattern);
                }
                return rule.transition;
            }
        }

        return defaultColdTransition;
    }

    // Returns whether captured frames should pass through the blur shader.
    bool Settings::IsBlurEnabled() const
    {
        return blurEnabled && blurAmount > 0.0F;
    }

    // Returns the configured shader sample radius in render-target pixels.
    float Settings::GetBlurAmount() const
    {
        return blurAmount;
    }

    // Returns the safe-zone placement and active-skin width scale for the loading meter.
    const Settings::ProgressBar& Settings::GetProgressBar() const
    {
        return progressBar;
    }

    // Returns whether per-load summaries and transition diagnostics may be written.
    bool Settings::IsLoadingLoggingEnabled() const
    {
        return loadingLoggingEnabled;
    }

    // Verbose queue samples require both logging switches to avoid accidental file spam.
    bool Settings::IsVerboseQueueLoggingEnabled() const
    {
        return loadingLoggingEnabled && verboseQueueLoggingEnabled;
    }

    // Returns the individual reference categories selected for diagnostic output.
    const Settings::LoadedEntryLogging& Settings::GetLoadedEntryLogging() const
    {
        return loadedEntryLogging;
    }

    // Loaded-entry diagnostics also honor the master loading-log switch.
    bool Settings::IsLoadedEntryLoggingEnabled() const
    {
        return loadingLoggingEnabled &&
               (loadedEntryLogging.objectReferences || loadedEntryLogging.transferredReferences ||
                   loadedEntryLogging.distantReferences);
    }

    // Resolves the conventional SKSE plugin configuration path beside the DLL.
    std::filesystem::path Settings::GetConfigPath()
    {
        auto path = std::filesystem::path(REL::Module::get().filePath()).parent_path();
        path /= "Data/SKSE/Plugins/SkyrimLoadProgress.toml";
        return path;
    }

    // Reads a non-negative millisecond duration and places an upper bound on accidental stalls.
    std::chrono::milliseconds Settings::ReadDuration(
        const toml::value&        a_table,
        std::string_view          a_key,
        std::chrono::milliseconds a_default)
    {
        const auto value = toml::find_or<std::int64_t>(a_table, std::string(a_key), a_default.count());
        return std::chrono::milliseconds(
            std::clamp(value, std::int64_t{ 0 }, maximumDurationMilliseconds));
    }

    // Applies one TOML transition table over an inherited cold-transition definition.
    Settings::ColdTransition Settings::ReadColdTransition(
        const toml::value& a_table, ColdTransition a_default)
    {
        const auto type = ToLower(toml::find_or<std::string>(a_table, "transition", ""));
        if (type == "blur") {
            a_default.type = TransitionType::blur;
        } else if (type == "color") {
            a_default.type = TransitionType::color;
        } else if (!type.empty()) {
            throw std::runtime_error(fmt::format("unknown transition type '{}'", type));
        }

        const auto color = toml::find_or<std::string>(a_table, "color", "");
        if (!color.empty()) {
            if (ToLower(color) == "dominant") {
                a_default.colorSource = ColorSource::dominant;
            } else {
                a_default.colorSource = ColorSource::fixed;
                a_default.color = ReadColor(color);
            }
        }

        const auto fallbackColor = toml::find_or<std::string>(a_table, "fallback_color", "");
        if (!fallbackColor.empty()) {
            a_default.color = ReadColor(fallbackColor);
        }

        a_default.fadeIn = ReadDuration(a_table, "fade_in_ms", a_default.fadeIn);
        a_default.holdAfterLoad =
            ReadDuration(a_table, "hold_after_load_ms", a_default.holdAfterLoad);
        a_default.fadeOut = ReadDuration(a_table, "fade_out_ms", a_default.fadeOut);
        return a_default;
    }

    // Converts #RRGGBB or RRGGBB text into the packed color used by the compositor.
    std::uint32_t Settings::ReadColor(std::string_view a_value)
    {
        if (!a_value.empty() && a_value.front() == '#') {
            a_value.remove_prefix(1);
        }
        if (a_value.size() != 6 || !std::ranges::all_of(a_value, [](char character) {
                return std::isxdigit(static_cast<unsigned char>(character)) != 0;
            })) {
            throw std::runtime_error(fmt::format("invalid transition color '{}'", a_value));
        }

        std::uint32_t color = 0;
        const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), color, 16);
        if (error != std::errc{} || end != a_value.data() + a_value.size()) {
            throw std::runtime_error(fmt::format("invalid transition color '{}'", a_value));
        }
        return color;
    }

    // Reads and clamps a percentage used by the loading meter layout.
    double Settings::ReadPercent(
        const toml::value& a_table, std::string_view a_key, double a_default, double a_maximum)
    {
        const auto value = toml::find_or<double>(a_table, std::string(a_key), a_default);
        return std::clamp(value, 0.0, a_maximum);
    }

    // Matches editor IDs case-insensitively with '*' and '?' wildcard support.
    bool Settings::MatchesPattern(std::string_view a_text, std::string_view a_pattern)
    {
        std::size_t textIndex = 0;
        std::size_t patternIndex = 0;
        std::size_t starIndex = std::string_view::npos;
        std::size_t starTextIndex = 0;

        while (textIndex < a_text.size()) {
            if (patternIndex < a_pattern.size() &&
                (a_pattern[patternIndex] == '?' || ToLower(a_pattern[patternIndex]) == ToLower(a_text[textIndex]))) {
                ++textIndex;
                ++patternIndex;
            } else if (patternIndex < a_pattern.size() && a_pattern[patternIndex] == '*') {
                starIndex = patternIndex++;
                starTextIndex = textIndex;
            } else if (starIndex != std::string_view::npos) {
                patternIndex = starIndex + 1;
                textIndex = ++starTextIndex;
            } else {
                return false;
            }
        }

        while (patternIndex < a_pattern.size() && a_pattern[patternIndex] == '*') {
            ++patternIndex;
        }
        return patternIndex == a_pattern.size();
    }

    // Performs locale-independent case folding for editor IDs and setting names.
    char Settings::ToLower(char a_character)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(a_character)));
    }

    // Returns a lower-case copy used for TOML enum values.
    std::string Settings::ToLower(std::string a_value)
    {
        std::ranges::transform(a_value, a_value.begin(), [](char character) { return ToLower(character); });
        return a_value;
    }

    // Reads ordered cell rules; the first wildcard pattern that matches wins.
    void Settings::ReadCellRules(const toml::value& a_document)
    {
        if (!a_document.contains("cell_rules")) {
            return;
        }

        const auto& rules = toml::find(a_document, "cell_rules");
        if (!rules.is_array()) {
            throw std::runtime_error("cell_rules must be an array of tables");
        }

        for (const auto& ruleTable : rules.as_array()) {
            const auto pattern = toml::find_or<std::string>(ruleTable, "pattern", "");
            if (pattern.empty()) {
                throw std::runtime_error("each cell rule requires a non-empty pattern");
            }

            cellRules.push_back({ pattern, ReadColdTransition(ruleTable, defaultColdTransition) });
        }
    }
}
