#include "../../include/public/Logger.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <Core/OSDef.hpp>

#ifdef PLATFORM_WIN
#include <Windows.h>
#define TRACE_COLOR FOREGROUND_BLUE | FOREGROUND_INTENSITY
#define WARN_COLOR FOREGROUND_RED | FOREGROUND_GREEN
#define ERROR_COLOR FOREGROUND_RED | FOREGROUND_INTENSITY
#define CRITIC_COLOR FOREGROUND_RED
#else
#define TRACE_COLOR "\033[0;34m"
#define WARN_COLOR "\033[33m"
#define ERROR_COLOR "\033[91m"
#define CRITIC_COLOR "\033[31m"
#endif

namespace Sleak {

std::shared_ptr<spdlog::logger> Logger::CoreLogger;
std::shared_ptr<spdlog::logger> Logger::GameLogger;

void Logger::Init(const std::string& ProjectName) {
    // Create the async thread pool (use 8192 queue size, 1 worker thread)
    spdlog::init_thread_pool(8192, 1);

    // Set up a shared console sink
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace);
    console_sink->set_color(spdlog::level::trace, TRACE_COLOR);
    console_sink->set_color(spdlog::level::warn, WARN_COLOR);
    console_sink->set_color(spdlog::level::err, ERROR_COLOR);
    console_sink->set_color(spdlog::level::critical, CRITIC_COLOR);

    // File sink (optional)
    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>("Engine.log", true);

    // Combine console + file sinks
    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

    // Create async loggers
    CoreLogger = std::make_shared<spdlog::async_logger>(
        "Sleak Engine", sinks.begin(), sinks.end(), spdlog::thread_pool());
    GameLogger = std::make_shared<spdlog::async_logger>(
        ProjectName, sinks.begin(), sinks.end(), spdlog::thread_pool());

    CoreLogger->set_level(spdlog::level::trace);
    GameLogger->set_level(spdlog::level::trace);

    spdlog::register_logger(CoreLogger);
    spdlog::register_logger(GameLogger);
}

// Static function to get the logger
std::shared_ptr<spdlog::logger>& Logger::GetCoreLogger() {
    static std::shared_ptr<spdlog::logger> CoreLoggerInstance = CoreLogger;
    return CoreLoggerInstance;
}

std::shared_ptr<spdlog::logger>& Logger::GetLogger() {
    static std::shared_ptr<spdlog::logger> GameLoggerInstance = GameLogger;
    return GameLoggerInstance;
}

}  // namespace Sleak
