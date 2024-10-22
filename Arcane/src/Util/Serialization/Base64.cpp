#include "arcpch.h"
#include "Base64.h"

constexpr std::string_view ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "abcdefghijklmnopqrstuvwxyz"
                                      "0123456789+/";

constexpr std::array<uint8_t, 4> EncodeBlock(const std::array<uint8_t, 3>& charArray3, size_t validBytes)
{
   std::array<uint8_t, 4> charArray4 = { 0 };
   charArray4[0] = (charArray3[0] & 0xfc) >> 2;
   charArray4[1] = ((charArray3[0] & 0x03) << 4) + ((charArray3[1] & 0xf0) >> 4);
   charArray4[2] = validBytes > 1 ? (((charArray3[1] & 0x0f) << 2) + ((charArray3[2] & 0xc0) >> 6)) : 64;
   charArray4[3] = validBytes > 2 ? (charArray3[2] & 0x3f) : 64; // 64 is the index for '=' padding
   return charArray4;
}

std::string ARC::Base64::Encode(const std::string& input)
{
   return Encode(std::span(reinterpret_cast<const uint8_t*>(input.data()), input.size()));
}

std::string ARC::Base64::Encode(std::span<const uint8_t> bytes)
{
   const size_t inputLength = bytes.size();
   size_t outputLength = 4 * ((inputLength + 2) / 3); // each 3 bytes of input -> 4 chars of output
   std::string result;
   result.reserve(outputLength);

   std::array<uint8_t, 3> charArray3 = { 0 };

   size_t i = 0;
   while (i < inputLength)
   {
      size_t remaining = std::min(inputLength - i, static_cast<size_t>(3));
      std::copy_n(bytes.begin() + i, remaining, charArray3.begin());

      std::array<uint8_t, 4> encodedBlock = EncodeBlock(charArray3, remaining);
      for (auto value : encodedBlock)
      {
         result += (value < 64 ? ALPHABET[value] : '='); // Use '=' for padding (index 64)
      }
      i += 3;
   }

   return result;
}

constexpr uint8_t BASE64_LOOKUP[] =
{
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
   52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
   64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
   15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
   64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
   41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
   64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

std::string ARC::Base64::Decode(const std::string& encodedString)
{
   size_t inputLength = encodedString.size();

   if (inputLength % 4 != 0)
   {
      ARC::Log::CoreError("Input length must be a multiple of 4 for valid Base64 encoding, cannot decode.");
      return {};
   }

   size_t padding = 0;
   if (encodedString[inputLength - 1] == '=')
      ++padding;
   if (encodedString[inputLength - 2] == '=')
      ++padding;

   size_t outputLength = (inputLength / 4) * 3 - padding;
   std::string result;
   result.reserve(outputLength);

   std::array<uint8_t, 4> charArray4;
   std::array<uint8_t, 3> charArray3;

   size_t i = 0;
   while (i < inputLength)
   {
      // Decode 4 Base64 characters into their 6-bit values
      for (size_t j = 0; j < 4; ++j)
      {
         uint8_t c = static_cast<uint8_t>(encodedString[i + j]);
         if (BASE64_LOOKUP[c] == -1)
         {
            ARC::Log::CoreError("Invalid character in Base64 string, cannot continue decoding.");
            return {};
         }
         charArray4[j] = BASE64_LOOKUP[c];
      }

      // Convert the 6-bit values back into 3 bytes
      charArray3[0] = (charArray4[0] << 2) + ((charArray4[1] & 0x30) >> 4);
      charArray3[1] = ((charArray4[1] & 0x0f) << 4) + ((charArray4[2] & 0x3c) >> 2);
      charArray3[2] = ((charArray4[2] & 0x03) << 6) + charArray4[3];

      if (i + 4 >= inputLength)
      {
         for (size_t j = 0; j < 3 - padding; ++j)
         {
            result += charArray3[j];
         }
      }
      else
      {
         for (size_t j = 0; j < 3; ++j)
         {
            result += charArray3[j];
         }
      }

      i += 4;
   }

   return result;
}
