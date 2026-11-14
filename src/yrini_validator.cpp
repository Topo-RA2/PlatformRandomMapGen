#include "rmg/yrini_validator.h"

#include "rmg/ini_config.h"
#include "pack_codec/pack_encoding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace rmg
{
namespace
{

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hashByte(std::uint64_t &hash, std::uint8_t value)
{
    hash ^= value;
    hash *= kFnvPrime;
}

void hashU32(std::uint64_t &hash, std::uint32_t value)
{
    hashByte(hash, static_cast<std::uint8_t>(value & 0xFFu));
    hashByte(hash, static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    hashByte(hash, static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    hashByte(hash, static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void hashI32(std::uint64_t &hash, int value)
{
    hashU32(hash, static_cast<std::uint32_t>(value));
}

std::string hex64(std::uint64_t value)
{
    char buffer[17] {};
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
    return buffer;
}

YrIniPackFingerprint hashPackBytes(const std::vector<std::uint8_t> &bytes)
{
    YrIniPackFingerprint result;
    if (bytes.empty())
    {
        return result;
    }

    std::uint64_t hash = kFnvOffset;
    hashI32(hash, static_cast<int>(bytes.size()));
    for (const auto byte : bytes)
    {
        hashByte(hash, byte);
    }

    result.captured = true;
    result.length = static_cast<int>(bytes.size());
    result.hash = hex64(hash);
    return result;
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("failed to open yrini file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool isExcludedSection(std::string_view name, bool excludeDigest)
{
    return excludeDigest && name == "Digest";
}

YrIniPackFingerprint hashMapFileWithoutSections(const std::vector<std::uint8_t> &bytes, bool excludeDigest)
{
    std::vector<std::uint8_t> filtered;
    filtered.reserve(bytes.size());

    bool skipSection = false;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t lineBegin = offset;
        while (offset < bytes.size() && bytes[offset] != '\n')
        {
            ++offset;
        }
        if (offset < bytes.size())
        {
            ++offset;
        }

        const std::size_t lineEnd = offset;
        std::size_t contentEnd = lineEnd;
        while (contentEnd > lineBegin && (bytes[contentEnd - 1] == '\n' || bytes[contentEnd - 1] == '\r'))
        {
            --contentEnd;
        }

        if (contentEnd > lineBegin && bytes[lineBegin] == '[')
        {
            std::size_t close = lineBegin + 1;
            while (close < contentEnd && bytes[close] != ']')
            {
                ++close;
            }

            if (close < contentEnd)
            {
                const auto *nameBegin = reinterpret_cast<const char *>(bytes.data() + lineBegin + 1);
                const auto nameLength = close - lineBegin - 1;
                skipSection = isExcludedSection(std::string_view(nameBegin, nameLength), excludeDigest);
            }
        }

        if (!skipSection)
        {
            filtered.insert(
                filtered.end(),
                bytes.begin() + static_cast<std::ptrdiff_t>(lineBegin),
                bytes.begin() + static_cast<std::ptrdiff_t>(lineEnd));
        }
    }

    return hashPackBytes(filtered);
}

std::string joinNumberedSection(const IniDocument &ini, const std::string &section)
{
    std::ostringstream out;
    for (int index = 1;; ++index)
    {
        const std::string value = ini.getString(section, std::to_string(index), "");
        if (value.empty())
        {
            break;
        }
        out << value;
    }
    return out.str();
}

YrIniPackFingerprint hashEncodedSection(const IniDocument &ini, const std::string &section)
{
    const auto encoded = joinNumberedSection(ini, section);
    if (encoded.empty())
    {
        return {};
    }
    return hashPackBytes(ra2::pack::base64Decode(encoded));
}

YrIniPackSummary summarizeEncodedSection(const IniDocument &ini, const std::string &section, bool lzo)
{
    YrIniPackSummary summary;
    const auto encoded = joinNumberedSection(ini, section);
    if (encoded.empty())
    {
        return summary;
    }

    const auto encodedBytes = ra2::pack::base64Decode(encoded);
    summary.encoded = hashPackBytes(encodedBytes);
    summary.decoded = hashPackBytes(lzo ? ra2::pack::decodeFormat5Lzo(encodedBytes) : ra2::pack::decodeFormat5Format80(encodedBytes));
    return summary;
}

std::vector<YrIniSectionSummary> summarizeSections(const std::vector<std::uint8_t> &bytes)
{
    std::vector<YrIniSectionSummary> sections;
    std::size_t offset = 0;
    int lineNumber = 1;
    int current = -1;
    while (offset < bytes.size())
    {
        const std::size_t lineBegin = offset;
        while (offset < bytes.size() && bytes[offset] != '\n')
        {
            ++offset;
        }
        if (offset < bytes.size())
        {
            ++offset;
        }

        std::size_t contentEnd = offset;
        while (contentEnd > lineBegin && (bytes[contentEnd - 1] == '\n' || bytes[contentEnd - 1] == '\r'))
        {
            --contentEnd;
        }

        if (contentEnd > lineBegin && bytes[lineBegin] == '[')
        {
            std::size_t close = lineBegin + 1;
            while (close < contentEnd && bytes[close] != ']')
            {
                ++close;
            }
            if (close < contentEnd)
            {
                const auto *nameBegin = reinterpret_cast<const char *>(bytes.data() + lineBegin + 1);
                const auto nameLength = close - lineBegin - 1;
                sections.push_back({std::string(nameBegin, nameLength), lineNumber, 0});
                current = static_cast<int>(sections.size()) - 1;
            }
        }
        else if (current >= 0 && contentEnd > lineBegin)
        {
            ++sections[static_cast<std::size_t>(current)].entryCount;
        }

        ++lineNumber;
    }
    return sections;
}

int parsePackedWaypoint(const std::string &value)
{
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char ch)
                                      {
                                          return ch >= '0' && ch <= '9';
                                      }))
    {
        throw std::runtime_error("Waypoint must be a non-negative decimal packed coordinate");
    }

    std::size_t parsedLength = 0;
    long long parsed = 0;
    try
    {
        parsed = std::stoll(value, &parsedLength, 10);
    }
    catch (const std::exception &)
    {
        throw std::runtime_error("Waypoint packed coordinate is outside the supported integer range");
    }
    if (parsedLength != value.size() || parsed > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("Waypoint packed coordinate is outside the supported integer range");
    }

    const int packed = static_cast<int>(parsed);
    const int x = packed % 1000;
    const int y = packed / 1000;
    if (x >= 512 || y >= 512)
    {
        throw std::runtime_error("Waypoint is outside 512x512 iso bounds");
    }
    return packed;
}

int countWaypoints(const IniDocument &ini)
{
    std::unordered_set<int> packedWaypoints;
    int count = 0;
    for (;; ++count)
    {
        const std::string value = ini.getString("Waypoints", std::to_string(count), "");
        if (value.empty())
        {
            return count;
        }
        const int packed = parsePackedWaypoint(value);
        if (!packedWaypoints.insert(packed).second)
        {
            throw std::runtime_error("Player waypoints must use unique coordinates");
        }
    }
}

std::vector<int> parseIntList(const std::string &value)
{
    std::vector<int> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (!item.empty())
        {
            result.push_back(std::stoi(item));
        }
    }
    return result;
}

std::vector<std::string> splitCommaList(const std::string &value)
{
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        result.push_back(item);
    }
    return result;
}

std::string lowerAscii(std::string value)
{
    for (char &ch : value)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

void requireBasicBool(const IniDocument &ini, const std::string &key)
{
    const std::string raw = ini.getString("Basic", key, "");
    if (raw.empty())
    {
        throw std::runtime_error("Basic." + key + " must be present");
    }
    const std::string value = lowerAscii(raw);
    if (value != "yes" && value != "no" && value != "true" && value != "false" && value != "1" && value != "0")
    {
        throw std::runtime_error("Basic." + key + " must be a boolean value");
    }
}

bool isResourceOverlayId(int overlay)
{
    return (overlay >= 27 && overlay <= 38) || (overlay >= 102 && overlay <= 113);
}

bool isAllowedOverlayId(int overlay)
{
    return overlay == 24 || overlay == 25 || isResourceOverlayId(overlay) || (overlay >= 168 && overlay <= 177);
}

/** @brief 校验阵营定义；纯地形按实际声明检查，不强制多人阵营顺序。 */
std::unordered_set<std::string> validateHouses(const IniDocument &ini, int players, bool declaredHouses = false)
{
    std::unordered_set<std::string> houses;
    const auto requireHouseSection = [&](const std::string &house)
    {
        if (!ini.hasSection(house))
        {
            throw std::runtime_error("missing house section: " + house);
        }
        if (ini.getInt(house, "TechLevel", -1) < 0)
        {
            throw std::runtime_error(house + ".TechLevel must be present and non-negative");
        }
        if (ini.getInt(house, "IQ", -1) < 0)
        {
            throw std::runtime_error(house + ".IQ must be present and non-negative");
        }
        if (ini.getString(house, "Edge", "").empty())
        {
            throw std::runtime_error(house + ".Edge must be present");
        }
        if (ini.getString(house, "Color", "").empty())
        {
            throw std::runtime_error(house + ".Color must be present");
        }
        if (ini.getString(house, "Country", "").empty())
        {
            throw std::runtime_error(house + ".Country must be present");
        }
        const std::string allies = ini.getString(house, "Allies", "");
        if (allies.empty())
        {
            throw std::runtime_error(house + ".Allies must be present");
        }
        bool selfAllied = false;
        for (const std::string &ally : splitCommaList(allies))
        {
            if (ally == house)
            {
                selfAllied = true;
                break;
            }
        }
        if (!selfAllied)
        {
            throw std::runtime_error(house + ".Allies must include itself");
        }
    };

    const auto requireHouse = [&](int index, const std::string &expected)
    {
        const std::string value = ini.getString("Houses", std::to_string(index), "");
        if (value != expected)
        {
            throw std::runtime_error("Houses." + std::to_string(index) + " must be " + expected);
        }
        houses.insert(value);
        requireHouseSection(value);
    };

    if (declaredHouses)
    {
        const auto entries = ini.sectionEntries("Houses");
        if (entries.empty())
        {
            throw std::runtime_error("Houses must declare at least one house");
        }
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto house = ini.getString("Houses", std::to_string(index), "");
            if (house.empty() || !houses.insert(house).second)
            {
                throw std::runtime_error("Houses contains sparse or duplicate declarations");
            }
            requireHouseSection(house);
        }
        return houses;
    }

    requireHouse(0, "Neutral");
    requireHouse(1, "Special");
    for (int i = 0; i < players; ++i)
    {
        requireHouse(i + 2, "Multi" + std::to_string(i + 1));
    }
    return houses;
}

void validateIsoCoordinate(int x, int y, const std::string &context)
{
    if (x < 0 || y < 0 || x >= 512 || y >= 512)
    {
        throw std::runtime_error(context + " is outside 512x512 iso bounds");
    }
}

/** @brief 判断 INI 项是否实际存在，空值不能作为缺省字段跳过校验。 */
bool hasIniKey(const IniDocument &ini, const std::string &section, const std::string &key)
{
    const auto entries = ini.sectionEntries(section);
    return std::any_of(entries.begin(), entries.end(), [&](const auto &entry)
                       {
                           return entry.first == lowerAscii(key);
                       });
}

/** @brief 校验新算法可选整数项，存在时必须为完整的非负整数。 */
int competitiveInteger(const IniDocument &ini, const std::string &section, const std::string &key)
{
    const auto raw = ini.getString(section, key, "");
    std::size_t length = 0;
    const int value = std::stoi(raw, &length);
    if (length != raw.size() || value < 0)
    {
        throw std::runtime_error(section + "." + key + " must be a non-negative integer");
    }
    return value;
}

/** @brief 校验光照值，仅纯地形允许省略 Dominator 项。 */
void validateLighting(const IniDocument &ini, bool competitiveFormat = false, bool terrainOnly = false)
{
    if (!ini.hasSection("Lighting"))
    {
        return;
    }

    const auto checkValue = [&](const std::string &key, double maximum)
    {
        if (terrainOnly && key.starts_with("Dominator") && !hasIniKey(ini, "Lighting", key))
        {
            return;
        }
        double value = ini.getDouble("Lighting", key, -1.0);
        if (competitiveFormat)
        {
            const auto raw = ini.getString("Lighting", key, "");
            std::size_t length = 0;
            value = std::stod(raw, &length);
            if (length != raw.size() || !std::isfinite(value))
            {
                throw std::runtime_error("Lighting." + key + " must be a finite RA2/YR double value");
            }
        }
        if (value < 0.0 || value > maximum)
        {
            throw std::runtime_error("Lighting." + key + " must be a normalized RA2/YR double value");
        }
    };
    for (const std::string key : {
             "Ambient",
             "Red",
             "Green",
             "Blue",
             "IonAmbient",
             "IonRed",
             "IonGreen",
             "IonBlue",
             "DominatorAmbient",
             "DominatorRed",
             "DominatorGreen",
             "DominatorBlue",
         })
    {
        checkValue(key, 2.0);
    }
    for (const std::string key : {
             "Ground",
             "Level",
             "IonGround",
             "IonLevel",
             "DominatorGround",
             "DominatorLevel",
             "DominatorAmbientChangeRate",
         })
    {
        checkValue(key, 1.0);
    }
}

std::uint16_t readU16LE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t readU32LE(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

int validateTerrainObjects(const IniDocument &ini)
{
    int count = 0;
    for (int packed = 0; packed < 512000; ++packed)
    {
        const std::string value = ini.getString("Terrain", std::to_string(packed), "");
        if (value.empty())
        {
            continue;
        }
        const int x = packed % 1000;
        const int y = packed / 1000;
        validateIsoCoordinate(x, y, "Terrain object");
        if (value.find(',') != std::string::npos)
        {
            throw std::runtime_error("Terrain object type must be a single token");
        }
        ++count;
    }
    return count;
}

int validateStructures(const IniDocument &ini, const std::unordered_set<std::string> &houses)
{
    int count = 0;
    for (;; ++count)
    {
        const std::string value = ini.getString("Structures", std::to_string(count), "");
        if (value.empty())
        {
            return count;
        }
        const auto fields = splitCommaList(value);
        if (fields.size() < 17)
        {
            throw std::runtime_error("Structure entry has too few fields");
        }
        if (fields[0].empty() || fields[1].empty())
        {
            throw std::runtime_error("Structure entry is missing owner or type");
        }
        if (!houses.contains(fields[0]))
        {
            throw std::runtime_error("Structure owner is not listed in [Houses]");
        }
        const int health = std::stoi(fields[2]);
        if (health <= 0 || health > 256)
        {
            throw std::runtime_error("Structure health is outside expected 1..256 range");
        }
        validateIsoCoordinate(std::stoi(fields[3]), std::stoi(fields[4]), "Structure");
    }
}

int validateUnits(const IniDocument &ini, const std::unordered_set<std::string> &houses)
{
    int count = 0;
    for (;; ++count)
    {
        const std::string value = ini.getString("Units", std::to_string(count), "");
        if (value.empty())
        {
            return count;
        }
        const auto fields = splitCommaList(value);
        if (fields.size() < 8)
        {
            throw std::runtime_error("Unit entry has too few fields");
        }
        if (fields[0].empty() || fields[1].empty())
        {
            throw std::runtime_error("Unit entry is missing owner or type");
        }
        if (!houses.contains(fields[0]))
        {
            throw std::runtime_error("Unit owner is not listed in [Houses]");
        }
        const int health = std::stoi(fields[2]);
        if (health <= 0 || health > 256)
        {
            throw std::runtime_error("Unit health is outside expected 1..256 range");
        }
        validateIsoCoordinate(std::stoi(fields[3]), std::stoi(fields[4]), "Unit");
    }
}

std::unordered_set<std::size_t> validatePlainOverlaySections(
    const IniDocument &ini,
    const std::vector<std::uint8_t> &overlayBytes,
    const std::vector<std::uint8_t> &overlayDataBytes,
    int &plainOverlayEntries)
{
    std::unordered_set<std::size_t> plainOverlayIndexes;
    plainOverlayEntries = 0;

    for (int packed = 0; packed < 512000; ++packed)
    {
        const std::string overlayRaw = ini.getString("Overlay", std::to_string(packed), "");
        const std::string dataRaw = ini.getString("OverlayData", std::to_string(packed), "");
        if (overlayRaw.empty() && dataRaw.empty())
        {
            continue;
        }
        if (overlayRaw.empty() || dataRaw.empty())
        {
            throw std::runtime_error("[Overlay] and [OverlayData] resource entries must match");
        }

        const int x = packed % 1000;
        const int y = packed / 1000;
        validateIsoCoordinate(x, y, "Overlay entry");
        const auto index = static_cast<std::size_t>(x + 512 * y);
        const int overlay = std::stoi(overlayRaw);
        const int data = std::stoi(dataRaw);
        if (!isAllowedOverlayId(overlay))
        {
            throw std::runtime_error("[Overlay] contains an unsupported overlay id");
        }
        if (data < 0 || data > 255)
        {
            throw std::runtime_error("[OverlayData] value must be in 0..255");
        }
        if (overlayBytes[index] != overlay)
        {
            throw std::runtime_error("plain [Overlay] entry does not match decoded OverlayPack data");
        }
        if (overlayDataBytes[index] != data)
        {
            throw std::runtime_error("plain [OverlayData] entry does not match decoded OverlayDataPack data");
        }
        plainOverlayIndexes.insert(index);
        ++plainOverlayEntries;
    }

    return plainOverlayIndexes;
}

void validateExpectedPlayers(int waypointCount, int expectedPlayers)
{
    if (expectedPlayers >= 0 && waypointCount != expectedPlayers)
    {
        throw std::runtime_error(
            "[Waypoints] count does not match requested players: expected=" + std::to_string(expectedPlayers) + " actual=" + std::to_string(waypointCount));
    }
}

void validateWaypointKeys(
    const IniDocument &ini,
    const std::vector<std::uint8_t> &bytes,
    int waypointCount,
    bool productFormat)
{
    const auto entries = ini.sectionEntries("Waypoints");
    const int expectedEntryCount = waypointCount + (productFormat ? 2 : 0);
    if (static_cast<int>(entries.size()) != expectedEntryCount)
    {
        throw std::runtime_error("[Waypoints] contains sparse, duplicate, or unexpected entries");
    }
    for (const auto &[key, value] : entries)
    {
        std::size_t parsedLength = 0;
        int index = -1;
        try
        {
            index = std::stoi(key, &parsedLength, 10);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error("[Waypoints] key must be a decimal index");
        }
        const bool playerWaypoint = parsedLength == key.size() && index >= 0 && index < waypointCount;
        const bool productHomeWaypoint = productFormat && parsedLength == key.size() && (index == 699 || index == 700);
        if (!playerWaypoint && !productHomeWaypoint)
        {
            throw std::runtime_error("[Waypoints] contains an unexpected key: " + key);
        }
        if (productHomeWaypoint)
        {
            parsePackedWaypoint(value);
        }
    }

    const auto sections = summarizeSections(bytes);
    const auto waypointSection = std::find_if(sections.begin(), sections.end(), [](const auto &section)
                                              {
                                                  return section.name == "Waypoints";
                                              });
    if (waypointSection == sections.end() || waypointSection->entryCount != expectedEntryCount)
    {
        throw std::runtime_error("[Waypoints] contains duplicate or invalid entries");
    }
}

int validateNativeProductStructures(const IniDocument &ini)
{
    if (!ini.hasSection("Structures"))
    {
        return 0;
    }

    const auto entries = ini.sectionEntries("Structures");
    if (entries.empty())
    {
        throw std::runtime_error("Origin product shell must omit an empty [Structures] section");
    }
    for (const auto &[key, value] : entries)
    {
        std::size_t parsedLength = 0;
        long long uniqueId = -1;
        try
        {
            uniqueId = std::stoll(key, &parsedLength, 10);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error("Origin product Structure key must be a non-negative decimal UniqueID");
        }
        if (parsedLength != key.size() || uniqueId < 0)
        {
            throw std::runtime_error("Origin product Structure key must be a non-negative decimal UniqueID");
        }

        const auto fields = splitCommaList(value);
        if (fields.size() < 17 || fields[0] != "Neutral" || fields[1].empty())
        {
            throw std::runtime_error("Origin product Structure must have a Neutral owner, type, and native fields");
        }
        const int health = std::stoi(fields[2]);
        if (health <= 0 || health > 256)
        {
            throw std::runtime_error("Origin product Structure health is outside expected 1..256 range");
        }
        validateIsoCoordinate(std::stoi(fields[3]), std::stoi(fields[4]), "Origin product Structure");
    }
    return static_cast<int>(entries.size());
}

int validateNativeProductShell(
    const IniDocument &ini,
    const std::vector<std::uint8_t> &bytes,
    int waypointCount)
{
    const auto sectionSummaries = summarizeSections(bytes);
    std::vector<std::string> actualSections;
    for (const auto &section : sectionSummaries)
    {
        actualSections.push_back(section.name);
    }
    const std::vector<std::string> requiredSections {
        "Header",
        "SpecialFlags",
        "Basic",
        "Ranking",
        "Lighting",
        "Houses",
        "<human player>",
        "Computer",
        "Neutral",
        "Special",
        "Map",
        "Waypoints",
        "IsoMapPack5",
        "Terrain",
        "OverlayPack",
        "OverlayDataPack",
        "AITriggerTypesEnable",
        "Digest",
    };

    std::size_t actualIndex = 0;
    for (const std::string &required : requiredSections)
    {
        if (actualIndex < actualSections.size() && actualSections[actualIndex] == "Structures")
        {
            if (required != "OverlayPack")
            {
                throw std::runtime_error("[Structures] is outside the Origin product shell order");
            }
            ++actualIndex;
        }
        if (actualIndex >= actualSections.size() || actualSections[actualIndex] != required)
        {
            throw std::runtime_error("Origin product shell section mismatch at: " + required);
        }
        ++actualIndex;
    }
    if (actualIndex != actualSections.size())
    {
        throw std::runtime_error("Origin product shell contains an unexpected or diagnostic section: " + actualSections[actualIndex]);
    }

    if (ini.getString("Basic", "Player", "") != "<human player>")
    {
        throw std::runtime_error("Basic.Player must be <human player> in Origin product shell");
    }
    if (ini.getInt("Basic", "NewINIFormat", 0) < 4)
    {
        throw std::runtime_error("Basic.NewINIFormat must be at least 4 for RA2/YR map pack sections");
    }
    if (ini.getInt("Basic", "InitTime", -1) < 0)
    {
        throw std::runtime_error("Basic.InitTime must be present and non-negative");
    }
    for (const std::string key : {
             "MultiplayerOnly",
             "TiberiumGrowthEnabled",
             "VeinGrowthEnabled",
             "IceGrowthEnabled",
             "TiberiumDeathToVisceroid",
             "FreeRadar",
         })
    {
        requireBasicBool(ini, key);
    }
    if (ini.getInt("Header", "Width", 0) <= 0 || ini.getInt("Header", "Height", 0) <= 0)
    {
        throw std::runtime_error("Origin product Header dimensions must be positive");
    }
    const auto localSize = parseIntList(ini.getString("Map", "LocalSize", ""));
    if (localSize.size() != 4 || localSize[2] <= 0 || localSize[3] <= 0 || ini.getString("Map", "Theater", "").empty() || !ini.getString("Map", "Level", "").empty())
    {
        throw std::runtime_error("Origin product Map shell is incomplete or contains preview-only Level");
    }
    for (const auto &[index, house] : std::vector<std::pair<int, std::string>> {
             {0, "<human player>"},
             {1, "Computer"},
             {2, "Neutral"},
             {3, "Special"},
         })
    {
        if (ini.getString("Houses", std::to_string(index), "") != house || !ini.hasSection(house))
        {
            throw std::runtime_error("Origin product shell house mismatch: " + house);
        }
        if (ini.getInt(house, "TechLevel", -1) < 0 || ini.getInt(house, "IQ", -1) < 0 || ini.getString(house, "Edge", "").empty() || ini.getString(house, "Country", "").empty() || ini.getString(house, "Allies", "") != house)
        {
            throw std::runtime_error("Origin product house section is incomplete: " + house);
        }
    }
    if (ini.getString("Waypoints", "699", "").empty() || ini.getString("Waypoints", "700", "").empty())
    {
        throw std::runtime_error("Origin product shell is missing home waypoints 699/700");
    }
    if (ini.getInt("Header", "NumberStartingPoints", -1) != waypointCount)
    {
        throw std::runtime_error("Header.NumberStartingPoints does not match [Waypoints]");
    }
    const auto aiTriggerSection = std::find_if(sectionSummaries.begin(), sectionSummaries.end(), [](const auto &section)
                                               {
                                                   return section.name == "AITriggerTypesEnable";
                                               });
    if (aiTriggerSection == sectionSummaries.end() || aiTriggerSection->entryCount != 165)
    {
        throw std::runtime_error("Origin product shell must contain 165 AITriggerTypesEnable entries");
    }

    static constexpr std::string_view digestDelimiter = "\r\n[Digest]\r\n";
    const auto delimiter = std::find_end(
        bytes.begin(),
        bytes.end(),
        digestDelimiter.begin(),
        digestDelimiter.end());
    if (delimiter == bytes.end())
    {
        throw std::runtime_error("Origin product Digest delimiter or CRLF shell is missing");
    }
    const std::size_t digestOffset = static_cast<std::size_t>(std::distance(bytes.begin(), delimiter));
    const auto encodedDigest = joinNumberedSection(ini, "Digest");
    const auto savedDigest = ra2::pack::base64Decode(encodedDigest);
    const auto digestSection = std::find_if(sectionSummaries.begin(), sectionSummaries.end(), [](const auto &section)
                                            {
                                                return section.name == "Digest";
                                            });
    if (digestSection == sectionSummaries.end() || digestSection->entryCount != 1 || savedDigest.size() != 20)
    {
        throw std::runtime_error("Origin product Digest must decode to 20 bytes");
    }
    const std::string_view digestInput(
        reinterpret_cast<const char *>(bytes.data()),
        digestOffset);
    const auto calculatedDigest = ra2::pack::sha1Digest(digestInput);
    if (!std::equal(savedDigest.begin(), savedDigest.end(), calculatedDigest.begin()))
    {
        throw std::runtime_error("Origin product Digest does not match saved map content");
    }
    const int structureCount = validateNativeProductStructures(ini);
    const auto structureSection = std::find_if(sectionSummaries.begin(), sectionSummaries.end(), [](const auto &section)
                                               {
                                                   return section.name == "Structures";
                                               });
    if (structureSection != sectionSummaries.end() && structureSection->entryCount != structureCount)
    {
        throw std::runtime_error("Origin product Structures contains duplicate or invalid entries");
    }
    return structureCount;
}

} // namespace

/** @brief 复用地图数据完整性校验，按产物类型检查对应的 INI 外壳。 */
YrIniValidationResult validateYrIniFileImpl(
    const std::filesystem::path &path,
    int expectedPlayers,
    bool productFormat,
    bool competitiveFormat = false)
{
    const bool terrainOnly = competitiveFormat && expectedPlayers == 0;
    const auto fileBytes = readFileBytes(path);
    const IniDocument ini = IniDocument::loadFile(path);
    for (const std::string section : {"Basic", "Header", "Houses", "Map", "Waypoints", "IsoMapPack5", "OverlayPack", "OverlayDataPack"})
    {
        if ((terrainOnly && section == "Header") || (competitiveFormat && (section == "OverlayPack" || section == "OverlayDataPack")))
        {
            continue;
        }
        if (!ini.hasSection(section))
        {
            throw std::runtime_error("missing required section: " + section);
        }
    }

    if (!productFormat)
    {
        if (ini.getInt("Basic", "NewINIFormat", 0) < 4)
        {
            throw std::runtime_error("Basic.NewINIFormat must be at least 4 for RA2/YR map pack sections");
        }
        if (ini.getInt("Basic", "InitTime", -1) < 0)
        {
            throw std::runtime_error("Basic.InitTime must be present and non-negative");
        }
        for (const std::string key : {
                 "RandomMap",
                 "MultiplayerOnly",
                 "TiberiumGrowthEnabled",
                 "VeinGrowthEnabled",
                 "IceGrowthEnabled",
                 "TiberiumDeathToVisceroid",
                 "FreeRadar",
             })
        {
            if (terrainOnly && key == "RandomMap" && !hasIniKey(ini, "Basic", key))
            {
                continue;
            }
            requireBasicBool(ini, key);
        }
    }

    const auto isoEncoded = joinNumberedSection(ini, "IsoMapPack5");
    const auto overlayEncoded = joinNumberedSection(ini, "OverlayPack");
    const auto overlayDataEncoded = joinNumberedSection(ini, "OverlayDataPack");
    const bool hasOverlay = ini.hasSection("OverlayPack");
    const bool hasOverlayData = ini.hasSection("OverlayDataPack");
    if (competitiveFormat && hasOverlay != hasOverlayData)
    {
        throw std::runtime_error("OverlayPack and OverlayDataPack must be present together");
    }
    const bool absentOverlay = competitiveFormat && !hasOverlay && !hasOverlayData;
    if (isoEncoded.empty() || (!absentOverlay && (overlayEncoded.empty() || overlayDataEncoded.empty())))
    {
        throw std::runtime_error("empty encoded pack section");
    }

    const auto mapSize = parseIntList(ini.getString("Map", "Size", ""));
    if (mapSize.size() != 4 || mapSize[2] <= 0 || mapSize[3] <= 0)
    {
        throw std::runtime_error("Map.Size must be x,y,width,height with positive dimensions");
    }
    if (!productFormat && (!terrainOnly || hasIniKey(ini, "Map", "Level")))
    {
        const int mapLevel = ini.getInt("Map", "Level", -1);
        if (mapLevel < 0)
        {
            throw std::runtime_error("Map.Level must be present and non-negative");
        }
    }
    if (competitiveFormat)
    {
        if (hasIniKey(ini, "Map", "Level"))
        {
            competitiveInteger(ini, "Map", "Level");
        }
        for (const auto &[key, expected] : std::vector<std::pair<std::string, int>> {
                 {"StartX", mapSize[0]}, {"StartY", mapSize[1]}, {"Width", mapSize[2]}, {"Height", mapSize[3]}, {"NumberStartingPoints", expectedPlayers}})
        {
            if ((!terrainOnly || hasIniKey(ini, "Header", key)) && competitiveInteger(ini, "Header", key) != expected)
            {
                throw std::runtime_error("Header." + key + " does not match the map or requested players");
            }
        }
        for (const std::string key : {"MinPlayer", "MaxPlayer"})
        {
            if (hasIniKey(ini, "Basic", key) && competitiveInteger(ini, "Basic", key) != expectedPlayers)
            {
                throw std::runtime_error("Basic." + key + " does not match requested players");
            }
        }
    }
    validateLighting(ini, competitiveFormat, terrainOnly);

    const auto isoBytes = ra2::pack::decodeFormat5Lzo(ra2::pack::base64Decode(isoEncoded));
    // 缺省覆盖物的语义与全空覆盖物一致，后续检查仍使用完整的 512x512 数据。
    const auto overlayBytes = absentOverlay ? std::vector<std::uint8_t>(1u << 18, 0xFF) : ra2::pack::decodeFormat5Format80(ra2::pack::base64Decode(overlayEncoded));
    const auto overlayDataBytes = absentOverlay ? std::vector<std::uint8_t>(1u << 18, 0) : ra2::pack::decodeFormat5Format80(ra2::pack::base64Decode(overlayDataEncoded));

    if (isoBytes.size() < 15 || (isoBytes.size() - 4) % 11 != 0)
    {
        throw std::runtime_error("IsoMapPack5 decoded payload is not N*11+4 bytes");
    }
    const int expectedIsoEntries = (mapSize[2] * 2 - 1) * mapSize[3];
    if (static_cast<int>((isoBytes.size() - 4) / 11) != expectedIsoEntries)
    {
        throw std::runtime_error("IsoMapPack5 tile count does not match Map.Size");
    }
    if (!std::all_of(isoBytes.end() - 4, isoBytes.end(), [](std::uint8_t value)
                     {
                         return value == 0;
                     }))
    {
        throw std::runtime_error("IsoMapPack5 decoded payload is missing the 4-byte terminator");
    }
    std::uint32_t maxTileNum = 0;
    int maxSubTile = 0;
    int nonZeroSubTileEntries = 0;
    int maxHeight = 0;
    int elevatedTileEntries = 0;
    int iceGrowthTileEntries = 0;
    for (std::size_t offset = 0; offset + 15 <= isoBytes.size(); offset += 11)
    {
        const std::uint16_t rx = readU16LE(isoBytes, offset);
        const std::uint16_t ry = readU16LE(isoBytes, offset + 2);
        const std::uint32_t tile = readU32LE(isoBytes, offset + 4);
        const int subTile = isoBytes[offset + 8];
        const int level = isoBytes[offset + 9];
        const int iceGrowth = isoBytes[offset + 10];
        if (rx >= 512 || ry >= 512)
        {
            throw std::runtime_error("IsoMapPack5 tile coordinate is outside 512x512 bounds");
        }
        if (iceGrowth != 0 && iceGrowth != 1)
        {
            throw std::runtime_error("IsoMapPack5 ice growth flag must be 0 or 1");
        }
        maxTileNum = std::max(maxTileNum, tile);
        maxSubTile = std::max(maxSubTile, subTile);
        maxHeight = std::max(maxHeight, level);
        if (subTile != 0)
        {
            ++nonZeroSubTileEntries;
        }
        if (level != 0)
        {
            ++elevatedTileEntries;
        }
        if (iceGrowth != 0)
        {
            ++iceGrowthTileEntries;
        }
    }
    if (overlayBytes.size() != (1u << 18))
    {
        throw std::runtime_error("OverlayPack decoded payload size is not 512*512");
    }
    if (overlayDataBytes.size() != (1u << 18))
    {
        throw std::runtime_error("OverlayDataPack decoded payload size is not 512*512");
    }
    const bool hasPlainOverlay = ini.hasSection("Overlay");
    const bool hasPlainOverlayData = ini.hasSection("OverlayData");
    if (!productFormat && hasPlainOverlay != hasPlainOverlayData)
    {
        throw std::runtime_error("plain [Overlay] and [OverlayData] sections must be present together");
    }

    int plainOverlayEntries = 0;
    const bool validatePlainOverlay = !productFormat && hasPlainOverlay;
    const auto plainOverlayIndexes = validatePlainOverlay ? validatePlainOverlaySections(ini, overlayBytes, overlayDataBytes, plainOverlayEntries) : std::unordered_set<std::size_t> {};
    int overlayEntries = 0;
    int bridgeOverlayEntries = 0;
    int intactBridgeOverlayEntries = 0;
    for (std::size_t i = 0; i < overlayBytes.size(); ++i)
    {
        if (overlayBytes[i] == 0xFF)
        {
            continue;
        }
        if (!isAllowedOverlayId(overlayBytes[i]))
        {
            throw std::runtime_error("OverlayPack contains an unsupported overlay id");
        }
        if (validatePlainOverlay && !plainOverlayIndexes.contains(i))
        {
            throw std::runtime_error("OverlayPack resource entry is missing from plain [Overlay] sections");
        }
        if (overlayBytes[i] == 24 || overlayBytes[i] == 25)
        {
            ++bridgeOverlayEntries;
            const bool intactEastWest = overlayBytes[i] == 24 && overlayDataBytes[i] <= 3;
            const bool intactNorthSouth = overlayBytes[i] == 25 && overlayDataBytes[i] >= 9 && overlayDataBytes[i] <= 12;
            if (intactEastWest || intactNorthSouth)
            {
                ++intactBridgeOverlayEntries;
            }
        }
        ++overlayEntries;
    }
    if (validatePlainOverlay && plainOverlayEntries != overlayEntries)
    {
        throw std::runtime_error("plain [Overlay] entry count does not match decoded OverlayPack entries");
    }

    YrIniValidationResult result;
    result.isoTileEntries = static_cast<int>((isoBytes.size() - 4) / 11);
    result.overlayBytes = static_cast<int>(overlayBytes.size());
    result.overlayDataBytes = static_cast<int>(overlayDataBytes.size());
    result.overlayEntries = overlayEntries;
    result.bridgeOverlayEntries = bridgeOverlayEntries;
    result.intactBridgeOverlayEntries = intactBridgeOverlayEntries;
    result.plainOverlayEntries = plainOverlayEntries;
    result.maxTileNum = maxTileNum;
    result.maxSubTile = maxSubTile;
    result.nonZeroSubTileEntries = nonZeroSubTileEntries;
    result.maxHeight = maxHeight;
    result.elevatedTileEntries = elevatedTileEntries;
    result.iceGrowthTileEntries = iceGrowthTileEntries;
    result.waypointCount = countWaypoints(ini);
    validateWaypointKeys(ini, fileBytes, result.waypointCount, productFormat);
    validateExpectedPlayers(result.waypointCount, expectedPlayers);
    const int headerStartingPoints = ini.getInt("Header", "NumberStartingPoints", result.waypointCount);
    if (headerStartingPoints != result.waypointCount)
    {
        throw std::runtime_error("Header.NumberStartingPoints does not match [Waypoints]");
    }
    if (productFormat)
    {
        result.structureCount = validateNativeProductShell(ini, fileBytes, result.waypointCount);
        result.unitCount = 0;
    }
    else
    {
        const auto houses = validateHouses(ini, headerStartingPoints, terrainOnly);
        const std::string basicPlayer = ini.getString("Basic", "Player", "");
        if (!basicPlayer.empty() && !houses.contains(basicPlayer))
        {
            throw std::runtime_error("Basic.Player is not listed in [Houses]");
        }
        result.structureCount = validateStructures(ini, houses);
        result.unitCount = validateUnits(ini, houses);
    }
    result.terrainObjectCount = validateTerrainObjects(ini);
    return result;
}

YrIniValidationResult validateYrIniFile(const std::filesystem::path &path, int expectedPlayers)
{
    return validateYrIniFileImpl(path, expectedPlayers, false);
}

YrIniValidationResult validateYrIniProductFile(const std::filesystem::path &path, int expectedPlayers)
{
    return validateYrIniFileImpl(path, expectedPlayers, true);
}

/** @brief 严格校验新算法的纯地形或平衡模式地图，不改变原生校验契约。 */
YrIniValidationResult validateCompetitiveYrIniFile(const std::filesystem::path &path, int expectedPlayers)
{
    if (expectedPlayers != 0 && (expectedPlayers < 2 || expectedPlayers > 8))
    {
        throw std::runtime_error("competitive players must be 0 or 2..8");
    }
    return validateYrIniFileImpl(path, expectedPlayers, false, true);
}

std::string describeValidation(const YrIniValidationResult &result)
{
    std::ostringstream out;
    out << "yrini valid: isoTileEntries=" << result.isoTileEntries
        << " overlayBytes=" << result.overlayBytes
        << " overlayDataBytes=" << result.overlayDataBytes
        << " overlayEntries=" << result.overlayEntries
        << " bridgeOverlayEntries=" << result.bridgeOverlayEntries
        << " intactBridgeOverlayEntries=" << result.intactBridgeOverlayEntries
        << " plainOverlayEntries=" << result.plainOverlayEntries
        << " maxTileNum=" << result.maxTileNum
        << " maxSubTile=" << result.maxSubTile
        << " nonZeroSubTiles=" << result.nonZeroSubTileEntries
        << " maxHeight=" << result.maxHeight
        << " elevatedTiles=" << result.elevatedTileEntries
        << " iceGrowthTiles=" << result.iceGrowthTileEntries
        << " waypoints=" << result.waypointCount
        << " structures=" << result.structureCount
        << " units=" << result.unitCount
        << " terrainObjects=" << result.terrainObjectCount;
    return out.str();
}

YrIniPackSignature signYrIniFile(const std::filesystem::path &path)
{
    const auto bytes = readFileBytes(path);
    const IniDocument ini = IniDocument::loadFile(path);

    YrIniPackSignature signature;
    signature.mapFile = hashPackBytes(bytes);
    signature.mapFileNoDigest = hashMapFileWithoutSections(bytes, true);
    signature.isoMapPack5 = hashEncodedSection(ini, "IsoMapPack5");
    signature.overlayPack = hashEncodedSection(ini, "OverlayPack");
    signature.overlayDataPack = hashEncodedSection(ini, "OverlayDataPack");
    signature.digest = hashEncodedSection(ini, "Digest");

    std::uint64_t hash = kFnvOffset;
    hashI32(hash, signature.isoMapPack5.length);
    for (const char ch : signature.isoMapPack5.hash)
    {
        hashByte(hash, static_cast<std::uint8_t>(ch));
    }
    hashI32(hash, signature.overlayPack.length);
    for (const char ch : signature.overlayPack.hash)
    {
        hashByte(hash, static_cast<std::uint8_t>(ch));
    }
    hashI32(hash, signature.overlayDataPack.length);
    for (const char ch : signature.overlayDataPack.hash)
    {
        hashByte(hash, static_cast<std::uint8_t>(ch));
    }

    signature.mapPacksCaptured = signature.isoMapPack5.captured && signature.overlayPack.captured && signature.overlayDataPack.captured;
    signature.mapPacksHash = hex64(hash);
    return signature;
}

std::string describeSignature(const YrIniPackSignature &signature)
{
    std::ostringstream out;
    out << "yrini signature:"
        << " map_file=" << signature.mapFile.hash << '/' << signature.mapFile.length
        << " map_file_no_digest=" << signature.mapFileNoDigest.hash << '/' << signature.mapFileNoDigest.length
        << " packs=" << signature.mapPacksHash
        << " iso5=" << signature.isoMapPack5.hash << '/' << signature.isoMapPack5.length
        << " overlay=" << signature.overlayPack.hash << '/' << signature.overlayPack.length
        << " overlay_data=" << signature.overlayDataPack.hash << '/' << signature.overlayDataPack.length
        << " digest=" << signature.digest.hash << '/' << signature.digest.length;
    return out.str();
}

YrIniInspection inspectYrIniFile(const std::filesystem::path &path)
{
    const auto bytes = readFileBytes(path);
    const IniDocument ini = IniDocument::loadFile(path);

    YrIniInspection inspection;
    inspection.sections = summarizeSections(bytes);
    inspection.signature = signYrIniFile(path);
    inspection.isoMapPack5 = summarizeEncodedSection(ini, "IsoMapPack5", true);
    inspection.overlayPack = summarizeEncodedSection(ini, "OverlayPack", false);
    inspection.overlayDataPack = summarizeEncodedSection(ini, "OverlayDataPack", false);

    inspection.headerStartX = ini.getInt("Header", "StartX", 0);
    inspection.headerStartY = ini.getInt("Header", "StartY", 0);
    inspection.headerWidth = ini.getInt("Header", "Width", 0);
    inspection.headerHeight = ini.getInt("Header", "Height", 0);

    const auto mapSize = parseIntList(ini.getString("Map", "Size", ""));
    if (mapSize.size() == 4)
    {
        inspection.mapSizeX = mapSize[0];
        inspection.mapSizeY = mapSize[1];
        inspection.mapSizeWidth = mapSize[2];
        inspection.mapSizeHeight = mapSize[3];
    }
    const auto localSize = parseIntList(ini.getString("Map", "LocalSize", ""));
    if (localSize.size() == 4)
    {
        inspection.mapLocalX = localSize[0];
        inspection.mapLocalY = localSize[1];
        inspection.mapLocalWidth = localSize[2];
        inspection.mapLocalHeight = localSize[3];
    }

    if (inspection.isoMapPack5.decoded.captured && inspection.isoMapPack5.decoded.length >= 4)
    {
        inspection.isoTileEntries = (inspection.isoMapPack5.decoded.length - 4) / 11;
    }

    const auto overlayEncoded = joinNumberedSection(ini, "OverlayPack");
    if (!overlayEncoded.empty())
    {
        const auto overlayBytes = ra2::pack::decodeFormat5Format80(ra2::pack::base64Decode(overlayEncoded));
        inspection.overlayEntries = static_cast<int>(std::count_if(
            overlayBytes.begin(),
            overlayBytes.end(),
            [](std::uint8_t value)
            {
                return value != 0xFF;
            }));
    }

    const auto overlayDataEncoded = joinNumberedSection(ini, "OverlayDataPack");
    if (!overlayDataEncoded.empty())
    {
        const auto overlayDataBytes = ra2::pack::decodeFormat5Format80(ra2::pack::base64Decode(overlayDataEncoded));
        inspection.overlayDataNonZeroEntries = static_cast<int>(std::count_if(
            overlayDataBytes.begin(),
            overlayDataBytes.end(),
            [](std::uint8_t value)
            {
                return value != 0;
            }));
    }

    return inspection;
}

std::string describeInspection(const YrIniInspection &inspection)
{
    std::ostringstream out;
    out << "yrini inspect:\n";
    out << "signature " << describeSignature(inspection.signature) << '\n';
    out << "header StartX=" << inspection.headerStartX
        << " StartY=" << inspection.headerStartY
        << " Width=" << inspection.headerWidth
        << " Height=" << inspection.headerHeight << '\n';
    out << "map Size=" << inspection.mapSizeX << ',' << inspection.mapSizeY << ',' << inspection.mapSizeWidth << ',' << inspection.mapSizeHeight
        << " LocalSize=" << inspection.mapLocalX << ',' << inspection.mapLocalY << ',' << inspection.mapLocalWidth << ',' << inspection.mapLocalHeight << '\n';
    out << "iso encoded=" << inspection.isoMapPack5.encoded.hash << '/' << inspection.isoMapPack5.encoded.length
        << " decoded=" << inspection.isoMapPack5.decoded.hash << '/' << inspection.isoMapPack5.decoded.length
        << " entries=" << inspection.isoTileEntries << '\n';
    out << "overlay encoded=" << inspection.overlayPack.encoded.hash << '/' << inspection.overlayPack.encoded.length
        << " decoded=" << inspection.overlayPack.decoded.hash << '/' << inspection.overlayPack.decoded.length
        << " entries=" << inspection.overlayEntries << '\n';
    out << "overlay_data encoded=" << inspection.overlayDataPack.encoded.hash << '/' << inspection.overlayDataPack.encoded.length
        << " decoded=" << inspection.overlayDataPack.decoded.hash << '/' << inspection.overlayDataPack.decoded.length
        << " non_zero=" << inspection.overlayDataNonZeroEntries << '\n';
    out << "sections";
    for (const auto &section : inspection.sections)
    {
        out << "\n  " << section.lineNumber << ':' << section.name << '=' << section.entryCount;
    }
    return out.str();
}

std::vector<std::uint8_t> decodeRequiredSection(const IniDocument &ini, const std::string &section, bool lzo)
{
    const auto encoded = joinNumberedSection(ini, section);
    if (encoded.empty())
    {
        throw std::runtime_error("missing or empty encoded section: " + section);
    }
    const auto encodedBytes = ra2::pack::base64Decode(encoded);
    return lzo ? ra2::pack::decodeFormat5Lzo(encodedBytes) : ra2::pack::decodeFormat5Format80(encodedBytes);
}

bool hasZeroTerminator(const std::vector<std::uint8_t> &bytes)
{
    return bytes.size() >= 4 && std::all_of(bytes.end() - 4, bytes.end(), [](std::uint8_t value)
                                            {
                                                return value == 0;
                                            });
}

bool hasMatchingTerminator(const std::vector<std::uint8_t> &left, const std::vector<std::uint8_t> &right)
{
    if (left.size() < 4 || right.size() < 4)
    {
        return false;
    }
    return std::equal(left.end() - 4, left.end(), right.end() - 4);
}

std::vector<YrIniIsoEntry> parseIsoEntries(const std::vector<std::uint8_t> &bytes)
{
    std::vector<YrIniIsoEntry> entries;
    if (bytes.size() < 4)
    {
        return entries;
    }

    const auto entryCount = static_cast<int>((bytes.size() - 4) / 11);
    entries.reserve(static_cast<std::size_t>(entryCount));
    for (int index = 0; index < entryCount; ++index)
    {
        const auto offset = static_cast<std::size_t>(index * 11);
        entries.push_back({
            readU16LE(bytes, offset),
            readU16LE(bytes, offset + 2),
            readU32LE(bytes, offset + 4),
            bytes[offset + 8],
            bytes[offset + 9],
            bytes[offset + 10],
        });
    }
    return entries;
}

std::vector<YrIniIsoEntry> readYrIniIsoEntries(const std::filesystem::path &path)
{
    const IniDocument ini = IniDocument::loadFile(path);
    return parseIsoEntries(decodeRequiredSection(ini, "IsoMapPack5", true));
}

std::vector<YrIniValueCountDiff> valueCountDiffs(
    const std::vector<YrIniIsoEntry> &leftEntries,
    const std::vector<YrIniIsoEntry> &rightEntries,
    int (*selector)(const YrIniIsoEntry &),
    std::size_t limit = 32)
{
    std::map<int, std::pair<int, int>> counts;
    for (const YrIniIsoEntry &entry : leftEntries)
    {
        ++counts[selector(entry)].first;
    }
    for (const YrIniIsoEntry &entry : rightEntries)
    {
        ++counts[selector(entry)].second;
    }

    std::vector<YrIniValueCountDiff> diffs;
    for (const auto &[value, count] : counts)
    {
        if (count.first != count.second)
        {
            diffs.push_back({value, count.first, count.second});
        }
    }

    std::sort(diffs.begin(), diffs.end(), [](const YrIniValueCountDiff &left, const YrIniValueCountDiff &right)
              {
                  const int leftDelta = std::abs(left.leftCount - left.rightCount);
                  const int rightDelta = std::abs(right.leftCount - right.rightCount);
                  if (leftDelta != rightDelta)
                  {
                      return leftDelta > rightDelta;
                  }
                  return left.value < right.value;
              });
    if (diffs.size() > limit)
    {
        diffs.resize(limit);
    }
    return diffs;
}

void updateValueExtent(YrIniValueExtent &extent, const YrIniIsoEntry &entry)
{
    if (!extent.captured)
    {
        extent.captured = true;
        extent.minRx = entry.rx;
        extent.minRy = entry.ry;
        extent.maxRx = entry.rx;
        extent.maxRy = entry.ry;
        extent.firstRx = entry.rx;
        extent.firstRy = entry.ry;
        return;
    }

    extent.minRx = std::min(extent.minRx, static_cast<int>(entry.rx));
    extent.minRy = std::min(extent.minRy, static_cast<int>(entry.ry));
    extent.maxRx = std::max(extent.maxRx, static_cast<int>(entry.rx));
    extent.maxRy = std::max(extent.maxRy, static_cast<int>(entry.ry));
}

std::vector<YrIniValueSpatialDiff> valueSpatialDiffs(
    const std::vector<YrIniIsoEntry> &leftEntries,
    const std::vector<YrIniIsoEntry> &rightEntries,
    int (*selector)(const YrIniIsoEntry &))
{
    struct SpatialCounts
    {
        int leftCount = 0;
        int rightCount = 0;
        YrIniValueExtent leftExtent;
        YrIniValueExtent rightExtent;
    };

    std::map<int, SpatialCounts> counts;
    for (const YrIniIsoEntry &entry : leftEntries)
    {
        SpatialCounts &count = counts[selector(entry)];
        ++count.leftCount;
        updateValueExtent(count.leftExtent, entry);
    }
    for (const YrIniIsoEntry &entry : rightEntries)
    {
        SpatialCounts &count = counts[selector(entry)];
        ++count.rightCount;
        updateValueExtent(count.rightExtent, entry);
    }

    std::vector<YrIniValueSpatialDiff> diffs;
    for (const auto &[value, count] : counts)
    {
        if (count.leftCount != count.rightCount)
        {
            diffs.push_back({
                value,
                count.leftCount,
                count.rightCount,
                count.leftExtent,
                count.rightExtent,
            });
        }
    }

    std::sort(diffs.begin(), diffs.end(), [](const YrIniValueSpatialDiff &left, const YrIniValueSpatialDiff &right)
              {
                  const int leftDelta = std::abs(left.leftCount - left.rightCount);
                  const int rightDelta = std::abs(right.leftCount - right.rightCount);
                  if (leftDelta != rightDelta)
                  {
                      return leftDelta > rightDelta;
                  }
                  return left.value < right.value;
              });
    if (diffs.size() > 32)
    {
        diffs.resize(32);
    }
    return diffs;
}

YrIniIsoComparison compareIsoMapPack5Bytes(
    const std::vector<std::uint8_t> &leftBytes,
    const std::vector<std::uint8_t> &rightBytes)
{
    YrIniIsoComparison comparison;
    comparison.leftDecoded = hashPackBytes(leftBytes);
    comparison.rightDecoded = hashPackBytes(rightBytes);
    comparison.leftReencoded = hashPackBytes(ra2::pack::encodeFormat5Lzo(leftBytes));
    comparison.rightReencoded = hashPackBytes(ra2::pack::encodeFormat5Lzo(rightBytes));
    comparison.decodedMatch = comparison.leftDecoded.captured && comparison.rightDecoded.captured && comparison.leftDecoded.length == comparison.rightDecoded.length && comparison.leftDecoded.hash == comparison.rightDecoded.hash;
    comparison.lengthMatch = leftBytes.size() == rightBytes.size();
    comparison.leftTerminatorOk = hasZeroTerminator(leftBytes);
    comparison.rightTerminatorOk = hasZeroTerminator(rightBytes);
    comparison.terminatorMatch = hasMatchingTerminator(leftBytes, rightBytes);

    const auto leftEntries = parseIsoEntries(leftBytes);
    const auto rightEntries = parseIsoEntries(rightBytes);
    comparison.leftEntries = static_cast<int>(leftEntries.size());
    comparison.rightEntries = static_cast<int>(rightEntries.size());
    comparison.entryCountMatch = leftEntries.size() == rightEntries.size();
    comparison.leftFirstEntries.assign(leftEntries.begin(), leftEntries.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(leftEntries.size(), 32)));
    comparison.rightFirstEntries.assign(rightEntries.begin(), rightEntries.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(rightEntries.size(), 32)));
    comparison.tileCountDiffs = valueCountDiffs(leftEntries, rightEntries, [](const YrIniIsoEntry &entry)
                                                {
                                                    return static_cast<int>(entry.tile);
                                                });
    comparison.tileSubTileCountDiffs = valueCountDiffs(leftEntries, rightEntries, [](const YrIniIsoEntry &entry)
                                                       {
                                                           return static_cast<int>(entry.tile) * 256 + entry.subTile;
                                                       },
                                                       128);
    comparison.subTileCountDiffs = valueCountDiffs(leftEntries, rightEntries, [](const YrIniIsoEntry &entry)
                                                   {
                                                       return entry.subTile;
                                                   });
    comparison.levelCountDiffs = valueCountDiffs(leftEntries, rightEntries, [](const YrIniIsoEntry &entry)
                                                 {
                                                     return entry.level;
                                                 });
    comparison.tileSpatialDiffs = valueSpatialDiffs(leftEntries, rightEntries, [](const YrIniIsoEntry &entry)
                                                    {
                                                        return static_cast<int>(entry.tile);
                                                    });

    const auto commonEntries = std::min(leftEntries.size(), rightEntries.size());
    for (std::size_t index = 0; index < commonEntries; ++index)
    {
        const auto &left = leftEntries[index];
        const auto &right = rightEntries[index];
        const bool coordinateMismatch = left.rx != right.rx || left.ry != right.ry;
        const bool tileMismatch = left.tile != right.tile;
        const bool subTileMismatch = left.subTile != right.subTile;
        const bool levelMismatch = left.level != right.level;
        const bool iceGrowthMismatch = left.iceGrowth != right.iceGrowth;

        if (coordinateMismatch)
        {
            ++comparison.coordinateMismatches;
        }
        if (tileMismatch)
        {
            ++comparison.tileMismatches;
        }
        if (subTileMismatch)
        {
            ++comparison.subTileMismatches;
        }
        if (levelMismatch)
        {
            ++comparison.levelMismatches;
        }
        if (iceGrowthMismatch)
        {
            ++comparison.iceGrowthMismatches;
        }
        if (!comparison.firstMismatch.captured && (coordinateMismatch || tileMismatch || subTileMismatch || levelMismatch || iceGrowthMismatch))
        {
            comparison.firstMismatch.captured = true;
            comparison.firstMismatch.index = static_cast<int>(index);
            comparison.firstMismatch.left = left;
            comparison.firstMismatch.right = right;
        }
        const auto captureFieldMismatch = [&](YrIniIsoFirstMismatch &mismatch)
        {
            if (!mismatch.captured)
            {
                mismatch.captured = true;
                mismatch.index = static_cast<int>(index);
                mismatch.left = left;
                mismatch.right = right;
            }
        };
        if (coordinateMismatch)
        {
            captureFieldMismatch(comparison.firstCoordinateMismatch);
        }
        if (tileMismatch)
        {
            captureFieldMismatch(comparison.firstTileMismatch);
        }
        if (subTileMismatch)
        {
            captureFieldMismatch(comparison.firstSubTileMismatch);
        }
        if (levelMismatch)
        {
            captureFieldMismatch(comparison.firstLevelMismatch);
        }
    }

    return comparison;
}

