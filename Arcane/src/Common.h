#pragma once

#ifndef ARC_PLATFORM_WINDOWS
   #ifndef(ARC_PLATFORM_LINUX)
      #error Unknown platform.
   #endif
#endif

#ifdef __GNUC__
   #define ARC_COMPILER_GNUC
   #ifdef __clang__
      #define ARC_COMPILER_CLANG
   #else
      #define ARC_COMPILER_GCC
   #endif
#elif defined(_MSC_VER)
   #define ARC_COMPILER_MSVC
#endif

#ifdef ARC_PLATFORM_WINDOWS
   #define NOMINMAX
   #ifdef min
      #undef min
   #endif
   #ifdef max
      #undef max
   #endif

   #ifndef GET_X_LPARAM
      #define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
   #endif

   #ifndef GET_Y_LPARAM
      #define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))
   #endif
#endif

#ifndef GET_X_LPARAM
   #define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#endif

#ifndef GET_Y_LPARAM
   #define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))
#endif

#ifdef ARC_BUILD_DEBUG
   #ifdef ARC_COMPILER_MSVC
      #define ARC_DEBUGBREAK() __debugbreak()
   #elif defined(ARC_COMPILER_GNUC)
      #include <csignal>
      #define ARC_DEBUGBREAK() raise(SIGTRAP)
   #else
      #define ARC_DEBUGBREAK() std::abort()
   #endif
      #define ARC_ENABLE_ASSERTS
#else
   #define ARC_DEBUGBREAK()
#endif

#define ARC_EXPAND_MACRO(x) x
#define ARC_STRINGIFY_MACRO(x) #x

using float32_t = float;
using float64_t = double;
