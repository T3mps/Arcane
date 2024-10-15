#pragma once

namespace ARC
{
	class UUID
	{
	public:
		UUID();
		UUID(uint64_t uuid);
		UUID(const UUID&) = default;

		operator uint64_t() const { return m_UUID; }

		std::string ToString() const;

	private:
		uint64_t m_UUID;
	};
} // namespace ARC

namespace std
{
	template  <typename T>
	struct hash;

	template <>
	struct hash<ARC::UUID>
	{
		size_t operator()(const ARC::UUID& uuid) const
		{
			return static_cast<uint64_t>(uuid);
		}
	};
} // namespace std