YrIniBytePackComparison compareBytePackBytes(
    const std::vector<std::uint8_t> &leftBytes,
    const std::vector<std::uint8_t> &rightBytes,
    std::uint8_t emptyValue)
{
    YrIniBytePackComparison comparison;
    comparison.leftDecoded = hashPackBytes(leftBytes);
    comparison.rightDecoded = hashPackBytes(rightBytes);
    comparison.leftReencoded = hashPackBytes(ra2::pack::encodeFormat5Format80(leftBytes));
    comparison.rightReencoded = hashPackBytes(ra2::pack::encodeFormat5Format80(rightBytes));
    comparison.decodedMatch = comparison.leftDecoded.captured && comparison.rightDecoded.captured && comparison.leftDecoded.length == comparison.rightDecoded.length && comparison.leftDecoded.hash == comparison.rightDecoded.hash;
    comparison.lengthMatch = leftBytes.size() == rightBytes.size();
    comparison.comparedBytes = static_cast<int>(std::min(leftBytes.size(), rightBytes.size()));

    const auto recordMismatch = [&](std::size_t index, int leftValue, int rightValue)
    {
        ++comparison.byteMismatches;
        if (!comparison.firstMismatch.captured)
        {
            comparison.firstMismatch.captured = true;
            comparison.firstMismatch.index = static_cast<int>(index);
            if (index < (1u << 18))
            {
                comparison.firstMismatch.x = static_cast<int>(index % 512);
                comparison.firstMismatch.y = static_cast<int>(index / 512);
            }
            comparison.firstMismatch.leftValue = leftValue;
            comparison.firstMismatch.rightValue = rightValue;
        }
    };

    const auto commonBytes = std::min(leftBytes.size(), rightBytes.size());
    for (std::size_t index = 0; index < commonBytes; ++index)
    {
        const bool leftOccupied = leftBytes[index] != emptyValue;
        const bool rightOccupied = rightBytes[index] != emptyValue;
        if (leftOccupied)
        {
            ++comparison.leftOccupied;
        }
        if (rightOccupied)
        {
            ++comparison.rightOccupied;
        }
        if (leftOccupied && rightOccupied)
        {
            ++comparison.occupiedBoth;
        }
        else if (leftOccupied)
        {
            ++comparison.leftOnly;
        }
        else if (rightOccupied)
        {
            ++comparison.rightOnly;
        }
        if (leftBytes[index] != rightBytes[index])
        {
            recordMismatch(index, leftBytes[index], rightBytes[index]);
        }
    }

    for (std::size_t index = commonBytes; index < leftBytes.size(); ++index)
    {
        if (leftBytes[index] != emptyValue)
        {
            ++comparison.leftOccupied;
            ++comparison.leftOnly;
        }
        recordMismatch(index, leftBytes[index], -1);
    }
    for (std::size_t index = commonBytes; index < rightBytes.size(); ++index)
    {
        if (rightBytes[index] != emptyValue)
        {
            ++comparison.rightOccupied;
            ++comparison.rightOnly;
        }
        recordMismatch(index, -1, rightBytes[index]);
    }

    return comparison;
}

