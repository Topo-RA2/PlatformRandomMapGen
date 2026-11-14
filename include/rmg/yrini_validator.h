#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rmg
{

struct YrIniValidationResult
{
    int isoTileEntries = 0;
    int overlayBytes = 0;
    int overlayDataBytes = 0;
    int overlayEntries = 0;
    int bridgeOverlayEntries = 0;
    int intactBridgeOverlayEntries = 0;
    int plainOverlayEntries = 0;
    std::uint32_t maxTileNum = 0;
    int maxSubTile = 0;
    int nonZeroSubTileEntries = 0;
    int maxHeight = 0;
    int elevatedTileEntries = 0;
    int iceGrowthTileEntries = 0;
    int waypointCount = 0;
    int structureCount = 0;
    int unitCount = 0;
    int terrainObjectCount = 0;
};

struct YrIniPackFingerprint
{
    bool captured = false;
    int length = 0;
    std::string hash;
};

struct YrIniPackSignature
{
    YrIniPackFingerprint mapFile;
    YrIniPackFingerprint mapFileNoDigest;
    YrIniPackFingerprint isoMapPack5;
    YrIniPackFingerprint overlayPack;
    YrIniPackFingerprint overlayDataPack;
    YrIniPackFingerprint digest;
    bool mapPacksCaptured = false;
    std::string mapPacksHash;
};

struct YrIniSectionSummary
{
    std::string name;
    int lineNumber = 0;
    int entryCount = 0;
};

struct YrIniPackSummary
{
    YrIniPackFingerprint encoded;
    YrIniPackFingerprint decoded;
};

struct YrIniInspection
{
    std::vector<YrIniSectionSummary> sections;
    YrIniPackSignature signature;
    YrIniPackSummary isoMapPack5;
    YrIniPackSummary overlayPack;
    YrIniPackSummary overlayDataPack;
    int headerStartX = 0;
    int headerStartY = 0;
    int headerWidth = 0;
    int headerHeight = 0;
    int mapSizeX = 0;
    int mapSizeY = 0;
    int mapSizeWidth = 0;
    int mapSizeHeight = 0;
    int mapLocalX = 0;
    int mapLocalY = 0;
    int mapLocalWidth = 0;
    int mapLocalHeight = 0;
    int isoTileEntries = 0;
    int overlayEntries = 0;
    int overlayDataNonZeroEntries = 0;
};

struct YrIniIsoEntry
{
    std::uint16_t rx = 0;
    std::uint16_t ry = 0;
    std::uint32_t tile = 0;
    int subTile = 0;
    int level = 0;
    int iceGrowth = 0;
};

struct YrIniIsoFirstMismatch
{
    bool captured = false;
    int index = -1;
    YrIniIsoEntry left;
    YrIniIsoEntry right;
};

struct YrIniValueCountDiff
{
    int value = 0;
    int leftCount = 0;
    int rightCount = 0;
};

struct YrIniValueExtent
{
    bool captured = false;
    int minRx = 0;
    int minRy = 0;
    int maxRx = 0;
    int maxRy = 0;
    int firstRx = 0;
    int firstRy = 0;
};

struct YrIniValueSpatialDiff
{
    int value = 0;
    int leftCount = 0;
    int rightCount = 0;
    YrIniValueExtent leftExtent;
    YrIniValueExtent rightExtent;
};

struct YrIniIsoComparison
{
    YrIniPackFingerprint leftDecoded;
    YrIniPackFingerprint rightDecoded;
    YrIniPackFingerprint leftReencoded;
    YrIniPackFingerprint rightReencoded;
    int leftEntries = 0;
    int rightEntries = 0;
    bool decodedMatch = false;
    bool lengthMatch = false;
    bool entryCountMatch = false;
    bool leftTerminatorOk = false;
    bool rightTerminatorOk = false;
    bool terminatorMatch = false;
    std::vector<YrIniIsoEntry> leftFirstEntries;
    std::vector<YrIniIsoEntry> rightFirstEntries;
    int coordinateMismatches = 0;
    int tileMismatches = 0;
    int subTileMismatches = 0;
    int levelMismatches = 0;
    int iceGrowthMismatches = 0;
    std::vector<YrIniValueCountDiff> tileCountDiffs;
    std::vector<YrIniValueCountDiff> tileSubTileCountDiffs;
    std::vector<YrIniValueCountDiff> subTileCountDiffs;
    std::vector<YrIniValueCountDiff> levelCountDiffs;
    std::vector<YrIniValueSpatialDiff> tileSpatialDiffs;
    YrIniIsoFirstMismatch firstMismatch;
    YrIniIsoFirstMismatch firstCoordinateMismatch;
    YrIniIsoFirstMismatch firstTileMismatch;
    YrIniIsoFirstMismatch firstSubTileMismatch;
    YrIniIsoFirstMismatch firstLevelMismatch;
};

struct YrIniByteFirstMismatch
{
    bool captured = false;
    int index = -1;
    int x = -1;
    int y = -1;
    int leftValue = -1;
    int rightValue = -1;
};

struct YrIniBytePackComparison
{
    YrIniPackFingerprint leftDecoded;
    YrIniPackFingerprint rightDecoded;
    YrIniPackFingerprint leftReencoded;
    YrIniPackFingerprint rightReencoded;
    bool decodedMatch = false;
    bool lengthMatch = false;
    int comparedBytes = 0;
    int byteMismatches = 0;
    int leftOccupied = 0;
    int rightOccupied = 0;
    int occupiedBoth = 0;
    int leftOnly = 0;
    int rightOnly = 0;
    YrIniByteFirstMismatch firstMismatch;
};

struct YrIniComparison
{
    YrIniPackSignature leftSignature;
    YrIniPackSignature rightSignature;
    YrIniIsoComparison isoMapPack5;
    YrIniBytePackComparison overlayPack;
    YrIniBytePackComparison overlayDataPack;
    bool overallMatch = false;
};

YrIniValidationResult validateYrIniFile(const std::filesystem::path &path, int expectedPlayers = -1);
YrIniValidationResult validateYrIniProductFile(const std::filesystem::path &path, int expectedPlayers = -1);
// 校验新算法地图，零玩家表示纯地形，2..8 表示平衡模式。
YrIniValidationResult validateCompetitiveYrIniFile(const std::filesystem::path &path, int expectedPlayers);
std::string describeValidation(const YrIniValidationResult &result);
YrIniPackSignature signYrIniFile(const std::filesystem::path &path);
std::string describeSignature(const YrIniPackSignature &signature);
YrIniInspection inspectYrIniFile(const std::filesystem::path &path);
std::string describeInspection(const YrIniInspection &inspection);
std::vector<YrIniIsoEntry> readYrIniIsoEntries(const std::filesystem::path &path);
YrIniComparison compareYrIniFiles(const std::filesystem::path &left, const std::filesystem::path &right);
std::string describeComparison(const YrIniComparison &comparison);

} // namespace rmg
