// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/ContextHook.h>
#include <CommonStates.h>
#include <SpriteBatch.h>
#include <spdlog/sinks/basic_file_sink.h>
#ifndef NDEBUG
#    include <spdlog/sinks/msvc_sink.h>
#    include <spdlog/sinks/stdout_color_sinks.h>
#endif

using namespace std::literals;
namespace logger = SKSE::log;