std::string yesNo(bool value)
{
    return value ? "yes" : "no";
}

void describeIsoEntry(std::ostringstream &out, const YrIniIsoEntry &left, const YrIniIsoEntry &right)
{
    out << " rx=" << left.rx << "->" << right.rx
        << " ry=" << left.ry << "->" << right.ry
        << " tile=" << left.tile << "->" << right.tile
        << " subTile=" << left.subTile << "->" << right.subTile
        << " level=" << left.level << "->" << right.level
        << " iceGrowth=" << left.iceGrowth << "->" << right.iceGrowth;
}

void describeIsoMismatch(std::ostringstream &out, const char *label, const YrIniIsoFirstMismatch &mismatch)
{
    out << "iso " << label << '=';
    if (mismatch.captured)
    {
        out << "index=" << mismatch.index;
        describeIsoEntry(out, mismatch.left, mismatch.right);
    }
    else
    {
        out << "none";
    }
    out << '\n';
}

void describeIsoEntrySamples(
    std::ostringstream &out,
    const char *label,
    const std::vector<YrIniIsoEntry> &entries)
{
    out << "iso " << label << '=';
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (i > 0)
        {
            out << ';';
        }
        out << i << ':' << entries[i].rx << ',' << entries[i].ry
            << ',' << entries[i].tile
            << ',' << entries[i].subTile
            << ',' << entries[i].level;
    }
    if (entries.empty())
    {
        out << "none";
    }
    out << '\n';
}

