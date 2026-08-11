#pragma once
#if defined(_WIN32)
  #if defined(GAME_BUILD_DLL)
    #define GAME_API __declspec(dllexport)
  #else
    #define GAME_API __declspec(dllimport)
  #endif
#else
  #define GAME_API __attribute__((visibility("default")))
#endif
