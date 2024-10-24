#include "arcpch.h"
#include "Module.h"

#include "DynamicLibrary.h"
#include "ErrorCode.h"

Module::Module(const std::filesystem::path& path) :
   m_originalModulePath(path)
{}

Module::~Module()
{
   Unload();
}

ARC::Result<void> Module::Load()
{
   std::lock_guard<std::mutex> lock(m_moduleMutex);

   if (m_running)
      ARC::Error::Create(LoomError::ModuleLoadFailure, "Module is already loaded: " + m_tempModulePath.string());

   m_tempModulePath = CreateTemporaryCopy();
   m_library = std::make_unique<DynamicLibrary>(m_tempModulePath);

   auto library = m_library->Load();
   if (!library)
   {
      DeleteTemporaryCopy();
      return library.Error();
   }

   auto entryPoint = m_library->GetFunction("EntryPoint");
   if (!entryPoint)
   {
      DeleteTemporaryCopy();
      return entryPoint.Error();
   }

   m_entryPointFunc = reinterpret_cast<EntryPointFunc>((void*) entryPoint);
   if (!m_entryPointFunc)
   {
      auto result = m_library->Unload();
      if (!result)
         ARC_CORE_ERROR(result.GetErrorMessage());
      DeleteTemporaryCopy();
      return entryPoint.Error();
   }

   m_running = true;
   m_moduleThread = std::thread([this]()
   {
      try
      {
         m_entryPointFunc();
      }
      catch (const std::exception& e)
      {
         ARC_CORE_ERROR("Exception thrown in module: " + m_originalModulePath.string() + ". " + e.what());
      }
      m_running = false;
   });

   m_lastWriteTime = std::filesystem::last_write_time(m_originalModulePath).time_since_epoch().count();

   return {};
}

ARC::Result<void> Module::Unload()
{
   std::lock_guard<std::mutex> lock(m_moduleMutex);
   if (!m_running && !m_monitoring)
      return ARC::Error::Create(LoomError::ModuleUnloadFailure, "Attempted to unload module '" + m_originalModulePath.string() + "', but the module is not loaded or currently monitored.");
   m_monitoring = false;

   if (m_monitorThread.joinable())
      m_monitorThread.join();

   if (m_moduleThread.joinable())
      m_moduleThread.join();

   auto result = m_library->Unload();
   if (!result)
      return result.Error();
   
   DeleteTemporaryCopy();

   m_running = false;
   return result;
}

void Module::StartMonitoring()
{
   m_monitorThread = std::thread(&Module::MonitorModule, this);
}

void Module::MonitorModule()
{
   while (m_monitoring.load(std::memory_order::relaxed))
   {
      std::this_thread::sleep_for(std::chrono::seconds(1));

      auto currentWriteTime = std::filesystem::last_write_time(m_originalModulePath).time_since_epoch().count();
      if (currentWriteTime != m_lastWriteTime)
      {
         ARC_CORE_INFO("Detected change in client module. Reloading...");

         try
         {
            std::lock_guard<std::mutex> lock(m_moduleMutex);
            m_library->Unload();

            if (m_moduleThread.joinable())
               m_moduleThread.join();

            m_library->Load();
            m_entryPointFunc = reinterpret_cast<EntryPointFunc>((void*)(m_library->GetFunction("EntryPoint")));

            if (!m_entryPointFunc)
               throw std::runtime_error("Failed to find 'EntryPoint' after reloading client module.");

            m_running = true;
            m_moduleThread = std::thread([this]()
            {
               try
               {
                  m_entryPointFunc();
               }
               catch (const std::exception& e)
               {
                  ARC_CORE_ERROR(std::string("Exception in client module: ") + e.what());
               }
               m_running = false;
            });

            ARC_CORE_INFO("Client module reloaded successfully.");
         }
         catch (const std::exception& e)
         {
            ARC_CORE_ERROR(std::string("Error during hot-swapping: ") + e.what());
         }

         m_lastWriteTime = currentWriteTime;
      }
   }
}

std::filesystem::path Module::CreateTemporaryCopy() const
{
   auto epoch = std::chrono::system_clock::now().time_since_epoch();
   std::string tempName = m_originalModulePath.filename().string() + ".tmp_" + std::to_string(epoch.count());
   std::filesystem::path tempPath = std::filesystem::temp_directory_path() / tempName;

   try
   {
      std::filesystem::copy_file(m_originalModulePath, tempPath, std::filesystem::copy_options::overwrite_existing);
   }
   catch (const std::exception& e)
   {
      ARC_CORE_ERROR("Failed to create temporary copy: " + std::string(e.what()));
      return m_originalModulePath;
   }

   return tempPath;
}

void Module::DeleteTemporaryCopy()
{
   if (!m_tempModulePath.empty())
   {
      try
      {
         std::filesystem::remove(m_tempModulePath);
         ARC_CORE_INFO("Temporary module copy deleted: " + m_tempModulePath.string());
      }
      catch (const std::exception& e)
      {
         ARC_CORE_ERROR("Failed to delete temporary copy: " + std::string(e.what()));
      }
      m_tempModulePath.clear();
   }
}
