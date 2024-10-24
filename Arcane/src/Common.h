#pragma once

#ifndef ARC_PLATFORM_WINDOWS
   #ifndef ARC_PLATFORM_LINUX
      #error Unsupported platform.
   #endif
#endif

#ifdef _MSC_VER
   #define ARC_COMPILER_MSVC
#elif defined(__clang__)
   #define ARC_COMPILER_CLANG
#elif defined(__GNUC__)
   #define ARC_COMPILER_GNUC
#else
   #define ARC_COMPILER_UNKNOWN
#endif

#ifdef ARC_PLATFORM_WINDOWS
   #define NOMINMAX
   #ifdef min
      #undef min
   #endif
   #ifdef max
      #undef max
   #endif
#endif

#ifdef ARC_BUILD_DEBUG
   #ifdef ARC_COMPILER_MSVC
      #define ARC_DEBUGBREAK() __debugbreak()
   #elif defined(ARC_COMPILER_GNUC)
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

#define ARC_API extern "C" __declspec(dllexport)

#define ARC_NOINLINE __declspec(noinline)
#define ARC_NORETURN __declspec(noreturn)

#define ARC_ALIGN(x) __declspec(align(x))

#define ARC_DEPRECATED __declspec(deprecated)

#define ARC_RESTRICT __declspec(restrict)

#if defined(ARC_COMPILER_MSVC)
   #define ARC_FORCE_INLINE __forceinline
#elif defined(ARC_COMPILER_GNUC) || defined(ARC_COMPILER_CLANG)
   #define ARC_FORCE_INLINE __attribute__((always_inline))
#else
   #define ARC_FORCE_INLINE
#endif

#if __cplusplus >= 201703L
   #define ARC_FALLTHROUGH [[fallthrough]]
#elif defined(ARC_COMPILER_GNUC) || defined(ARC_COMPILER_CLANG)
   #define ARC_FALLTHROUGH __attribute__((fallthrough))
#elif defined(ARC_COMPILER_MSVC)
   #define ARC_FALLTHROUGH __fallthrough
#else
   #define ARC_FALLTHROUGH // fallthrough
#endif

using float32_t = float;
using float64_t = double;
