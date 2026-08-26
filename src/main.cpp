#include "PCH.h"
#include "LoadProgress.h"
#include "Version.h"

namespace
{
#ifndef NDEBUG
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

    bool AttachDebugConsoleSink()
    {
        auto log = spdlog::default_logger();
        if (!log) {
            return false;
        }
        log->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        log->set_level(spdlog::level::trace);
        log->flush_on(spdlog::level::trace);
        return true;
    }
#endif

    bool InitializeLog()
    {
#ifndef NDEBUG
        if (!OpenDebugConsole()) {
            return false;
        }
        auto msvc = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto log = std::make_shared<spdlog::logger>("multi_sink", spdlog::sinks_init_list{ msvc, console });
        log->set_level(spdlog::level::trace);
#else
        auto path = logger::log_directory();
        if (!path) {
            return false;
        }
        *path /= "SkyrimLoadProgress.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
#endif
        spdlog::set_default_logger(std::move(log));
        return true;
    }

    void MessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (a_message && a_message->type == SKSE::MessagingInterface::kDataLoaded) {
            load_progress::Install();
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    try {
        if (!InitializeLog()) {
            return false;
        }
        SKSE::Init(a_skse);
#ifndef NDEBUG
        if (!AttachDebugConsoleSink()) {
            return false;
        }
#endif
        logger::info("Skyrim Load Progress {} loading", Version::NAME);
        if (!SKSE::GetMessagingInterface()->RegisterListener("SKSE", MessageHandler)) {
            logger::critical("could not register SKSE message listener");
            return false;
        }
        logger::info("Skyrim Load Progress loaded");
        return true;
    } catch (const std::exception& e) {
        logger::critical("{}", e.what());
        return false;
    }
}
