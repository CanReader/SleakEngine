#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <Core/OSDef.hpp>

#define SPDLOG_WCHAR_SUPPORT

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef SLEAK_ENGINE
#define _LOGGER Sleak::Logger::GetCoreLogger()
#else
#define _LOGGER Sleak::Logger::GetLogger()
#endif

#define SLEAK_LOG(...) _LOGGER->trace(__VA_ARGS__);
#define SLEAK_INFO(...) _LOGGER->info(__VA_ARGS__);
#define SLEAK_WARN(...) _LOGGER->warn(__VA_ARGS__);
#define SLEAK_ERROR(...) _LOGGER->error(__VA_ARGS__);
#define SLEAK_FATAL(...) _LOGGER->critical(__VA_ARGS__);

#define SLEAK_RETURN_ERR(...)    \
    {                            \
        SLEAK_ERROR(__VA_ARGS__) \
        return false;            \
    }

namespace Sleak {

class ENGINE_API Logger {
   public:
    static void Init(const std::string& ProjectName);

    static std::shared_ptr<spdlog::logger>& GetCoreLogger();
    static std::shared_ptr<spdlog::logger>& GetLogger();

   private:
    static std::shared_ptr<spdlog::logger> CoreLogger;
    static std::shared_ptr<spdlog::logger> GameLogger;
};

}  // namespace Sleak

#endif  // _LOGGER_H_