void describeValueCountDiffs(
    std::ostringstream &out,
    const char *label,
    const std::vector<YrIniValueCountDiff> &diffs)
{
    out << "iso " << label << '=';
    for (std::size_t i = 0; i < diffs.size(); ++i)
    {
        if (i > 0)
        {
            out << ';';
        }
        out << diffs[i].value << ':' << diffs[i].leftCount << "->" << diffs[i].rightCount;
    }
    if (diffs.empty())
    {
        out << "none";
    }
    out << '\n';
}

void describeTileSubTileCountDiffs(
    std::ostringstream &out,
    const std::vector<YrIniValueCountDiff> &diffs)
{
    out << "iso tile_subtile_count_diffs=";
    for (std::size_t i = 0; i < diffs.size(); ++i)
    {
        if (i > 0)
        {
            out << ';';
        }
        out << (diffs[i].value / 256) << '/' << (diffs[i].value % 256)
            << ':' << diffs[i].leftCount << "->" << diffs[i].rightCount;
    }
    if (diffs.empty())
    {
        out << "none";
    }
    out << '\n';
}

void describeValueExtent(std::ostringstream &out, const YrIniValueExtent &extent)
{
    if (!extent.captured)
    {
        out << "none";
        return;
    }

    out << extent.minRx << ',' << extent.minRy
        << ".." << extent.maxRx << ',' << extent.maxRy
        << " first=" << extent.firstRx << ',' << extent.firstRy;
}

