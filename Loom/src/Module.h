#pragma once

class DynamicLibrary;

class Module
{
public:
   explicit Module(const std::filesystem::path& path);
   ~Module();

   Module(const Module&) = delete;
   Module& operator=(const Module&) = delete;
   Module(Module&&) = delete;
   Module& operator=(Module&&) = delete;

   explicit operator bool() const noexcept { return IsRunning(); }

   ARC::Result<void> Load();
   ARC::Result<void> Unload();
   
   void StartMonitoring();

   inline bool IsRunning() const { return m_running; }

   std::string GetName() const { return m_originalModulePath.filename().string(); }
   std::string GetPath() const { return m_originalModulePath.string(); }

private:
   void MonitorModule();
   std::filesystem::path CreateTemporaryCopy() const;
   void DeleteTemporaryCopy();

   std::unique_ptr<DynamicLibrary> m_library;
   std::filesystem::path m_originalModulePath;
   std::filesystem::path m_tempModulePath;

   using EntryPointFunc = void (*)();

   EntryPointFunc m_entryPointFunc{nullptr};

   std::thread m_moduleThread;
   std::thread m_monitorThread;
   
   std::atomic<bool> m_running{false};
   std::atomic<bool> m_monitoring{true};

   std::time_t m_lastWriteTime{0};

   mutable std::mutex m_moduleMutex;
};
