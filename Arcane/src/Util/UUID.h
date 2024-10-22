#pragma once

#if defined(ARC_PLATFORM_WINDOWS)
	#include <Windows.h>
	#include <bcrypt.h>
	#pragma comment(lib, "Bcrypt.lib")
#else
	#include <unistd.h>
	#include <sys/syscall.h>
	#include <sys/random.h>
#endif

namespace ARC
{
   template <std::size_t Bits = 128>
   class UID;

	template <>
	class UID<128>
	{
	public:
		using value_t = std::array<uint64_t, 2>;

		UID()
		{
			GenerateRandomBytes(reinterpret_cast<uint8_t*>(&m_uuid), sizeof(m_uuid));
		}

		explicit UID(value_t uuid) : m_uuid(uuid) {}

		UID(const UID&) = default;
		UID(UID&&) noexcept = default;
		UID& operator=(const UID&) = default;
		UID& operator=(UID&&) noexcept = default;
		~UID() = default;

		explicit operator value_t() const noexcept { return m_uuid; }

		std::string ToString() const
		{
			return std::format("{:016x}{:016x}", m_uuid[0], m_uuid[1]);
		}

		friend bool operator==(const UID& lhs, const UID& rhs) noexcept = default;
		friend std::strong_ordering operator<=>(const UID& lhs, const UID& rhs) noexcept = default;

		friend std::ostream& operator<<(std::ostream& os, const UID& uid)
		{
			os << uid.ToString();
			return os;
		}

		friend std::istream& operator>>(std::istream& is, UID& uid)
		{
			std::string str;
			is >> str;
			if (str.size() == 32)
			{
				uint64_t high, low;
				auto result1 = std::from_chars(str.data(), str.data() + 16, high, 16);
				auto result2 = std::from_chars(str.data() + 16, str.data() + 32, low, 16);

				if (result1.ec == std::errc() && result2.ec == std::errc())
				{
					uid.m_uuid = { high, low };
				}
				else
				{
					is.setstate(std::ios::failbit);
				}
			}
			else
			{
				is.setstate(std::ios::failbit);
			}
			return is;
		}

	private:
		value_t m_uuid{};

		static void GenerateRandomBytes(uint8_t* buffer, std::size_t size)
		{
#if defined(ARC_PLATFORM_WINDOWS)
			NTSTATUS status = BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
			if (!BCRYPT_SUCCESS(status))
			{
				throw std::runtime_error("Failed to generate random bytes using BCryptGenRandom");
			}
#elif defined(ARC_PLATFORM_LINUX)
			ssize_t result = syscall(SYS_getrandom, buffer, size, 0);
			if (result != static_cast<ssize_t>(size))
			{
				throw std::runtime_error("Failed to generate random bytes using getrandom");
			}
#endif
		}
	};

	template <>
	class UID<64>
	{
	public:
		using value_t = uint64_t;

		UID()
		{
			GenerateRandomBytes(reinterpret_cast<uint8_t*>(&m_uuid), sizeof(m_uuid));
		}

		explicit UID(value_t uuid) : m_uuid(uuid) {}

		UID(const UID&) = default;
		UID(UID&&) noexcept = default;
		UID& operator=(const UID&) = default;
		UID& operator=(UID&&) noexcept = default;
		~UID() = default;

		explicit operator value_t() const noexcept { return m_uuid; }

		std::string ToString() const
		{
			return std::format("{:016x}", m_uuid);
		}

		friend bool operator==(const UID& lhs, const UID& rhs) noexcept = default;
		friend std::strong_ordering operator<=>(const UID& lhs, const UID& rhs) noexcept = default;

		friend std::ostream& operator<<(std::ostream& os, const UID& uid)
		{
			os << uid.ToString();
			return os;
		}

		friend std::istream& operator>>(std::istream& is, UID& uid)
		{
			std::string str;
			is >> str;
			if (str.size() == 16)
			{
				uint64_t value = 0;
				auto result = std::from_chars(str.data(), str.data() + 16, value, 16);

				if (result.ec == std::errc{})
				{
					uid.m_uuid = value;
				}
				else
				{
					is.setstate(std::ios::failbit);
				}
			}
			else
			{
				is.setstate(std::ios::failbit);
			}
			return is;
		}