void describeValueSpatialDiffs(
    std::ostringstream &out,
    const char *label,
    const std::vector<YrIniValueSpatialDiff> &diffs)
{
    out << "iso " << label << '=';
    for (std::size_t i = 0; i < diffs.size(); ++i)
    {
        if (i > 0)
        {
            out << ';';
        }
        out << diffs[i].value << ':' << diffs[i].leftCount << '[';
        describeValueExtent(out, diffs[i].leftExtent);
        out << "]->" << diffs[i].rightCount << '[';
        describeValueExtent(out, diffs[i].rightExtent);
        out << ']';
    }
    if (diffs.empty())
    {
        out << "none";
    }
    out << '\n';
}

void describeBytePack(
    std::ostringstream &out,
    const char *name,
    const YrIniBytePackComparison &comparison)
{
    out << name << " decoded left=" << comparison.leftDecoded.hash << '/' << comparison.leftDecoded.length
        << " right=" << comparison.rightDecoded.hash << '/' << comparison.rightDecoded.length
        << " reencoded=" << comparison.leftReencoded.length << "->" << comparison.rightReencoded.length
        << " match=" << yesNo(comparison.decodedMatch)
        << " length_match=" << yesNo(comparison.lengthMatch)
        << " compared_bytes=" << comparison.comparedBytes
        << " byte_mismatches=" << comparison.byteMismatches
        << " occupied left=" << comparison.leftOccupied
        << " right=" << comparison.rightOccupied
        << " both=" << comparison.occupiedBoth
        << " left_only=" << comparison.leftOnly
        << " right_only=" << comparison.rightOnly << '\n';
    out << name << " first_mismatch=";
    if (comparison.firstMismatch.captured)
    {
        out << "index=" << comparison.firstMismatch.index
            << " x=" << comparison.firstMismatch.x
            << " y=" << comparison.firstMismatch.y
            << " value=" << comparison.firstMismatch.leftValue << "->" << comparison.firstMismatch.rightValue;
    }
    else
    {
        out << "none";
    }
    out << '\n';
}

