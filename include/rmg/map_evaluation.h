#pragma once

#include "rmg/map_types.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rmg
{

inline constexpr int kMapEvaluationSchemaVersion = 2;
inline constexpr const char *kMapEvaluatorVersion = "ra2-pvp-v2";

struct MapEvaluationContext
{
    std::uint32_t seed = 0;
    int requestedPlayers = 0;
    int tiberium = 0;
};

struct EvaluationDomainScore
{
    bool availabilityApplicable = false;
    int availability = 0;
    bool parityApplicable = false;
    int parity = 0;
    int confidence = 0;
};

struct PlayerMapEvaluation
{
    int player = 0;
    Point start;
    int localBuildableCells = 0;
    int connectedBuildableCells = 0;
    int componentPassableCells = 0;
    int componentBuildableCells = 0;
    int territoryCells = 0;
    int distanceToEdge = 0;
    int nearestStartDistance = -1;
    int nearestGroundStartDistance = -1;
    int nearestResourceDistance = -1;
    int averageResourceDistance = -1;
    int resourceValue12 = 0;
    int resourceValue24 = 0;
    int resourceValue34 = 0;
    int exclusiveResourceValue = 0;
    int contestedResourceValue = 0;
    int componentId = -1;
};

struct MapEvaluationObservation
{
    std::string code;
    std::string impact;
    std::vector<int> players;
};

struct StrategyFitScore
{
    bool applicable = false;
    int score = 0;
    int confidence = 0;
};

struct MapStrategyFitEvaluation
{
    bool applicable = false;
    int score = 0;
    int confidence = 0;

    StrategyFitScore expansionEconomy;
    int expansionFillRatioPermille = 0;
    int minimumExpansionOwnershipLead = -1;

    StrategyFitScore strategicObjectives;
    int plannedObjectiveSites = 0;
    int placedObjectiveSites = 0;
    int minimumObjectiveApproachClearCells = 0;

    StrategyFitScore navalMobility;
    int navigableWaterComponents = 0;
    int navalLaunchSiteCount = 0;
    int navalLaunchViolations = 0;
    int maximumCenterSeaDistanceSpread = 0;

    StrategyFitScore symmetry;
    int symmetryMismatchCount = 0;
};

struct MapEvaluationReport
{
    int schemaVersion = kMapEvaluationSchemaVersion;
    std::string evaluatorVersion = kMapEvaluatorVersion;
    std::uint32_t seed = 0;
    int width = 0;
    int height = 0;
    int requestedPlayers = 0;
    int evaluatedPlayers = 0;
    int playability = 0;
    int fairness = 0;
    int confidence = 0;
    EvaluationDomainScore baseSpace;
    EvaluationDomainScore startingEconomy;
    EvaluationDomainScore spawnGeometry;
    EvaluationDomainScore groundMobility;
    std::string topology = "unknown";
    std::string pace = "unknown";
    int groundContactRatio = 0;
    int landFragmentation = 0;
    int resourceCentralization = 0;
    int mapUtilization = 0;
    int contestedTerritoryCells = 0;
    bool expansionEconomyCovered = false;
    bool strategicObjectivesCovered = false;
    bool navalMobilityCovered = false;
    MapStrategyFitEvaluation strategyFit;
    std::vector<PlayerMapEvaluation> players;
    std::vector<MapEvaluationObservation> observations;
};

MapEvaluationReport evaluateMap(
    const MapGrid &map,
    std::span<const Point> starts,
    const MapEvaluationContext &context);

std::string serializeMapEvaluation(const MapEvaluationReport &report);
void writeMapEvaluationFile(const std::filesystem::path &path, const MapEvaluationReport &report);
void verifyMapEvaluation();

} // namespace rmg