	private:
		value_t m_uuid{};

		static void GenerateRandomBytes(uint8_t* buffer, std::size_t size)
		{
#if defined(ARC_PLATFORM_WINDOWS)
			NTSTATUS status = BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
			if (!BCRYPT_SUCCESS(status))
			{
				throw std::runtime_error("Failed to generate random bytes using BCryptGenRandom");
			}
#elif defined(ARC_PLATFORM_LINUX)
			ssize_t result = syscall(SYS_getrandom, buffer, size, 0);
			if (result != static_cast<ssize_t>(size))
			{
				throw std::runtime_error("Failed to generate random bytes using getrandom");
			}
#endif
		}
	};

	template <>
	class UID<32>
	{
	public:
		using value_t = uint32_t;

		UID()
		{
			GenerateRandomBytes(reinterpret_cast<uint8_t*>(&m_uuid), sizeof(m_uuid));
		}

		explicit UID(value_t uuid) : m_uuid(uuid) {}

		UID(const UID&) = default;
		UID(UID&&) noexcept = default;
		UID& operator=(const UID&) = default;
		UID& operator=(UID&&) noexcept = default;
		~UID() = default;

		explicit operator value_t() const noexcept { return m_uuid; }

		std::string ToString() const
		{
			return std::format("{:08x}", m_uuid);
		}

		friend bool operator==(const UID& lhs, const UID& rhs) noexcept = default;
		friend std::strong_ordering operator<=>(const UID& lhs, const UID& rhs) noexcept = default;

		friend std::ostream& operator<<(std::ostream& os, const UID& uid)
		{
			os << uid.ToString();
			return os;
		}

		friend std::istream& operator>>(std::istream& is, UID& uid)
		{
			std::string str;
			is >> str;
			if (str.size() == 8)
			{
				uint32_t value = 0;
				auto result = std::from_chars(str.data(), str.data() + 8, value, 16);

				if (result.ec == std::errc{})
				{
					uid.m_uuid = value;
				}
				else
				{
					is.setstate(std::ios::failbit);
				}
			}
			else
			{
				is.setstate(std::ios::failbit);
			}
			return is;
		}

	private:
		value_t m_uuid{};

		static void GenerateRandomBytes(uint8_t* buffer, std::size_t size)
		{
#if defined(ARC_PLATFORM_WINDOWS)
			NTSTATUS status = BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
			if (!BCRYPT_SUCCESS(status))
			{
				throw std::runtime_error("Failed to generate random bytes using BCryptGenRandom");
			}
#elif defined(ARC_PLATFORM_LINUX)
			ssize_t result = syscall(SYS_getrandom, buffer, size, 0);
			if (result != static_cast<ssize_t>(size))
			{
				throw std::runtime_error("Failed to generate random bytes using getrandom");
			}
#endif
		}
	};

	using UUID	 = UID<128>;
	using GUID	 = UUID;
	using UUID64 = UID<64>;
	using UUID32 = UID<32>;
} // namespace ARC

namespace std
{
	template <std::size_t Bits>
	struct hash<ARC::UID<Bits>>
	{
		size_t operator()(const ARC::UID<Bits>& uuid) const noexcept
		{
			if constexpr (Bits == 128)
			{
				const auto& value = static_cast<typename ARC::UID<Bits>::value_t>(uuid);
				size_t seed = 0;
				seed ^= std::hash<uint64_t>{}(value[0]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
				seed ^= std::hash<uint64_t>{}(value[1]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
				return seed;
			}
			else if constexpr (Bits == 64)
			{
				return std::hash<uint64_t>()(static_cast<uint64_t>(uuid));
			}
			else if constexpr (Bits == 32)
			{
				return std::hash<uint32_t>()(static_cast<uint32_t>(uuid));
			}
		}
	};
} // namespace std