YrIniComparison compareYrIniFiles(const std::filesystem::path &left, const std::filesystem::path &right)
{
    const IniDocument leftIni = IniDocument::loadFile(left);
    const IniDocument rightIni = IniDocument::loadFile(right);

    const auto leftIso = decodeRequiredSection(leftIni, "IsoMapPack5", true);
    const auto rightIso = decodeRequiredSection(rightIni, "IsoMapPack5", true);
    const auto leftOverlay = decodeRequiredSection(leftIni, "OverlayPack", false);
    const auto rightOverlay = decodeRequiredSection(rightIni, "OverlayPack", false);
    const auto leftOverlayData = decodeRequiredSection(leftIni, "OverlayDataPack", false);
    const auto rightOverlayData = decodeRequiredSection(rightIni, "OverlayDataPack", false);

    YrIniComparison comparison;
    comparison.leftSignature = signYrIniFile(left);
    comparison.rightSignature = signYrIniFile(right);
    comparison.isoMapPack5 = compareIsoMapPack5Bytes(leftIso, rightIso);
    comparison.overlayPack = compareBytePackBytes(leftOverlay, rightOverlay, 0xFF);
    comparison.overlayDataPack = compareBytePackBytes(leftOverlayData, rightOverlayData, 0);
    comparison.overallMatch = comparison.isoMapPack5.decodedMatch && comparison.overlayPack.decodedMatch && comparison.overlayDataPack.decodedMatch;
    return comparison;
}

