#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>
#ifndef NDEBUG
#    include <spdlog/sinks/msvc_sink.h>
#    include <spdlog/sinks/stdout_color_sinks.h>
#endif

using namespace std::literals;
namespace logger = SKSE::log;

