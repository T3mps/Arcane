#include "arcpch.h"
#include "JSON.h"

ARC::JSON::JSON(const std::string& filePath)
{
   Load(filePath);
}

bool ARC::JSON::Save(const std::string& filePath, int32_t tabSize) const
{
   std::ofstream file(filePath);
   if (!file.is_open())
   {
      std::cerr << "Failed to save file: " << filePath << std::endl;
      return false;
   }

   try
   {
      file << std::setw(tabSize) << m_data;
   }
   catch (const std::exception& e)
   {
      std::cerr << "Error writing JSON to file: " << filePath << "\n" << e.what() << std::endl;
      return false;
   }

   file.close();
   return true;
}

bool ARC::JSON::Load(const std::string& filePath)
{
   std::ifstream file(filePath);
   if (!file.is_open())
   {
      std::cerr << "Failed to open file: " << filePath << std::endl;
      return false;
   }

   try
   {
      file >> m_data;
   }
   catch (const std::exception& e)
   {
      std::cerr << "Error parsing JSON from file: " << filePath << "\n" << e.what() << std::endl;
      return false;
   }

   file.close();
   return true;
}

std::string ARC::JSON::ToString(int32_t tabSize) const
{
   return m_data.dump(tabSize);
}