std::string describeComparison(const YrIniComparison &comparison)
{
    std::ostringstream out;
    out << "yrini compare:\n";
    out << "left_signature " << describeSignature(comparison.leftSignature) << '\n';
    out << "right_signature " << describeSignature(comparison.rightSignature) << '\n';

    const auto &iso = comparison.isoMapPack5;
    out << "iso decoded left=" << iso.leftDecoded.hash << '/' << iso.leftDecoded.length
        << " right=" << iso.rightDecoded.hash << '/' << iso.rightDecoded.length
        << " reencoded=" << iso.leftReencoded.length << "->" << iso.rightReencoded.length
        << " match=" << yesNo(iso.decodedMatch)
        << " length_match=" << yesNo(iso.lengthMatch)
        << " entries=" << iso.leftEntries << "->" << iso.rightEntries
        << " entry_count_match=" << yesNo(iso.entryCountMatch)
        << " terminator_left=" << yesNo(iso.leftTerminatorOk)
        << " terminator_right=" << yesNo(iso.rightTerminatorOk)
        << " terminator_match=" << yesNo(iso.terminatorMatch) << '\n';
    out << "iso mismatches coordinate=" << iso.coordinateMismatches
        << " tile=" << iso.tileMismatches
        << " subTile=" << iso.subTileMismatches
        << " level=" << iso.levelMismatches
        << " iceGrowth=" << iso.iceGrowthMismatches << '\n';
    describeIsoEntrySamples(out, "left_first_entries", iso.leftFirstEntries);
    describeIsoEntrySamples(out, "right_first_entries", iso.rightFirstEntries);
    describeValueCountDiffs(out, "tile_count_diffs", iso.tileCountDiffs);
    describeTileSubTileCountDiffs(out, iso.tileSubTileCountDiffs);
    describeValueCountDiffs(out, "subtile_count_diffs", iso.subTileCountDiffs);
    describeValueCountDiffs(out, "level_count_diffs", iso.levelCountDiffs);
    describeValueSpatialDiffs(out, "tile_spatial_diffs", iso.tileSpatialDiffs);
    out << "iso first_mismatch=";
    if (iso.firstMismatch.captured)
    {
        out << "index=" << iso.firstMismatch.index;
        describeIsoEntry(out, iso.firstMismatch.left, iso.firstMismatch.right);
    }
    else
    {
        out << "none";
    }
    out << '\n';
    describeIsoMismatch(out, "first_coordinate_mismatch", iso.firstCoordinateMismatch);
    describeIsoMismatch(out, "first_tile_mismatch", iso.firstTileMismatch);
    describeIsoMismatch(out, "first_subtile_mismatch", iso.firstSubTileMismatch);
    describeIsoMismatch(out, "first_level_mismatch", iso.firstLevelMismatch);

    describeBytePack(out, "overlay", comparison.overlayPack);
    describeBytePack(out, "overlay_data", comparison.overlayDataPack);
    out << "overall_match=" << yesNo(comparison.overallMatch);
    return out.str();
}

} // namespace rmg
