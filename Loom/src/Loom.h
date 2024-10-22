#pragma once

#include "ArgumentParser.h"
#include "Module.h"

class Loom
{
public:
   ARC_NORETURN Loom(int32_t argc, char* argv[]);
   ~Loom();
   Loom(const Loom&) = delete;
   Loom& operator=(const Loom&) = delete;

   static inline Loom& GetInstance() { return *s_instance; }

   bool IsRunning() const { return m_clientModule && m_clientModule->IsRunning(); }

private:
   ARC::Result<void> Initialize(int32_t argc, char* argv[]);
   void Cleanup();

   ARC_NORETURN void ShowHelpMessage();

   std::unique_ptr<Module> m_clientModule;
   ArgumentParser m_argParser;

   static Loom* s_instance;
};
