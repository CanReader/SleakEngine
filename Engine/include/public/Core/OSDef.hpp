#ifndef _OSDEF_H_
#define _OSDEF_H_

/* Windows x86/x64 */
#ifdef _WIN32 
    #ifdef SLEAK_EXPORTS
        #define SLEAK_API __declspec(dllexport)
    #else
        #define SLEAK_API __declspec(dllimport)
    #endif

    #ifdef SLEAK_ENGINE
        #define ENGINE_API __declspec(dllexport)
    #else
        #define ENGINE_API __declspec(dllimport)
    #endif
    
  /* Windows x64 */
 #ifdef _WIN64
    #define PLATFORM_WIN
    #define OS "windows"
    #define WINDOW_SYSTEM "dwm"
  /* Windows x86 */
  #else
    #error "x86 architecture is not supported for this program!" 
  #endif

  /* MacOS/Iphone */
#elif defined(__IOS__) || defined(__APPLE__) || defined(__MACH__) 
  #define PLATFORM_IOS
  #define OS "ios"
  #define WINDOW_SYSTEM "quartz"
  #define SLEAK_API
  #define ENGINE_API

/* Android devices (need to be defined before linux) */
#elif defined(__ANDROID__)
  #define PLATFORM_ANDROID
  #define OS "android"
  #define WINDOW_SYSTEM "surfaceflinger"  
  #define SLEAK_API
  #define ENGINE_API

/* Berkley Software Distribution */
#elif defined(__FreeBSD__)
  #error "FreeBSD not supported yet"
/* Linux and all distros */
#elif defined(__linux__)
  #define PLATFORM_LINUX
  #define OS "linux"
  #define WINDOW_SYSTEM std::getenv("XDG_SESSION_TYPE")
  #define SLEAK_API
  #define ENGINE_API

#else
  #error "Undefined OS"

#endif

#define MACRO_STR(macro) #macro

#define RESOURCE(path) R"(assets/textures/" path ")"

#include <Logger.hpp>

#endif