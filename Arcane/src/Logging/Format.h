// Format.h
#pragma once

namespace ARC
{
   class Format;

   enum class OutputFormat : uint8_t 
   { 
      Pattern = 0,
      JSON,
      XML,
      CSV,
      KeyValue,
      Syslog,
      Graylog
   };

   enum class TokenType : uint8_t 
   {
      None,
      Timestamp,
      Level,
      Name,
      Message,
      Source,
      Function,
      Line
   };

   struct FormatPatterns
   {
      static constexpr std::string_view DEFAULT  = "[{timestamp}] [{level}] [{name} @ {function}] - {message}";
      static constexpr std::string_view SIMPLE   = "[{timestamp}] [{level}] [{name}] - {message}";
      static constexpr std::string_view DETAILED = "[{timestamp}] [{level}] [{name} @ {function}] [{source}:{line}] - {message}";
   };

   struct FormatInfo
   {
      std::string_view pattern = FormatPatterns::DEFAULT;
      OutputFormat outputFormat = OutputFormat::Pattern;
      bool colorize = true;
      size_t fragmentCapacity = 32;
      std::string_view timeFormat = "%H:%M:%S";
   };

   class Format 
   {
   public:
      struct Token
      {
         static constexpr std::string_view TIMESTAMP = "{timestamp}";
         static constexpr std::string_view LEVEL     = "{level}";
         static constexpr std::string_view NAME      = "{name}";
         static constexpr std::string_view MESSAGE   = "{message}";
         static constexpr std::string_view SOURCE    = "{source}";
         static constexpr std::string_view FUNCTION  = "{function}";
         static constexpr std::string_view LINE      = "{line}";

         static TokenType GetType(std::string_view token);
      };

      Format(const FormatInfo& info = FormatInfo{});
      ~Format() = default;

      Format(const Format&) = delete;
      Format& operator=(const Format&) = delete;
      Format(Format&&) noexcept = default;
      Format& operator=(Format&&) noexcept = default;

      std::string operator()(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location = std::source_location::current());

      void SetPattern(std::string_view pattern);
      void SetOutputFormat(OutputFormat format) { m_info.outputFormat = format; }
      void SetTimeFormat(std::string_view format) { m_info.timeFormat = format; }
      void SetColorize(bool enable) { m_info.colorize = enable; }

      void UseSimplePattern() { SetPattern(FormatPatterns::SIMPLE); SetOutputFormat(OutputFormat::Pattern); }
      void UseDetailedPattern() { SetPattern(FormatPatterns::DETAILED); SetOutputFormat(OutputFormat::Pattern); }
      void UseJSON() { SetOutputFormat(OutputFormat::JSON); }
      void UseXML() { SetOutputFormat(OutputFormat::XML); }
      void UseCSV() { SetOutputFormat(OutputFormat::CSV); }
      void UseKeyValue() { SetOutputFormat(OutputFormat::KeyValue); }
      void UseSyslog() { SetOutputFormat(OutputFormat::Syslog); }
      void UseGraylog() { SetOutputFormat(OutputFormat::Graylog); }

      [[nodiscard]] OutputFormat GetOutputFormat() const { return m_info.outputFormat; }
      [[nodiscard]] std::string_view GetPattern() const { return m_pattern; }
      [[nodiscard]] std::string_view GetTimeFormat() const { return m_info.timeFormat; }
      [[nodiscard]] bool IsColorized() const { return m_info.colorize; }

   private:
      struct Fragment 
      {
         std::string_view literal;
         TokenType type;
      };

      struct TimeBuffer 
      {
         std::array<char, 32> buffer;
         size_t length = 0;

         [[nodiscard]] constexpr size_t size() const { return length; }
         [[nodiscard]] const char* data() const { return buffer.data(); }
      };

      std::string FormatPattern(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);
      std::string FormatJSON(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);
      std::string FormatXML(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);
      std::string FormatCSV(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);
      std::string FormatKeyValue(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);
      std::string FormatSyslog(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);
      std::string FormatGraylog(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);

      void ParsePattern(std::string_view pattern);
      [[nodiscard]] TimeBuffer GetTimeString() const;
      [[nodiscard]] static std::string GetFunctionName(const char* function);
      [[nodiscard]] static std::string CleanFunctionName(const std::string& signature);
      [[nodiscard]] static std::string EscapeString(const std::string& input);
      [[nodiscard]] std::string GetSourceFileName(const std::source_location& location) const;

      std::string m_pattern;
      std::vector<Fragment> m_fragments;
      FormatInfo m_info;
      size_t m_estimatedSize;
   };

   struct FormatSpecifier
   {
      static constexpr const char* JSON     = R"({{"timestamp":"{}","level":"{}","name":"{}","message":"{}","source":"{}","function":"{}","line":"{}"}})";
      static constexpr const char* XML      = R"(<log><timestamp>{}</timestamp><level>{}</level><name>{}</name><message>{}</message><source>{}</source><function>{}</function><line>{}</line></log>)";
      static constexpr const char* CSV      = "{},{},{},{},{},{},{}";
      static constexpr const char* KeyValue = "timestamp={} level={} name={} message=\"{}\" source={} function={} line={}";
      static constexpr const char* Syslog   = "<{}>{} {} {}[{}]: {} ({}:{})";
      static constexpr const char* Graylog  = R"({{"version":"1.1","host":"{}","short_message":"{}","full_message":"{}","timestamp":"{}","level":"{}","_logger_name":"{}","_source_file":"{}","_source_line":"{}","_source_function":"{}"}})";
   };
} // namespace ARC