#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Arcane
{
    // ============================================================================
    // Enums
    // ============================================================================

    enum class ItemRarity : uint8_t
    {
        Invalid = 0,
        ThreeStar = 3,
        FourStar = 4,
        FiveStar = 5
    };

    enum class ItemType : uint8_t
    {
        Invalid = 0,
        Character,
        Weapon
    };

    enum class CharacterArchetype : uint8_t
    {
        Invalid   = 0,
        Duelist   = 1,  // Single-target damage
        Vanguard  = 2,  // Tank / frontline
        Support   = 3,  // Healer / sustain
        Tactician = 4,  // Buffer / debuffer
    };

    // Combat element — circular weakness chain:
    // Solar > Frost > Arc > Force > Void > Radiance > Solar
    enum class Element : uint8_t
    {
        None     = 0,
        Solar    = 1,
        Frost    = 2,
        Arc      = 3,
        Force    = 4,
        Void     = 5,
        Radiance = 6,
    };

    enum class BannerType : uint8_t
    {
        Invalid = 0,
        Character,
        Weapon,
        Standard
    };

    // ============================================================================
    // Data Structures
    // ============================================================================

    struct Item
    {
        std::string id;
        std::string name;
        ItemRarity    rarity     = ItemRarity::Invalid;
        ItemType  type       = ItemType::Invalid;
        CharacterArchetype archetype  = CharacterArchetype::Invalid; // Meaningful for characters; Invalid for weapons
        Element   element    = Element::None;

        bool operator==(const Item& other) const { return id == other.id; }
        bool operator!=(const Item& other) const { return !(*this == other); }
    };

    struct PullResult
    {
        Item item;
        bool wasFeatured = false;
        bool wasPity = false;
        bool wasGuarantee = false;
        int pullNumber = 0;
    };

    struct PlayerStats
    {
        int totalCharacterPulls = 0;
        int totalWeaponPulls = 0;
        int totalStandardPulls = 0;
        int fiveStarCharacters = 0;
        int fiveStarWeapons = 0;
        int fourStarCount = 0;
        int threeStarCount = 0;
    };

    // int64_t to match the full persistence chain (event log, reducers,
    // BIGINT schema columns) — see Wallet.hpp's class comment.
    struct WalletState
    {
        std::int64_t credits = 0;            // Earnable hard currency (quests, login, events; converts to tickets at 160:1)
        std::int64_t universalCredits = 0;   // Paid premium currency (real money; future premium shop)
        std::int64_t tickets = 0;            // Standard banner pull vouchers
        std::int64_t limitedTickets = 0;     // Limited banner pull vouchers
        std::int64_t scrap = 0;              // Soft currency (combat drops, dailies, dismantling; funds all progression)
    };

    struct PityState
    {
        int pullsSinceFourStar = 0;
        int pullsSinceFiveStar = 0;
    };

    struct BannerInfo
    {
        std::string id;
        std::string name;
        BannerType type = BannerType::Invalid;
        std::string featuredItemName;
        std::string featuredItemId;
        std::vector<std::string> featured4StarIds;
    };

    // ============================================================================
    // Utility Functions
    // ============================================================================

    inline std::string GetRarityStars(ItemRarity r)
    {
        switch (r)
        {
            case ItemRarity::ThreeStar: return "***";
            case ItemRarity::FourStar:  return "****";
            case ItemRarity::FiveStar:  return "*****";
            default:                return "*";
        }
    }

    inline CharacterArchetype ParseArchetype(const std::string& s)
    {
        if (s == "duelist")   return CharacterArchetype::Duelist;
        if (s == "vanguard")  return CharacterArchetype::Vanguard;
        if (s == "support")   return CharacterArchetype::Support;
        if (s == "tactician") return CharacterArchetype::Tactician;
        return CharacterArchetype::Invalid;
    }

    inline Element ParseElement(const std::string& s)
    {
        if (s == "solar")    return Element::Solar;
        if (s == "frost")    return Element::Frost;
        if (s == "arc")      return Element::Arc;
        if (s == "force")    return Element::Force;
        if (s == "void")     return Element::Void;
        if (s == "radiance") return Element::Radiance;
        return Element::None;
    }

    inline std::string GetElementName(Element e)
    {
        switch (e)
        {
            case Element::Solar:    return "solar";
            case Element::Frost:    return "frost";
            case Element::Arc:      return "arc";
            case Element::Force:    return "force";
            case Element::Void:     return "void";
            case Element::Radiance: return "radiance";
            default:                return "none";
        }
    }

    inline std::string GetItemTypeName(ItemType t)
    {
        switch (t)
        {
            case ItemType::Character: return "Character";
            case ItemType::Weapon:    return "Weapon";
            default:                  return "Unknown";
        }
    }

    inline std::string GetBannerTypeName(BannerType t)
    {
        switch (t)
        {
            case BannerType::Character: return "character";
            case BannerType::Weapon:    return "weapon";
            case BannerType::Standard:  return "standard";
            default:                    return "unknown";
        }
    }

    // ANSI Colors (for console output)
    namespace Color
    {
        constexpr const char* Reset   = "\033[0m";
        constexpr const char* Bold    = "\033[1m";
        constexpr const char* Red     = "\033[31m";
        constexpr const char* Green   = "\033[32m";
        constexpr const char* Yellow  = "\033[33m";
        constexpr const char* Magenta = "\033[35m";
        constexpr const char* Cyan    = "\033[36m";
        constexpr const char* White   = "\033[37m";
    }

    inline std::string GetRarityColor(ItemRarity r)
    {
        switch (r)
        {
            case ItemRarity::ThreeStar: return Color::White;
            case ItemRarity::FourStar:  return Color::Magenta;
            case ItemRarity::FiveStar:  return Color::Yellow;
            default:                return Color::Reset;
        }
    }

} // namespace Arcane
