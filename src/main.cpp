// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "CellTransitioner.h"
#include "LoadingProgress.h"
#include "Settings.h"
#include "Version.h"

namespace
{
    // Logs a fatal plugin error without allowing the logging backend to unwind through SKSE.
    void LogCritical(std::string_view a_message, std::string_view a_detail = {}) noexcept
    {
        try {
            if (a_detail.empty()) {
                logger::critical("{}", a_message);
            } else {
                logger::critical("{}: {}", a_message, a_detail);
            }
        } catch (...) {
            REX::W32::OutputDebugStringA("Skyrim Load Progress: fatal plugin error\n");
        }
    }

#ifndef NDEBUG
    // Opens a console sink for local Debug builds.
    bool OpenDebugConsole()
    {
        if (!GetConsoleWindow() && !AllocConsole()) {
            return false;
        }

        FILE* stream = nullptr;
        if (freopen_s(&stream, "CONOUT$", "w", stdout) != 0) {
            return false;
        }
        SetConsoleTitleW(L"Skyrim Load Progress - Debug");
        return true;
    }

#endif

    // Creates the file sink and the additional Debug-only logging sinks.
    bool InitializeLog()
    {
        auto path = logger::log_directory();
        if (!path) {
            return false;
        }
        *path /= "SkyrimLoadProgress.log";
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

#ifndef NDEBUG
        if (!OpenDebugConsole()) {
            return false;
        }

        auto msvc = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto log = std::make_shared<spdlog::logger>(
            "global", spdlog::sinks_init_list{ file, msvc, console });
        log->set_level(spdlog::level::trace);
        log->flush_on(spdlog::level::trace);
#else
        auto log = std::make_shared<spdlog::logger>("global", std::move(file));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
#endif

        spdlog::set_default_logger(std::move(log));
        return true;
    }

    // Defers hook installation until Skyrim has finished loading game data.
    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (a_message && a_message->type == SKSE::MessagingInterface::kDataLoaded) {
            try {
                load_progress::Settings::GetSingleton().Load();
                load_progress::transitions::InstallHooks();
                load_progress::InstallHooks();
            } catch (const std::exception& error) {
                load_progress::LoadingProgress::DisableHooks(error.what());
                load_progress::CellTransitioner::DisableHooks(error.what());
                LogCritical("could not install Skyrim Load Progress hooks", error.what());
            } catch (...) {
                load_progress::LoadingProgress::DisableHooks("unknown hook-installation exception");
                load_progress::CellTransitioner::DisableHooks("unknown hook-installation exception");
                LogCritical("could not install Skyrim Load Progress hooks: unknown exception");
            }
        }
    }
}

// Initializes SKSE, logging, and the data-loaded listener.
SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    try {
        if (!a_skse) {
            return false;
        }

        SKSE::Init(a_skse);

        if (!InitializeLog()) {
            return false;
        }

        logger::info("Skyrim Load Progress {} loading", Version::NAME);

        // Reserve enough room for the six context-capture stubs and every branch island.
        constexpr std::size_t trampolineSize = 1 << 14;
        SKSE::AllocTrampoline(trampolineSize);

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener("SKSE", MessageHandler)) {
            logger::critical("could not register SKSE message listener");
            return false;
        }

        logger::info("Skyrim Load Progress loaded");
        return true;
    } catch (const std::exception& e) {
        LogCritical(e.what());
        return false;
    } catch (...) {
        LogCritical("unknown exception during plugin initialization");
        return false;
    }
}
