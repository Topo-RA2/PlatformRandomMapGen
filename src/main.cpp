#include "rmg/cliff_pieces.h"
#include "rmg/generation_error.h"
#include "rmg/ini_config.h"
#include "rmg/map_evaluation.h"
#include "rmg/map_writer.h"
#include "rmg/native_game_options.h"
#include "rmg/native_generator.h"
#include "rmg/native_theater_catalog.h"
#include "pack_codec/pack_encoding.h"
#include "rmg/shore_pieces.h"
#include "rmg/yrini_validator.h"
#include "rmg/tile_catalog.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef RMG_GENERATOR_VERSION
#define RMG_GENERATOR_VERSION "1.0.0"
#endif

namespace
{

constexpr std::string_view kGeneratorVersion = "RandomMapGen " RMG_GENERATOR_VERSION;

struct CliOptions
{
    std::string mode = "native";
    std::uint32_t seed = 1;
    int width = 80;
    int height = 80;
    int players = 2;
    int theater = 0;
    int mapType = 1;
    int resources = 1;
    int time = 1;
    int waterAmount = 1;
    int tiberium = 4;
    int tiberiumLayout = 0;
    int vegetation = 2;
    int ruggedness = 1;
    int urbanPresence = 0;
    int accessibility = 0;
    int regionSize = 1;
    int terrainType = -1;
    std::uint32_t randomizationSeed = 0;
    int gameTerrainType = 1;
    int timeOfDay = 0;
    int climate = 0;
    int mapSize = 0;
    std::filesystem::path output = "random.map.txt";
    std::filesystem::path productOutput;
    std::filesystem::path evaluationReportOutput;
    std::filesystem::path errorReportOutput;
    std::filesystem::path nativeOptionsReportOutput;
    std::string nativeOptionsReportHex;
    std::filesystem::path input;
    std::filesystem::path compare;
    std::string format = "debug";
    std::filesystem::path sedPath;
    std::filesystem::path rmgConfigPath;
    std::filesystem::path theaterIniPath;
    std::filesystem::path tileCatalogPath;
    std::filesystem::path gameResourceDir;
    std::filesystem::path nativeStructuresTracePath;
    std::filesystem::path nativeTechTracePath;
    int nativeScenarioUniqueId = -1;
    std::string tilePreset;
    bool hasSeed = false;
    bool hasWidth = false;
    bool hasHeight = false;
    bool hasPlayers = false;
    bool hasTheater = false;
    bool hasMapType = false;
    bool hasResources = false;
    bool hasTime = false;
    bool hasWaterAmount = false;
    bool hasTiberium = false;
    bool hasTiberiumLayout = false;
    bool hasVegetation = false;
    bool hasRuggedness = false;
    bool hasUrbanPresence = false;
    bool hasAccessibility = false;
    bool hasRegionSize = false;
    bool hasTerrainType = false;
    bool hasRandomizationSeed = false;
    bool hasGameTerrainType = false;
    bool hasTimeOfDay = false;
    bool hasClimate = false;
    bool hasMapSize = false;
    bool hasNativeScenarioUniqueId = false;
    bool hasOutput = false;
    bool hasProductOutput = false;
    bool hasEvaluationReportOutput = false;
    bool hasErrorReportOutput = false;
    bool hasNativeOptionsReportOutput = false;
    bool hasNativeOptionsReportHex = false;
    bool hasSed = false;
    bool showVersion = false;
};

std::string decodeLowerHexText(std::string_view value)
{
    if (value.empty() || value.size() % 2 != 0 || value.size() > rmg::kMaximumNativeGameOptionsReportBytes * 2)
    {
        throw std::runtime_error(
            "--native-options-report-hex must contain 1..8192 bytes as lowercase hexadecimal");
    }

    auto hexDigit = [](char character) -> int
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f')
        {
            return character - 'a' + 10;
        }
        return -1;
    };

    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2)
    {
        const int high = hexDigit(value[index]);
        const int low = hexDigit(value[index + 1]);
        if (high < 0 || low < 0)
        {
            throw std::runtime_error(
                "--native-options-report-hex must contain 1..8192 bytes as lowercase hexadecimal");
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

void removeOutputFile(const std::filesystem::path &output)
{
    std::error_code removeError;
    std::filesystem::remove(output, removeError);
}

std::filesystem::path findErrorReportOutput(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--error-report-out")
        {
            return argv[i + 1];
        }
    }
    return {};
}

std::string escapeJsonString(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                escaped += "\\u00";
                escaped.push_back(hex[(ch >> 4) & 0x0f]);
                escaped.push_back(hex[ch & 0x0f]);
            }
            else
            {
                escaped.push_back(static_cast<char>(ch));
            }
        }
    }
    return escaped;
}

void writeErrorReport(
    const std::filesystem::path &output,
    std::string_view code,
    std::string_view detail) noexcept
{
    if (output.empty())
    {
        return;
    }
    try
    {
        const std::filesystem::path temporary = output.string() + ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return;
        }
        stream << "{\"schemaVersion\":1,\"code\":\"" << escapeJsonString(code)
               << "\",\"detail\":\"" << escapeJsonString(detail) << "\"}\n";
        stream.flush();
        if (!stream)
        {
            removeOutputFile(temporary);
            return;
        }
        stream.close();
        if (!stream)
        {
            removeOutputFile(temporary);
            return;
        }
        std::error_code error;
        std::filesystem::rename(temporary, output, error);
        if (error)
        {
            removeOutputFile(temporary);
        }
    }
    catch (...)
    {
    }
}

std::filesystem::path resolvedOutputPath(const std::filesystem::path &output)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(output, error);
    if (error)
    {
        throw std::runtime_error("failed to resolve output path: " + output.string());
    }

    // weakly_canonical does not resolve a final symlink whose target does not
    // exist yet. Follow that component explicitly so two dangling links to the
    // same output cannot bypass the dual-output alias check.
    for (int depth = 0; depth < 40; ++depth)
    {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(absolute, statusError);
        if (statusError == std::errc::no_such_file_or_directory)
        {
            break;
        }
        if (statusError)
        {
            throw std::runtime_error("failed to inspect output path: " + output.string());
        }
        if (!std::filesystem::is_symlink(status))
        {
            break;
        }

        const auto target = std::filesystem::read_symlink(absolute, error);
        if (error)
        {
            throw std::runtime_error("failed to resolve output symlink: " + output.string());
        }
        absolute = target.is_absolute() ? target : absolute.parent_path() / target;
        if (depth == 39)
        {
            throw std::runtime_error("too many output symlink levels: " + output.string());
        }
    }

    const auto resolved = std::filesystem::weakly_canonical(absolute, error);
    if (error)
    {
        throw std::runtime_error("failed to canonicalize output path: " + output.string());
    }
    return resolved.lexically_normal();
}

void ensureOutputParentDirectory(const std::filesystem::path &output)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(output, error);
    if (error)
    {
        throw std::runtime_error("failed to resolve output parent directory: " + output.string());
    }
    std::filesystem::create_directories(absolute.parent_path(), error);
    if (error)
    {
        throw std::runtime_error("failed to create output parent directory: " + output.string());
    }
}

#ifdef _WIN32
void validateWindowsOutputFileName(const std::filesystem::path &output)
{
    const auto fileName = output.filename().native();
    if (fileName.empty())
    {
        throw std::runtime_error("output path must name a file: " + output.string());
    }
    if (fileName.back() == L'.' || fileName.back() == L' ' || fileName.find(L':') != std::wstring::npos)
    {
        throw std::runtime_error("output filename uses an ambiguous Windows spelling: " + output.string());
    }
}

bool directoryUsesCaseSensitiveNames(const std::filesystem::path &directory)
{
    const HANDLE handle = CreateFileW(
        directory.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error("failed to inspect output directory case sensitivity: " + directory.string());
    }

    struct DirectoryCaseSensitiveInfo
    {
        ULONG flags;
    } info {};
    constexpr auto caseSensitiveInfoClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
    constexpr ULONG caseSensitiveDirectoryFlag = 0x00000001;
    const BOOL queried = GetFileInformationByHandleEx(
        handle,
        caseSensitiveInfoClass,
        &info,
        sizeof(info));
    const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);

    if (!queried)
    {
        if (queryError == ERROR_INVALID_PARAMETER || queryError == ERROR_NOT_SUPPORTED)
        {
            // Directory case-sensitive lookup is unavailable on these systems.
            return false;
        }
        throw std::runtime_error("failed to query output directory case sensitivity: " + directory.string());
    }
    return (info.flags & caseSensitiveDirectoryFlag) != 0;
}

bool windowsFileNamesEqualIgnoreCase(
    const std::filesystem::path &left,
    const std::filesystem::path &right)
{
    const auto leftName = left.filename().native();
    const auto rightName = right.filename().native();
    return CompareStringOrdinal(
               leftName.c_str(),
               static_cast<int>(leftName.size()),
               rightName.c_str(),
               static_cast<int>(rightName.size()),
               TRUE) == CSTR_EQUAL;
}
#endif

bool outputPathsReferToSameFile(
    const std::filesystem::path &previewOutput,
    const std::filesystem::path &productOutput)
{
#ifdef _WIN32
    validateWindowsOutputFileName(previewOutput);
    validateWindowsOutputFileName(productOutput);
#endif
    // Map writers already create parent directories. Do it before alias
    // comparison as well so filesystem identity and case-sensitivity queries
    // work for a newly selected output directory.
    ensureOutputParentDirectory(previewOutput);
    ensureOutputParentDirectory(productOutput);

    std::error_code previewError;
    std::error_code productError;
    const bool previewExists = std::filesystem::exists(previewOutput, previewError) && !previewError;
    const bool productExists = std::filesystem::exists(productOutput, productError) && !productError;
    if (previewExists && productExists)
    {
        std::error_code equivalentError;
        const bool equivalent = std::filesystem::equivalent(previewOutput, productOutput, equivalentError);
        if (equivalentError)
        {
            throw std::runtime_error("failed to compare --out and --product-out");
        }
        return equivalent;
    }

    const auto previewResolved = resolvedOutputPath(previewOutput);
    const auto productResolved = resolvedOutputPath(productOutput);
#ifdef _WIN32
    // If one spelling resolves and the other does not, the containing directory
    // is using case-sensitive lookup. Preserve the filesystem's distinction.
    if (previewExists != productExists)
    {
        return previewResolved == productResolved;
    }
    if (previewResolved == productResolved)
    {
        return true;
    }

    std::error_code parentError;
    const bool sameParent = std::filesystem::equivalent(
        previewResolved.parent_path(),
        productResolved.parent_path(),
        parentError);
    if (parentError)
    {
        throw std::runtime_error("failed to compare --out and --product-out parent directories");
    }
    if (!sameParent || directoryUsesCaseSensitiveNames(previewResolved.parent_path()))
    {
        return false;
    }
    return windowsFileNamesEqualIgnoreCase(previewResolved, productResolved);
#else
    return previewResolved == productResolved;
#endif
}

void requireOutputDoesNotExist(const std::filesystem::path &output, const char *option)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(output, error);
    if (status.type() != std::filesystem::file_type::not_found)
    {
        throw std::runtime_error(std::string(option) + " already exists: " + output.string());
    }
    if (error && error != std::errc::no_such_file_or_directory)
    {
        throw std::runtime_error("failed to inspect " + std::string(option) + ": " + output.string());
    }
}

void validateNativeExportWaypoints(const rmg::NativeGenerationReport &report, int expectedPlayers)
{
    if (static_cast<int>(report.nativeWaypoints.size()) != expectedPlayers)
    {
        throw std::runtime_error(
            "native generation selected " + std::to_string(report.nativeWaypoints.size()) + " of " + std::to_string(expectedPlayers) + " requested export waypoints");
    }

    std::set<std::pair<int, int>> uniqueWaypoints;
    for (const rmg::Point waypoint : report.nativeWaypoints)
    {
        if (waypoint.x < 0 || waypoint.y < 0 || waypoint.x >= 512 || waypoint.y >= 512)
        {
            throw std::runtime_error("native generation selected an export waypoint outside 512x512 iso bounds");
        }
        if (!uniqueWaypoints.emplace(waypoint.x, waypoint.y).second)
        {
            throw std::runtime_error("native generation selected duplicate export waypoints");
        }
    }
}

void writeOutputMapFile(const std::filesystem::path &output, const rmg::MapGrid &map, const rmg::MapDocument &document, const std::string &format)
{
    try
    {
        if (format == "yrini")
        {
            rmg::writeYrIniMapFile(output, map, document);
        }
        else if (format == "yrini-product")
        {
            rmg::writeYrIniProductMapFile(output, map, document);
        }
        else
        {
            rmg::writeTextMapFile(output, map, document);
        }
    }
    catch (...)
    {
        removeOutputFile(output);
        throw;
    }
}

void writeAtomicTextOutputFile(const std::filesystem::path &output, std::string_view text)
{
    const std::filesystem::path temporary = output.string() + ".tmp";
    requireOutputDoesNotExist(temporary, "temporary output");
    try
    {
        ensureOutputParentDirectory(output);
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error("failed to open temporary output file: " + temporary.string());
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
        {
            throw std::runtime_error("failed to write temporary output file: " + temporary.string());
        }
        stream.close();
        if (!stream)
        {
            throw std::runtime_error("failed to close temporary output file: " + temporary.string());
        }

        std::error_code error;
        std::filesystem::rename(temporary, output, error);
        if (error)
        {
            throw std::runtime_error("failed to publish output file: " + output.string() + ": " + error.message());
        }
    }
    catch (...)
    {
        removeOutputFile(temporary);
        throw;
    }
}

void writeNativeOutputFiles(
    const CliOptions &cli,
    const rmg::MapGrid &map,
    const rmg::MapDocument &document,
    std::string_view nativeOptionsReport)
{
    if (cli.hasNativeOptionsReportOutput && (nativeOptionsReport.empty() || nativeOptionsReport.size() > rmg::kMaximumNativeGameOptionsReportBytes))
    {
        throw std::runtime_error("native options report must be between 1 and 8192 bytes");
    }
    try
    {
        writeOutputMapFile(cli.output, map, document, cli.format);
        if (cli.hasProductOutput)
        {
            rmg::writeYrIniProductMapFile(cli.productOutput, map, document);
        }
        if (cli.hasNativeOptionsReportOutput)
        {
            writeAtomicTextOutputFile(cli.nativeOptionsReportOutput, nativeOptionsReport);
        }
    }
    catch (...)
    {
        removeOutputFile(cli.output);
        if (cli.hasProductOutput)
        {
            removeOutputFile(cli.productOutput);
        }
        if (cli.hasNativeOptionsReportOutput)
        {
            removeOutputFile(cli.nativeOptionsReportOutput);
        }
        throw;
    }
}

bool writeOptionalMapEvaluation(
    const CliOptions &cli,
    const rmg::MapEvaluationReport &evaluation)
{
    if (!cli.hasEvaluationReportOutput)
    {
        return false;
    }
    try
    {
        rmg::writeMapEvaluationFile(cli.evaluationReportOutput, evaluation);
        return true;
    }
    catch (const std::exception &ex)
    {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(cli.evaluationReportOutput, statusError);
        if (!statusError && (std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))
        {
            removeOutputFile(cli.evaluationReportOutput);
        }
        std::cerr << "rmg: warning: map evaluation report was not written: " << ex.what() << '\n';
        return false;
    }
}

void writeTextOutputFile(const std::filesystem::path &output, const std::string &text)
{
    try
    {
        if (output.has_parent_path())
        {
            std::filesystem::create_directories(output.parent_path());
        }
        std::ofstream file(output, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("failed to open output file: " + output.string());
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    catch (...)
    {
        removeOutputFile(output);
        throw;
    }
}

struct StageReportEntry
{
    int index = 0;
    std::string name;
    std::map<std::string, std::string> fields;
    std::string line;
};

struct StageReportFile
{
    std::filesystem::path path;
    int declaredStages = -1;
    std::vector<StageReportEntry> stages;
    std::map<std::string, std::size_t> indexByName;
};

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string trimTrailingCarriageReturn(std::string value)
{
    if (!value.empty() && value.back() == '\r')
    {
        value.pop_back();
    }
    return value;
}

StageReportFile readStageReportFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("failed to open stage report: " + path.string());
    }

    StageReportFile report;
    report.path = path;
    std::string line;
    while (std::getline(input, line))
    {
        line = trimTrailingCarriageReturn(std::move(line));
        if (startsWith(line, "stages="))
        {
            report.declaredStages = std::stoi(line.substr(7));
            continue;
        }
        if (!startsWith(line, "stage="))
        {
            continue;
        }

        StageReportEntry entry;
        entry.line = line;
        std::istringstream parts(line);
        std::string token;
        while (parts >> token)
        {
            const auto equals = token.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }
            const std::string key = token.substr(0, equals);
            const std::string value = token.substr(equals + 1);
            entry.fields[key] = value;
        }

        if (auto it = entry.fields.find("stage"); it != entry.fields.end())
        {
            entry.index = std::stoi(it->second);
        }
        if (auto it = entry.fields.find("name"); it != entry.fields.end())
        {
            entry.name = it->second;
        }
        if (entry.name.empty())
        {
            throw std::runtime_error("stage report line is missing name: " + line);
        }
        report.indexByName[entry.name] = report.stages.size();
        report.stages.push_back(std::move(entry));
    }

    if (report.declaredStages >= 0 && report.declaredStages != static_cast<int>(report.stages.size()))
    {
        throw std::runtime_error(
            "stage report declared " + std::to_string(report.declaredStages) + " stages but parsed " + std::to_string(report.stages.size()) + ": " + path.string());
    }
    return report;
}

bool isUnavailableFieldValue(const std::string &value)
{
    return value.empty() || value == "origin-unavailable" || value == "unavailable" || value == "NA";
}

bool isUnavailableRngState(const StageReportEntry &entry)
{
    const auto it = entry.fields.find("rngState");
    if (it == entry.fields.end())
    {
        return true;
    }
    return isUnavailableFieldValue(it->second);
}

bool shouldCompareStageField(const std::string &key, const StageReportEntry &left, const StageReportEntry &right)
{
    if (key == "stage" || key == "name")
    {
        return false;
    }
    if (key == "rngState" && (isUnavailableRngState(left) || isUnavailableRngState(right)))
    {
        return false;
    }
    if (key == "rngDraws" && (isUnavailableRngState(left) || isUnavailableRngState(right)))
    {
        return false;
    }
    return true;
}

std::string describeStageReportComparison(const StageReportFile &left, const StageReportFile &right)
{
    struct StageSummary
    {
        std::string name;
        bool missingRight = false;
        bool orderMismatch = false;
        int commonFields = 0;
        int differingFields = 0;
        int missingLeftFields = 0;
        int missingRightFields = 0;
        bool fieldSetMatches = true;
        std::vector<std::string> differences;
    };

    std::vector<StageSummary> summaries;
    std::string firstDifference;
    bool overallMatch = left.stages.size() == right.stages.size();
    bool fieldSetMatch = true;

    for (std::size_t i = 0; i < left.stages.size(); ++i)
    {
        const auto &leftStage = left.stages[i];
        StageSummary summary;
        summary.name = leftStage.name;
        const auto rightIt = right.indexByName.find(leftStage.name);
        if (rightIt == right.indexByName.end())
        {
            summary.missingRight = true;
            overallMatch = false;
            if (firstDifference.empty())
            {
                firstDifference = "stage=" + leftStage.name + " missing_in_right";
            }
            summaries.push_back(std::move(summary));
            continue;
        }

        const auto &rightStage = right.stages[rightIt->second];
        summary.orderMismatch = rightIt->second != i;
        if (summary.orderMismatch)
        {
            overallMatch = false;
            if (firstDifference.empty())
            {
                firstDifference = "stage=" + leftStage.name + " order left=" + std::to_string(i + 1) + " right=" + std::to_string(rightIt->second + 1);
            }
        }

        std::set<std::string> allKeys;
        for (const auto &[key, value] : leftStage.fields)
        {
            (void) value;
            allKeys.insert(key);
        }
        for (const auto &[key, value] : rightStage.fields)
        {
            (void) value;
            allKeys.insert(key);
        }

        for (const std::string &key : allKeys)
        {
            if (key == "stage" || key == "name")
            {
                continue;
            }
            const auto leftField = leftStage.fields.find(key);
            const auto rightField = rightStage.fields.find(key);
            if (leftField == leftStage.fields.end())
            {
                ++summary.missingLeftFields;
                summary.fieldSetMatches = false;
                fieldSetMatch = false;
                continue;
            }
            if (rightField == rightStage.fields.end())
            {
                ++summary.missingRightFields;
                summary.fieldSetMatches = false;
                fieldSetMatch = false;
                continue;
            }
            if (!shouldCompareStageField(key, leftStage, rightStage))
            {
                continue;
            }
            if (isUnavailableFieldValue(leftField->second) || isUnavailableFieldValue(rightField->second))
            {
                continue;
            }

            ++summary.commonFields;
            if (leftField->second != rightField->second)
            {
                ++summary.differingFields;
                overallMatch = false;
                if (summary.differences.size() < 8)
                {
                    summary.differences.push_back(
                        key + " left=" + leftField->second + " right=" + rightField->second);
                }
                if (firstDifference.empty())
                {
                    firstDifference = "stage=" + leftStage.name + " field=" + key + " left=" + leftField->second + " right=" + rightField->second;
                }
            }
        }
        summaries.push_back(std::move(summary));
    }

    for (const auto &rightStage : right.stages)
    {
        if (!left.indexByName.contains(rightStage.name))
        {
            overallMatch = false;
            if (firstDifference.empty())
            {
                firstDifference = "stage=" + rightStage.name + " missing_in_left";
            }
        }
    }

    std::ostringstream out;
    out << "stage report comparison\n";
    out << "left=" << left.path.string() << '\n';
    out << "right=" << right.path.string() << '\n';
    out << "left_stages=" << left.stages.size() << " right_stages=" << right.stages.size() << '\n';
    out << "overall_match=" << (overallMatch ? "yes" : "no") << '\n';
    out << "field_set_match=" << (fieldSetMatch ? "yes" : "no") << '\n';
    if (!firstDifference.empty())
    {
        out << "first_difference " << firstDifference << '\n';
    }
    for (const auto &summary : summaries)
    {
        const bool stageMatches = !summary.missingRight && !summary.orderMismatch && summary.differingFields == 0;
        out << "stage name=" << summary.name
            << " status=" << (stageMatches ? "match" : "different")
            << " common=" << summary.commonFields
            << " differing=" << summary.differingFields
            << " missing_left_fields=" << summary.missingLeftFields
            << " missing_right_fields=" << summary.missingRightFields
            << " field_set_match=" << (summary.fieldSetMatches ? "yes" : "no");
        if (summary.missingRight)
        {
            out << " missing_in_right=1";
        }
        if (summary.orderMismatch)
        {
            out << " order_mismatch=1";
        }
        for (const auto &diff : summary.differences)
        {
            out << " diff=\"" << diff << "\"";
        }
        out << '\n';
    }
    return out.str();
}

void printUsage()
{
    std::cout
        << "Usage: rmg --version | --mode native|game-native|native-stage-report|compare-stage-report|shore-atlas|verify-yrini|verify-yrini-product|verify-native-options-report|signature-yrini|inspect-yrini|compare-yrini|verify-codecs|verify-map-writer|verify-map-evaluation|verify-native-game-options --seed N --width W --height H --players N --out FILE [--format debug|yrini|yrini-product]\n"
        << "Competitive verification: --mode verify-competitive-yrini --input FILE --players 0|2..8 (0: terrain only)\n"
        << "Native extras: --sed FILE --rmg-config FILE --rmg-ini FILE --game-resource-dir DIR --theater-ini FILE --tile-catalog FILE --tile-preset yr --native-scenario-unique-id N --native-structures-trace FILE --native-tech-trace FILE --product-out FILE --evaluation-report-out FILE --error-report-out FILE --theater N --map-type 0..4 --resources 0..4 --time N --water-amount 0..100 --tiberium 0..100 --tiberium-layout 0..100 --vegetation 0..100 --ruggedness 0..100 --urban-presence 0..100 --accessibility 0..100 --region-size 0..100 --terrain-type 0..2\n"
        << "Game-native extras: --randomization-seed 0..4294967295 --game-terrain-type 1..4 --time-of-day 0..3 --climate 0..1 --map-size 0..3 --resources 0..3 --players 2..8 [--native-options-report-out FILE] [--native-options-report-hex HEX]\n"
        << "Debug extras: --mode shore-atlas --theater-ini FILE --out FILE\n";
}

const std::set<std::string> &knownCliOptions()
{
    static const std::set<std::string> options {
        "mode",
        "seed",
        "width",
        "height",
        "players",
        "theater",
        "map-type",
        "resources",
        "time",
        "water-amount",
        "tiberium",
        "tiberium-layout",
        "vegetation",
        "ruggedness",
        "urban-presence",
        "accessibility",
        "region-size",
        "terrain-type",
        "randomization-seed",
        "game-terrain-type",
        "time-of-day",
        "climate",
        "map-size",
        "out",
        "product-out",
        "evaluation-report-out",
        "error-report-out",
        "native-options-report-out",
        "native-options-report-hex",
        "input",
        "compare",
        "format",
        "sed",
        "rmg-config",
        "rmg-ini",
        "theater-ini",
        "tile-catalog",
        "tile-preset",
        "game-resource-dir",
        "native-scenario-unique-id",
        "native-structures-trace",
        "native-tech-trace",
    };
    return options;
}

int readInt(const std::unordered_map<std::string, std::string> &args, const std::string &key, int fallback)
{
    auto it = args.find(key);
    if (it == args.end())
    {
        return fallback;
    }
    const std::string &text = it->second;
    std::size_t parsedLength = 0;
    long long value = 0;
    try
    {
        value = std::stoll(text, &parsedLength, 10);
    }
    catch (const std::exception &)
    {
        throw std::runtime_error("--" + key + " must be an integer: " + text);
    }
    if (parsedLength != text.size() || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("--" + key + " must be a 32-bit integer: " + text);
    }
    return static_cast<int>(value);
}

std::uint32_t readUInt32(const std::unordered_map<std::string, std::string> &args, const std::string &key, std::uint32_t fallback)
{
    auto it = args.find(key);
    if (it == args.end())
    {
        return fallback;
    }

    const std::string &text = it->second;
    if (text.empty() || text.front() == '-' || text.front() == '+')
    {
        throw std::runtime_error("--" + key + " must be an unsigned 32-bit integer: " + text);
    }

    std::size_t parsedLength = 0;
    unsigned long long value = 0;
    try
    {
        value = std::stoull(text, &parsedLength, 10);
    }
    catch (const std::exception &)
    {
        throw std::runtime_error("--" + key + " must be an unsigned 32-bit integer: " + text);
    }
    if (parsedLength != text.size() || value > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("--" + key + " must be between 0 and 4294967295: " + text);
    }
    return static_cast<std::uint32_t>(value);
}

std::string readString(const std::unordered_map<std::string, std::string> &args, const std::string &key, const std::string &fallback)
{
    auto it = args.find(key);
    if (it == args.end())
    {
        return fallback;
    }
    return it->second;
}

std::vector<std::string> splitCsvRow(const std::string &line)
{
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const char ch = line[i];
        if (ch == '"')
        {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"')
            {
                field.push_back('"');
                ++i;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if (ch == ',' && !quoted)
        {
            fields.push_back(field);
            field.clear();
        }
        else
        {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

int csvIntField(const std::vector<std::string> &fields, const std::unordered_map<std::string, std::size_t> &indexByName, const std::string &name, int fallback)
{
    const auto it = indexByName.find(name);
    if (it == indexByName.end() || it->second >= fields.size() || fields[it->second].empty())
    {
        return fallback;
    }
    try
    {
        return std::stoi(fields[it->second]);
    }
    catch (const std::exception &)
    {
        return fallback;
    }
}

std::string csvStringField(const std::vector<std::string> &fields, const std::unordered_map<std::string, std::size_t> &indexByName, const std::string &name)
{
    const auto it = indexByName.find(name);
    if (it == indexByName.end() || it->second >= fields.size())
    {
        return {};
    }
    return fields[it->second];
}

void requireCsvColumn(const std::unordered_map<std::string, std::size_t> &indexByName, const std::string &name, const std::filesystem::path &path)
{
    if (!indexByName.contains(name))
    {
        throw std::runtime_error("native structures trace is missing column '" + name + "': " + path.string());
    }
}

bool parseIntStrict(const std::string &text, int &output)
{
    if (text.empty())
    {
        return false;
    }

    std::size_t parsedLength = 0;
    long long value = 0;
    try
    {
        value = std::stoll(text, &parsedLength, 10);
    }
    catch (const std::exception &)
    {
        return false;
    }
    if (parsedLength != text.size() || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    {
        return false;
    }

    output = static_cast<int>(value);
    return true;
}

int parseRequiredIntField(const std::string &text, const std::string &fieldName, const std::filesystem::path &path)
{
    int value = 0;
    if (!parseIntStrict(text, value))
    {
        throw std::runtime_error("native structures trace has invalid integer field '" + fieldName + "': " + text + " in " + path.string());
    }
    return value;
}

std::string nativeTechStructureTypeForTrace(int nativeTechIndex)
{
    static const std::vector<std::string> techBuildings {
        "CAAIRP",
        "CATHOSP",
        "CAOILD",
        "CAOUTP",
        "CAMACH",
        "CAPOWR",
        "CASLAB",
    };
    if (nativeTechIndex < 0 || nativeTechIndex >= static_cast<int>(techBuildings.size()))
    {
        return "CAOILD";
    }
    return techBuildings[static_cast<std::size_t>(nativeTechIndex)];
}

struct NativeTechTraceStructureKey
{
    rmg::Point iso;
    int nativeTechIndex = -1;
    int nativeUniqueId = -1;
};

struct NativeStructureSaveTraceEntry
{
    int rowIndex = 0;
    rmg::Point iso;
    int nativeUniqueId = -1;
    std::string key;
    std::string typeId;
    std::string value;
};

void applyNativeStructureSaveTrace(rmg::MapDocument &document, const std::filesystem::path &path)
{
    if (path.empty())
    {
        return;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("failed to open native structures trace: " + path.string());
    }

    std::string headerLine;
    if (!std::getline(input, headerLine))
    {
        return;
    }
    if (!headerLine.empty() && headerLine.back() == '\r')
    {
        headerLine.pop_back();
    }
    const std::vector<std::string> header = splitCsvRow(headerLine);
    std::unordered_map<std::string, std::size_t> indexByName;
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        indexByName.emplace(header[i], i);
    }

    requireCsvColumn(indexByName, "index", path);
    requireCsvColumn(indexByName, "buildingUniqueId", path);
    requireCsvColumn(indexByName, "key", path);
    requireCsvColumn(indexByName, "typeId", path);
    requireCsvColumn(indexByName, "value", path);

    std::vector<NativeStructureSaveTraceEntry> entries;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        const std::vector<std::string> fields = splitCsvRow(line);
        const std::string value = csvStringField(fields, indexByName, "value");
        const std::vector<std::string> valueFields = splitCsvRow(value);
        if (valueFields.size() < 5)
        {
            throw std::runtime_error("native structures trace value does not contain owner,type,health,x,y: " + value);
        }

        const std::string traceTypeId = csvStringField(fields, indexByName, "typeId");
        const std::string valueTypeId = valueFields[1];
        if (valueTypeId.empty())
        {
            throw std::runtime_error("native structures trace value is missing building type: " + value);
        }
        if (!traceTypeId.empty() && traceTypeId != valueTypeId)
        {
            throw std::runtime_error("native structures trace typeId does not match saved value: typeId=" + traceTypeId + " valueType=" + valueTypeId);
        }

        NativeStructureSaveTraceEntry entry;
        entry.rowIndex = parseRequiredIntField(csvStringField(fields, indexByName, "index"), "index", path);
        entry.iso = {
            parseRequiredIntField(valueFields[3], "value.x", path),
            parseRequiredIntField(valueFields[4], "value.y", path),
        };
        entry.typeId = valueTypeId;
        entry.key = csvStringField(fields, indexByName, "key");
        entry.value = value;
        entry.nativeUniqueId = parseRequiredIntField(csvStringField(fields, indexByName, "buildingUniqueId"), "buildingUniqueId", path);
        if (entry.nativeUniqueId < 0)
        {
            throw std::runtime_error("native structures trace has negative buildingUniqueId: " + std::to_string(entry.nativeUniqueId));
        }
        int keyAsInt = -1;
        if (!parseIntStrict(entry.key, keyAsInt))
        {
            throw std::runtime_error("native structures trace has invalid numeric key: " + entry.key);
        }
        if (keyAsInt != entry.nativeUniqueId)
        {
            throw std::runtime_error("native structures trace key does not match buildingUniqueId: key=" + entry.key + " buildingUniqueId=" + std::to_string(entry.nativeUniqueId));
        }
        entries.push_back(std::move(entry));
    }

    if (entries.empty())
    {
        return;
    }
    if (document.nativeStructureOverrides.empty())
    {
        throw std::runtime_error("native structures trace contains structures but generated document has none: " + path.string());
    }

    std::vector<bool> matched(document.nativeStructureOverrides.size(), false);
    std::vector<rmg::NativeStructureOverride> reordered;
    reordered.reserve(document.nativeStructureOverrides.size());
    for (const NativeStructureSaveTraceEntry &entry : entries)
    {
        int matchedIndex = -1;
        for (std::size_t i = 0; i < document.nativeStructureOverrides.size(); ++i)
        {
            if (matched[i])
            {
                continue;
            }
            const rmg::NativeStructureOverride &structure = document.nativeStructureOverrides[i];
            if (structure.iso == entry.iso && nativeTechStructureTypeForTrace(structure.nativeTechIndex) == entry.typeId)
            {
                matchedIndex = static_cast<int>(i);
                break;
            }
        }

        if (matchedIndex < 0)
        {
            std::ostringstream message;
            message << "native structures trace row did not match generated structure: row="
                    << entry.rowIndex << " type=" << entry.typeId << " x=" << entry.iso.x << " y=" << entry.iso.y;
            throw std::runtime_error(message.str());
        }

        matched[static_cast<std::size_t>(matchedIndex)] = true;
        rmg::NativeStructureOverride structure = document.nativeStructureOverrides[static_cast<std::size_t>(matchedIndex)];
        structure.nativeUniqueId = entry.nativeUniqueId;
        structure.nativeSaveKey = entry.key;
        structure.nativeSaveValue = entry.value;
        reordered.push_back(std::move(structure));
    }

    for (std::size_t i = 0; i < matched.size(); ++i)
    {
        if (!matched[i])
        {
            const rmg::NativeStructureOverride &structure = document.nativeStructureOverrides[i];
            std::ostringstream message;
            message << "generated structure was not present in native structures trace: type="
                    << nativeTechStructureTypeForTrace(structure.nativeTechIndex)
                    << " x=" << structure.iso.x << " y=" << structure.iso.y;
            throw std::runtime_error(message.str());
        }
    }

    document.nativeStructureOverrides = std::move(reordered);
}

void applyNativeTechTraceStructureKeys(rmg::MapDocument &document, const std::filesystem::path &path)
{
    if (path.empty() || document.nativeStructureOverrides.empty())
    {
        return;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("failed to open native tech trace: " + path.string());
    }

    std::string headerLine;
    if (!std::getline(input, headerLine))
    {
        return;
    }
    if (!headerLine.empty() && headerLine.back() == '\r')
    {
        headerLine.pop_back();
    }
    const std::vector<std::string> header = splitCsvRow(headerLine);
    std::unordered_map<std::string, std::size_t> indexByName;
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        indexByName.emplace(header[i], i);
    }
    if (!indexByName.contains("buildingUniqueId"))
    {
        return;
    }

    std::unordered_map<int, int> typeByCall;
    std::unordered_map<int, int> uniqueIdByCall;
    std::vector<NativeTechTraceStructureKey> keys;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const std::vector<std::string> fields = splitCsvRow(line);
        const std::string event = csvStringField(fields, indexByName, "event");
        const int call = csvIntField(fields, indexByName, "call", -1);
        if (call < 0)
        {
            continue;
        }
        if (event == "global_type")
        {
            typeByCall[call] = csvIntField(fields, indexByName, "typeIndex", -1);
        }
        else if (event == "global_constructed")
        {
            const int uniqueId = csvIntField(fields, indexByName, "buildingUniqueId", -1);
            if (uniqueId >= 0)
            {
                uniqueIdByCall[call] = uniqueId;
            }
        }
        else if (event == "global_attempt" && csvIntField(fields, indexByName, "placed", 0) != 0)
        {
            const auto typeIt = typeByCall.find(call);
            const auto uniqueIt = uniqueIdByCall.find(call);
            if (typeIt == typeByCall.end() || uniqueIt == uniqueIdByCall.end())
            {
                continue;
            }
            keys.push_back({
                {csvIntField(fields, indexByName, "x", -1), csvIntField(fields, indexByName, "y", -1)},
                typeIt->second,
                uniqueIt->second,
            });
        }
    }

    for (const NativeTechTraceStructureKey &key : keys)
    {
        bool matched = false;
        for (rmg::NativeStructureOverride &structure : document.nativeStructureOverrides)
        {
            if (structure.nativeUniqueId >= 0)
            {
                continue;
            }
            if (structure.iso.x == key.iso.x && structure.iso.y == key.iso.y && structure.nativeTechIndex == key.nativeTechIndex)
            {
                structure.nativeUniqueId = key.nativeUniqueId;
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            std::ostringstream message;
            message << "native tech trace structure did not match generated structure: x="
                    << key.iso.x << " y=" << key.iso.y << " typeIndex=" << key.nativeTechIndex;
            throw std::runtime_error(message.str());
        }
    }
}

CliOptions parseArgs(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--version")
    {
        CliOptions options;
        options.showVersion = true;
        return options;
    }

    std::unordered_map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        std::string key = argv[i];
        if (key == "--help" || key == "-h")
        {
            printUsage();
            std::exit(0);
        }
        if (key == "--version")
        {
            throw std::runtime_error("--version must be used alone");
        }
        if (key.rfind("--", 0) != 0 || i + 1 >= argc)
        {
            throw std::runtime_error("invalid argument near: " + key);
        }
        const std::string option = key.substr(2);
        if (!knownCliOptions().contains(option))
        {
            throw std::runtime_error("unknown argument: --" + option);
        }
        args[option] = argv[++i];
    }

    CliOptions options;
    options.mode = readString(args, "mode", options.mode);
    options.hasSeed = args.contains("seed");
    options.hasWidth = args.contains("width");
    options.hasHeight = args.contains("height");
    options.hasPlayers = args.contains("players");
    options.hasTheater = args.contains("theater");
    options.hasMapType = args.contains("map-type");
    options.hasResources = args.contains("resources");
    options.hasTime = args.contains("time");
    options.hasWaterAmount = args.contains("water-amount");
    options.hasTiberium = args.contains("tiberium");
    options.hasTiberiumLayout = args.contains("tiberium-layout");
    options.hasVegetation = args.contains("vegetation");
    options.hasRuggedness = args.contains("ruggedness");
    options.hasUrbanPresence = args.contains("urban-presence");
    options.hasAccessibility = args.contains("accessibility");
    options.hasRegionSize = args.contains("region-size");
    options.hasTerrainType = args.contains("terrain-type");
    options.hasRandomizationSeed = args.contains("randomization-seed");
    options.hasGameTerrainType = args.contains("game-terrain-type");
    options.hasTimeOfDay = args.contains("time-of-day");
    options.hasClimate = args.contains("climate");
    options.hasMapSize = args.contains("map-size");
    options.hasNativeScenarioUniqueId = args.contains("native-scenario-unique-id");
    if (options.hasSeed)
    {
        options.seed = readUInt32(args, "seed", options.seed);
    }
    if (options.hasWidth)
    {
        options.width = readInt(args, "width", options.width);
    }
    if (options.hasHeight)
    {
        options.height = readInt(args, "height", options.height);
    }
    if (options.hasPlayers)
    {
        options.players = readInt(args, "players", options.players);
    }
    if (options.hasTheater)
    {
        options.theater = readInt(args, "theater", options.theater);
    }
    if (options.hasMapType)
    {
        options.mapType = readInt(args, "map-type", options.mapType);
    }
    if (options.hasResources)
    {
        options.resources = readInt(args, "resources", options.resources);
    }
    if (options.hasTime)
    {
        options.time = readInt(args, "time", options.time);
    }
    if (options.hasWaterAmount)
    {
        options.waterAmount = readInt(args, "water-amount", options.waterAmount);
    }
    if (options.hasTiberium)
    {
        options.tiberium = readInt(args, "tiberium", options.tiberium);
    }
    if (options.hasTiberiumLayout)
    {
        options.tiberiumLayout = readInt(args, "tiberium-layout", options.tiberiumLayout);
    }
    if (options.hasVegetation)
    {
        options.vegetation = readInt(args, "vegetation", options.vegetation);
    }
    if (options.hasRuggedness)
    {
        options.ruggedness = readInt(args, "ruggedness", options.ruggedness);
    }
    if (options.hasUrbanPresence)
    {
        options.urbanPresence = readInt(args, "urban-presence", options.urbanPresence);
    }
    if (options.hasAccessibility)
    {
        options.accessibility = readInt(args, "accessibility", options.accessibility);
    }
    if (options.hasRegionSize)
    {
        options.regionSize = readInt(args, "region-size", options.regionSize);
    }
    if (options.hasTerrainType)
    {
        options.terrainType = readInt(args, "terrain-type", options.terrainType);
    }
    if (options.hasRandomizationSeed)
    {
        options.randomizationSeed = readUInt32(args, "randomization-seed", options.randomizationSeed);
    }
    if (options.hasGameTerrainType)
    {
        options.gameTerrainType = readInt(args, "game-terrain-type", options.gameTerrainType);
    }
    if (options.hasTimeOfDay)
    {
        options.timeOfDay = readInt(args, "time-of-day", options.timeOfDay);
    }
    if (options.hasClimate)
    {
        options.climate = readInt(args, "climate", options.climate);
    }
    if (options.hasMapSize)
    {
        options.mapSize = readInt(args, "map-size", options.mapSize);
    }
    if (options.hasNativeScenarioUniqueId)
    {
        options.nativeScenarioUniqueId = readInt(args, "native-scenario-unique-id", options.nativeScenarioUniqueId);
    }
    options.output = readString(args, "out", options.output.string());
    options.hasOutput = args.contains("out");
    options.productOutput = readString(args, "product-out", "");
    options.hasProductOutput = args.contains("product-out");
    options.evaluationReportOutput = readString(args, "evaluation-report-out", "");
    options.hasEvaluationReportOutput = args.contains("evaluation-report-out");
    options.errorReportOutput = readString(args, "error-report-out", "");
    options.hasErrorReportOutput = args.contains("error-report-out");
    options.nativeOptionsReportOutput = readString(args, "native-options-report-out", "");
    options.hasNativeOptionsReportOutput = args.contains("native-options-report-out");
    options.nativeOptionsReportHex = readString(args, "native-options-report-hex", "");
    options.hasNativeOptionsReportHex = args.contains("native-options-report-hex");
    options.input = readString(args, "input", "");
    options.compare = readString(args, "compare", "");
    options.format = readString(args, "format", options.format);
    options.sedPath = readString(args, "sed", "");
    options.hasSed = args.contains("sed");
    options.rmgConfigPath = readString(args, "rmg-config", "");
    if (options.rmgConfigPath.empty())
    {
        options.rmgConfigPath = readString(args, "rmg-ini", "");
    }
    options.theaterIniPath = readString(args, "theater-ini", "");
    options.tileCatalogPath = readString(args, "tile-catalog", "");
    options.tilePreset = readString(args, "tile-preset", "");
    options.gameResourceDir = readString(args, "game-resource-dir", "");
    options.nativeStructuresTracePath = readString(args, "native-structures-trace", "");
    options.nativeTechTracePath = readString(args, "native-tech-trace", "");
    if (options.format != "debug" && options.format != "yrini" && options.format != "yrini-product")
    {
        throw std::runtime_error("--format must be debug, yrini, or yrini-product");
    }
    return options;
}

rmg::MapGrid makeShoreAtlasMap(int requestedWidth, int requestedHeight, int singleVariant)
{
    constexpr int kVariants = 42;
    constexpr int kColumns = 6;
    constexpr int kSpacingX = 10;
    constexpr int kSpacingY = 9;
    constexpr int kMarginX = 6;
    constexpr int kMarginY = 6;

    const bool single = singleVariant >= 0 && singleVariant < kVariants;
    const int rows = single ? 1 : (kVariants + kColumns - 1) / kColumns;
    const int columns = single ? 1 : kColumns;
    const int width = std::max(requestedWidth, kMarginX * 2 + (columns - 1) * kSpacingX + 5);
    const int height = std::max(requestedHeight, kMarginY * 2 + (rows - 1) * kSpacingY + 5);

    rmg::MapGrid map(width, height);
    map.fill(rmg::Terrain::Land);
    for (auto &cell : map.cells())
    {
        cell.level = 0;
    }

    const int firstVariant = single ? singleVariant : 0;
    const int lastVariant = single ? singleVariant + 1 : kVariants;
    for (int variant = firstVariant; variant < lastVariant; ++variant)
    {
        const int displayVariant = single ? 0 : variant;
        const int column = displayVariant % kColumns;
        const int row = displayVariant / kColumns;
        const rmg::Point center {kMarginX + column * kSpacingX, kMarginY + row * kSpacingY};
        if (!map.inBounds(center))
        {
            continue;
        }

        auto &cell = map.at(center);
        cell.terrain = rmg::Terrain::Shore;
        cell.slope = rmg::encodeShorePiece({variant, 0});

        if (!single)
        {
            // Small water marks below each sample make shore-water alignment easier to read
            // in the overview atlas without hiding the ShorePieces tile itself.
            for (const rmg::Point mark : {rmg::Point {center.x, center.y + 2}, rmg::Point {center.x + 1, center.y + 2}})
            {
                if (map.inBounds(mark))
                {
                    auto &water = map.at(mark);
                    water.terrain = rmg::Terrain::Water;
                    water.slope = 0;
                }
            }
        }
    }

    return map;
}

rmg::MapGrid makeShoreSubtileMap(int requestedWidth, int requestedHeight, int tileIndex, int subTile)
{
    const int width = std::max(requestedWidth, 20);
    const int height = std::max(requestedHeight, 20);

    rmg::MapGrid map(width, height);
    map.fill(rmg::Terrain::Land);
    for (auto &cell : map.cells())
    {
        cell.level = 0;
    }

    const rmg::Point center {width / 2, height / 2};
    auto &cell = map.at(center);
    cell.terrain = rmg::Terrain::Shore;
    cell.slope = rmg::encodeShorePiece({tileIndex, subTile});
    return map;
}

rmg::MapGrid makeCliffAtlasMap(int requestedWidth, int requestedHeight, int singleVariant)
{
    constexpr int kVariants = 40;
    constexpr int kColumns = 8;
    constexpr int kSpacingX = 10;
    constexpr int kSpacingY = 10;
    constexpr int kMarginX = 6;
    constexpr int kMarginY = 6;

    const bool single = singleVariant >= 0 && singleVariant < kVariants;
    const int rows = single ? 1 : (kVariants + kColumns - 1) / kColumns;
    const int columns = single ? 1 : kColumns;
    const int width = std::max(requestedWidth, kMarginX * 2 + (columns - 1) * kSpacingX + 5);
    const int height = std::max(requestedHeight, kMarginY * 2 + (rows - 1) * kSpacingY + 5);

    rmg::MapGrid map(width, height);
    map.fill(rmg::Terrain::Land);
    for (auto &cell : map.cells())
    {
        cell.level = 0;
    }

    const int firstVariant = single ? singleVariant : 0;
    const int lastVariant = single ? singleVariant + 1 : kVariants;
    for (int variant = firstVariant; variant < lastVariant; ++variant)
    {
        const int displayVariant = single ? 0 : variant;
        const int column = displayVariant % kColumns;
        const int row = displayVariant / kColumns;
        const rmg::Point anchor {kMarginX + column * kSpacingX, kMarginY + row * kSpacingY};
        for (int y = 0; y < rmg::cliffPieceHeight(variant); ++y)
        {
            for (int x = 0; x < rmg::cliffPieceWidth(variant); ++x)
            {
                if (!rmg::isValidCliffPieceSlot(variant, x, y))
                {
                    continue;
                }
                const rmg::Point point {anchor.x + x, anchor.y + y};
                if (!map.inBounds(point))
                {
                    continue;
                }
                auto &cliff = map.at(point);
                cliff.terrain = rmg::Terrain::Cliff;
                const int subTile = rmg::cliffPieceSubTile(variant, x, y);
                cliff.slope = rmg::encodeCliffPiece({variant, subTile});
            }
        }
    }

    return map;
}

rmg::TileCatalog loadTileCatalogForCli(const CliOptions &cli, int theater)
{
    rmg::TileCatalog tileCatalog;
    if (!cli.tilePreset.empty())
    {
        tileCatalog = rmg::loadTileCatalogPreset(cli.tilePreset, theater);
    }
    if (!cli.theaterIniPath.empty())
    {
        tileCatalog = rmg::loadTileCatalogFromTheaterIni(cli.theaterIniPath, tileCatalog);
    }
    if (!cli.tileCatalogPath.empty())
    {
        tileCatalog = rmg::loadTileCatalog(cli.tileCatalogPath, tileCatalog);
    }
    return tileCatalog;
}

void requireRange(const std::string &name, int value, int minimum, int maximum)
{
    if (value < minimum || value > maximum)
    {
        throw std::runtime_error(
            name + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
    }
}

void validateNativeOptions(const rmg::MapSeedOptions &options, bool gameNativeSize = false)
{
    requireRange("width", options.width, gameNativeSize ? 0 : 1, 512);
    requireRange("height", options.height, gameNativeSize ? 0 : 1, 512);
    requireRange("players", options.numPlayers, 2, 8);
    requireRange("theater", options.theater, 0, 5);
    requireRange("map-type", options.mapType, 0, 4);
    requireRange("resources", options.resources, 0, 4);
    requireRange("time", options.time, 0, 4);
    requireRange("water-amount", options.waterAmount, 0, 100);
    requireRange("tiberium", options.tiberium, 0, 100);
    requireRange("tiberium-layout", options.tiberiumLayout, 0, 100);
    requireRange("vegetation", options.vegetation, 0, 100);
    requireRange("ruggedness", options.ruggedness, 0, 100);
    requireRange("urban-presence", options.urbanPresence, 0, 100);
    requireRange("accessibility", options.accessibility, 0, 100);
    requireRange("region-size", options.regionSize, 0, 100);
    requireRange("terrain-type", options.terrainTypeOverride, -1, 2);
    if (options.nativeScenarioUniqueId < -1)
    {
        throw std::runtime_error("native-scenario-unique-id must be -1 or a non-negative 32-bit integer");
    }
}

void verifyMapWriter()
{
    rmg::MapGrid map(6, 6);
    map.fill(rmg::Terrain::Land);

    rmg::Cell &terrainChain = map.at({2, 2});
    terrainChain.overlay = rmg::Overlay::TerrainChain;
    terrainChain.overlayValue = 1;
    rmg::Cell &neutralBuilding = map.at({1, 1});
    neutralBuilding.overlay = rmg::Overlay::NeutralBuilding;
    neutralBuilding.overlayValue = 0;
    rmg::Cell &oreLamp = map.at({4, 4});
    oreLamp.terrainObject = rmg::TerrainObject::OrePatchLamp;
    oreLamp.terrainObjectValue = 0;
    rmg::Cell &bridge1 = map.at({2, 3});
    bridge1.terrain = rmg::Terrain::Water;
    bridge1.overlay = rmg::Overlay::Bridge;
    bridge1.overlayValue = 0;
    rmg::Cell &bridge2 = map.at({3, 2});
    bridge2.terrain = rmg::Terrain::Water;
    bridge2.overlay = rmg::Overlay::Bridge;
    bridge2.overlayValue = 9;

    rmg::MapDocument document;
    document.mode = "native";
    document.seed = 1;
    document.players = 2;
    document.theater = 0;
    document.mapLevel = 4;
    document.starts = {{0, 0}, {5, 5}};
    document.rmgConfig.civilianBuildings = {"CABHUT"};
    document.rmgConfig.temperateOrePatchLamps = {"INGALITE"};
    document.rmgConfig.terrainChainObjects = {"ROCK03", "ROCK04", "TREE03"};
    document.tileCatalog.land = {200, 18, 0, 1, {}};
    document.tileCatalog.ramp = {90, 18, 7, 1, {}};
    document.nativeObjectOverrides.push_back({{3, 4}, 36, 14, -1, -1});
    document.nativeStructureOverrides.push_back({{3, 3}, 1, 123});
    document.isoTileOverrides.push_back({{5, 2}, rmg::Terrain::Land, 0, 0, 4, false, 0, 0, 0xFFFF});

    rmg::Cell &ramp = map.at({0, 0});
    ramp.level = 6;
    ramp.slope = 5;
    ramp.landType = 3;
    rmg::Cell &ordinaryVariant = map.at({0, 1});
    ordinaryVariant.slope = 5;

    const std::string yrini = rmg::writeYrIniMap(map, document);
    if (yrini.find("=94,6,5,") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost RampBase slope mapping");
    }
    if (yrini.find("=205,4,5,") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test treated an ordinary tile variant as a Ramp slope");
    }
    if (yrini.find("=TREE01") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost native terrain override");
    }
    if (yrini.find("=ROCK03") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost terrain-chain fallback in yrini");
    }
    if (yrini.find("123=Neutral,CATHOSP") == std::string::npos || yrini.find("=Neutral,CABHUT") == std::string::npos || yrini.find("=Neutral,INGALITE") == std::string::npos)
    {
        throw std::runtime_error(
            "map writer self-test lost native override or non-tech structure fallback in yrini");
    }

    const std::filesystem::path tilePath = std::filesystem::temp_directory_path() / "rmg_native_tile_writer_selftest.yrini";
    rmg::YrIniValidationResult tileValidation;
    try
    {
        writeTextOutputFile(tilePath, yrini);
        tileValidation = rmg::validateYrIniFile(tilePath);
    }
    catch (...)
    {
        removeOutputFile(tilePath);
        throw;
    }
    removeOutputFile(tilePath);
    if (tileValidation.maxTileNum != 0xFFFF || tileValidation.maxSubTile != 0)
    {
        throw std::runtime_error("map writer self-test lost native Ramp subtile or 0xFFFF tile semantics");
    }
    if (tileValidation.bridgeOverlayEntries != 2)
    {
        throw std::runtime_error("map writer self-test lost high bridge overlays");
    }

    const std::string product = rmg::writeYrIniProductMap(map, document);
    if (product.find("=ROCK03") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost terrain-chain fallback in yrini-product");
    }
    if (product.find("123=Neutral,CATHOSP") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost native structure override key in yrini-product");
    }
    if (product.find("=Neutral,CABHUT") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost neutral building fallback in yrini-product");
    }
    if (product.find("=Neutral,INGALITE") == std::string::npos)
    {
        throw std::runtime_error("map writer self-test lost ore patch lamp fallback in yrini-product");
    }

    rmg::MapDocument tracedDocument = document;
    tracedDocument.nativeStructureOverrides.clear();
    tracedDocument.nativeStructureOverrides.push_back({{3, 3}, 1});
    tracedDocument.nativeStructureOverrides.push_back({{4, 5}, 2});

    const std::filesystem::path tracePath = std::filesystem::temp_directory_path() / "rmg_native_structures_trace_selftest.csv";
    writeTextOutputFile(
        tracePath,
        "index,buildingPtr,buildingUniqueId,key,typeArrayIndex,typeId,value\n"
        "0,0,777,777,-1,CAOILD,\"Neutral,CAOILD,256,4,5,0,None,1,0,1,0,0,None,None,None,0,0\"\n"
        "1,0,555,555,-1,CATHOSP,\"Neutral,CATHOSP,256,3,3,0,None,1,0,1,0,0,None,None,None,0,0\"\n");
    applyNativeStructureSaveTrace(tracedDocument, tracePath);
    removeOutputFile(tracePath);

    const std::string tracedProduct = rmg::writeYrIniProductMap(map, tracedDocument);
    const std::size_t oilIndex = tracedProduct.find("777=Neutral,CAOILD,256,4,5,0,None,1,0,1,0,0,None,None,None,0,0");
    const std::size_t hospitalIndex = tracedProduct.find("555=Neutral,CATHOSP,256,3,3,0,None,1,0,1,0,0,None,None,None,0,0");
    const std::size_t civilianIndex = tracedProduct.find("=Neutral,CABHUT");
    const std::size_t lampIndex = tracedProduct.find("=Neutral,INGALITE");
    if (oilIndex == std::string::npos || hospitalIndex == std::string::npos || civilianIndex == std::string::npos || lampIndex == std::string::npos || oilIndex > hospitalIndex || hospitalIndex > civilianIndex || hospitalIndex > lampIndex)
    {
        throw std::runtime_error("map writer self-test lost native structures trace key/value/order override");
    }

    writeTextOutputFile(
        tracePath,
        "index,buildingPtr,buildingUniqueId,key,typeArrayIndex,typeId,value\n"
        "0,0,777,777,-1,CAOILD,\"Neutral,CAOILD,256,3,3,0,None,1,0,1,0,0,None,None,None,0,0\"\n");
    bool rejectedMismatchedTrace = false;
    try
    {
        applyNativeStructureSaveTrace(tracedDocument, tracePath);
    }
    catch (const std::runtime_error &)
    {
        rejectedMismatchedTrace = true;
    }
    removeOutputFile(tracePath);
    if (!rejectedMismatchedTrace)
    {
        throw std::runtime_error("map writer self-test accepted mismatched native structures trace");
    }

    writeTextOutputFile(
        tracePath,
        "index,buildingPtr,buildingUniqueId,key,typeArrayIndex,typeId,value\n"
        "0,0,777,778,-1,CATHOSP,\"Neutral,CATHOSP,256,3,3,0,None,1,0,1,0,0,None,None,None,0,0\"\n");
    bool rejectedMismatchedUniqueId = false;
    try
    {
        applyNativeStructureSaveTrace(tracedDocument, tracePath);
    }
    catch (const std::runtime_error &)
    {
        rejectedMismatchedUniqueId = true;
    }
    removeOutputFile(tracePath);
    if (!rejectedMismatchedUniqueId)
    {
        throw std::runtime_error("map writer self-test accepted mismatched native structures trace key/UniqueID");
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::filesystem::path errorReportOutput = findErrorReportOutput(argc, argv);
    try
    {
        const CliOptions cli = parseArgs(argc, argv);

        if (cli.showVersion)
        {
            std::cout << kGeneratorVersion << '\n';
            return 0;
        }

        const bool gameNativeMode = cli.mode == "game-native";
        const bool verifyNativeOptionsReportMode = cli.mode == "verify-native-options-report";
        const bool nativeGameInputMode = gameNativeMode || verifyNativeOptionsReportMode;
        const bool generationMode = cli.mode == "native" || gameNativeMode;
        const bool hasGameNativeInputs = cli.hasRandomizationSeed || cli.hasGameTerrainType || cli.hasClimate || cli.hasMapSize || cli.hasTimeOfDay;
        if (!nativeGameInputMode && hasGameNativeInputs)
        {
            throw std::runtime_error(
                "game-native options require --mode game-native or --mode verify-native-options-report");
        }
        if (!gameNativeMode && cli.hasNativeOptionsReportOutput)
        {
            throw std::runtime_error("--native-options-report-out is only supported by --mode game-native");
        }
        if (!verifyNativeOptionsReportMode && cli.hasNativeOptionsReportHex)
        {
            throw std::runtime_error(
                "--native-options-report-hex is only supported by --mode verify-native-options-report");
        }
        if (nativeGameInputMode)
        {
            if (!cli.hasRandomizationSeed || !cli.hasGameTerrainType || !cli.hasTimeOfDay || !cli.hasClimate || !cli.hasMapSize || !cli.hasResources || !cli.hasPlayers)
            {
                throw std::runtime_error(
                    "--mode " + cli.mode + " requires --randomization-seed, --game-terrain-type, --time-of-day, "
                                           "--climate, --map-size, --resources, and --players");
            }
            if (cli.hasSed)
            {
                throw std::runtime_error("--sed cannot be combined with --mode game-native");
            }
            if (cli.hasSeed || cli.hasWidth || cli.hasHeight || cli.hasTheater || cli.hasMapType || cli.hasTime || cli.hasWaterAmount || cli.hasTiberium || cli.hasTiberiumLayout || cli.hasVegetation || cli.hasRuggedness || cli.hasUrbanPresence || cli.hasAccessibility || cli.hasRegionSize || cli.hasTerrainType)
            {
                throw std::runtime_error("low-level native options cannot be combined with --mode game-native");
            }
        }
        if (cli.hasProductOutput && !generationMode)
        {
            throw std::runtime_error("--product-out is only supported by generation modes");
        }
        if (cli.hasEvaluationReportOutput && !generationMode)
        {
            throw std::runtime_error("--evaluation-report-out is only supported by generation modes");
        }
        if (cli.hasProductOutput && cli.productOutput.empty())
        {
            throw std::runtime_error("--product-out requires a non-empty file path");
        }
        if (cli.hasProductOutput && cli.format != "yrini")
        {
            throw std::runtime_error("--product-out requires --format yrini for the preview output");
        }
        if (cli.hasProductOutput && outputPathsReferToSameFile(cli.output, cli.productOutput))
        {
            throw std::runtime_error("--out and --product-out must be different files");
        }
        if (cli.hasEvaluationReportOutput && cli.evaluationReportOutput.empty())
        {
            throw std::runtime_error("--evaluation-report-out requires a non-empty file path");
        }
        if (cli.hasErrorReportOutput && cli.errorReportOutput.empty())
        {
            throw std::runtime_error("--error-report-out requires a non-empty file path");
        }
        if (cli.hasNativeOptionsReportOutput && cli.nativeOptionsReportOutput.empty())
        {
            throw std::runtime_error("--native-options-report-out requires a non-empty file path");
        }
        if (cli.hasEvaluationReportOutput && outputPathsReferToSameFile(cli.output, cli.evaluationReportOutput))
        {
            throw std::runtime_error("--out and --evaluation-report-out must be different files");
        }
        if (cli.hasProductOutput && cli.hasEvaluationReportOutput && outputPathsReferToSameFile(cli.productOutput, cli.evaluationReportOutput))
        {
            throw std::runtime_error("--product-out and --evaluation-report-out must be different files");
        }
        if (cli.hasNativeOptionsReportOutput && (outputPathsReferToSameFile(cli.output, cli.nativeOptionsReportOutput) || (cli.hasProductOutput && outputPathsReferToSameFile(cli.productOutput, cli.nativeOptionsReportOutput)) || (cli.hasEvaluationReportOutput && outputPathsReferToSameFile(cli.evaluationReportOutput, cli.nativeOptionsReportOutput)) || (cli.hasErrorReportOutput && outputPathsReferToSameFile(cli.errorReportOutput, cli.nativeOptionsReportOutput))))
        {
            throw std::runtime_error("--native-options-report-out must be different from other output files");
        }
        if (cli.hasProductOutput)
        {
            requireOutputDoesNotExist(cli.output, "--out");
            requireOutputDoesNotExist(cli.productOutput, "--product-out");
        }
        if (cli.hasNativeOptionsReportOutput)
        {
            requireOutputDoesNotExist(cli.output, "--out");
            requireOutputDoesNotExist(cli.nativeOptionsReportOutput, "--native-options-report-out");
        }

        if (verifyNativeOptionsReportMode)
        {
            const bool hasInputFile = !cli.input.empty();
            if (hasInputFile == cli.hasNativeOptionsReportHex)
            {
                throw std::runtime_error(
                    "--mode verify-native-options-report requires exactly one of --input FILE or --native-options-report-hex HEX");
            }
            rmg::RmgConfig config;
            if (!cli.rmgConfigPath.empty())
            {
                config = rmg::loadRmgConfig(cli.rmgConfigPath, config);
            }
            const rmg::NativeGameOptions expected {
                cli.gameTerrainType,
                cli.timeOfDay,
                cli.climate,
                cli.mapSize,
                cli.resources,
                cli.players,
                cli.randomizationSeed,
            };
            const rmg::NativeGameOptionsResolution resolution = cli.hasNativeOptionsReportHex ? rmg::validateNativeGameOptionsReport(
                                                                                                    decodeLowerHexText(cli.nativeOptionsReportHex), expected, config) :
                                                                                                rmg::validateNativeGameOptionsReportFile(cli.input, expected, config);
            std::cout << "native options report valid: finalMapSeed="
                      << resolution.mapSeedOptions.seed
                      << " playableWidth=" << resolution.playableWidth
                      << " playableHeight=" << resolution.playableHeight
                      << " rngDrawCount=" << resolution.randomDrawCount << '\n';
            return 0;
        }

        if (cli.mode == "verify-competitive-yrini")
        {
            // 模式来自可信请求，禁止根据缺失字段把损坏的多人地图降级为纯地形。
            if (cli.input.empty() || !cli.hasPlayers)
            {
                throw std::runtime_error("--mode verify-competitive-yrini requires --input FILE and --players 0|2..8");
            }
            const auto result = rmg::validateCompetitiveYrIniFile(cli.input, cli.players);
            std::cout << rmg::describeValidation(result) << '\n';
            return 0;
        }

        if (cli.mode == "verify-yrini")
        {
            if (cli.input.empty())
            {
                throw std::runtime_error("--mode verify-yrini requires --input FILE");
            }
            if (cli.hasPlayers)
            {
                requireRange("players", cli.players, 2, 8);
            }
            const auto result = rmg::validateYrIniFile(cli.input, cli.hasPlayers ? cli.players : -1);
            std::cout << rmg::describeValidation(result) << '\n';
            return 0;
        }

        if (cli.mode == "verify-yrini-product")
        {
            if (cli.input.empty())
            {
                throw std::runtime_error("--mode verify-yrini-product requires --input FILE");
            }
            if (cli.hasPlayers)
            {
                requireRange("players", cli.players, 2, 8);
            }
            const auto result = rmg::validateYrIniProductFile(cli.input, cli.hasPlayers ? cli.players : -1);
            std::cout << rmg::describeValidation(result) << '\n';
            return 0;
        }

        if (cli.mode == "signature-yrini")
        {
            if (cli.input.empty())
            {
                throw std::runtime_error("--mode signature-yrini requires --input FILE");
            }
            const auto signature = rmg::signYrIniFile(cli.input);
            std::cout << rmg::describeSignature(signature) << '\n';
            return 0;
        }

        if (cli.mode == "inspect-yrini")
        {
            if (cli.input.empty())
            {
                throw std::runtime_error("--mode inspect-yrini requires --input FILE");
            }
            const auto inspection = rmg::inspectYrIniFile(cli.input);
            std::cout << rmg::describeInspection(inspection) << '\n';
            return 0;
        }

        if (cli.mode == "compare-yrini")
        {
            if (cli.input.empty() || cli.compare.empty())
            {
                throw std::runtime_error("--mode compare-yrini requires --input LEFT --compare RIGHT");
            }
            const auto comparison = rmg::compareYrIniFiles(cli.input, cli.compare);
            std::cout << rmg::describeComparison(comparison) << '\n';
            return 0;
        }

        if (cli.mode == "compare-stage-report")
        {
            if (cli.input.empty() || cli.compare.empty())
            {
                throw std::runtime_error("--mode compare-stage-report requires --input LEFT --compare RIGHT");
            }
            const auto left = readStageReportFile(cli.input);
            const auto right = readStageReportFile(cli.compare);
            const std::string comparison = describeStageReportComparison(left, right);
            if (cli.hasOutput)
            {
                writeTextOutputFile(cli.output, comparison);
            }
            std::cout << comparison;
            return 0;
        }

        if (cli.mode == "verify-codecs")
        {
            auto requireLzoRoundTrip = [](const char *label, const std::vector<std::uint8_t> &payload)
            {
                try
                {
                    if (ra2::pack::decodeLzo1x(ra2::pack::encodeLzo1x(payload), payload.size()) != payload)
                    {
                        throw std::runtime_error("decoded bytes differ");
                    }
                }
                catch (const std::exception &ex)
                {
                    throw std::runtime_error(std::string(label) + " LZO round-trip failed: " + ex.what());
                }
            };
            auto requireFormat5RoundTrip = [](const char *label, const std::vector<std::uint8_t> &payload)
            {
                try
                {
                    if (ra2::pack::decodeFormat5Lzo(ra2::pack::encodeFormat5Lzo(payload)) != payload)
                    {
                        throw std::runtime_error("decoded bytes differ");
                    }
                }
                catch (const std::exception &ex)
                {
                    throw std::runtime_error(std::string(label) + " Format5/LZO round-trip failed: " + ex.what());
                }
            };

            const std::vector<std::uint8_t> literal {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
            const auto encodedLiteral = ra2::pack::encodeLzo1xLiteral(literal);
            if (ra2::pack::decodeLzo1x(encodedLiteral, literal.size()) != literal)
            {
                throw std::runtime_error("literal LZO round-trip failed");
            }

            const std::vector<std::uint8_t> emptyPayload;
            if (ra2::pack::decodeLzo1x(ra2::pack::encodeLzo1xLiteral(emptyPayload), 0) != emptyPayload)
            {
                throw std::runtime_error("empty literal LZO round-trip failed");
            }

            const std::vector<std::uint8_t> maximumFormat80Chunk(
                std::numeric_limits<std::uint16_t>::max(),
                static_cast<std::uint8_t>('A'));
            if (ra2::pack::decodeFormat80(
                    ra2::pack::encodeFormat80(maximumFormat80Chunk),
                    maximumFormat80Chunk.size()) != maximumFormat80Chunk)
            {
                throw std::runtime_error("maximum-size Format80 chunk round-trip failed");
            }
            bool rejectedOversizedFormat80 = false;
            try
            {
                static_cast<void>(ra2::pack::encodeFormat80(std::vector<std::uint8_t>(static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1, static_cast<std::uint8_t>('A'))));
            }
            catch (const std::length_error &)
            {
                rejectedOversizedFormat80 = true;
            }
            if (!rejectedOversizedFormat80)
            {
                throw std::runtime_error("oversized Format80 chunk was not rejected");
            }

            bool rejectedZeroLineLength = false;
            try
            {
                static_cast<void>(ra2::pack::splitEncodedSection("A", 0));
            }
            catch (const std::invalid_argument &)
            {
                rejectedZeroLineLength = true;
            }
            if (!rejectedZeroLineLength)
            {
                throw std::runtime_error("zero encoded-section line length was not rejected");
            }

            const std::vector<std::uint8_t> expected {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
            const std::vector<std::uint8_t> withMatch {
                25,
                'A',
                'B',
                'C',
                'D',
                'E',
                'F',
                'G',
                'H',
                0xDD,
                0x00,
                'H',
                0x11,
                0x00,
                0x00,
            };
            if (ra2::pack::decodeLzo1x(withMatch, expected.size()) != expected)
            {
                throw std::runtime_error("matched LZO decode failed");
            }

            const auto encodedMatched = ra2::pack::encodeLzo1x(expected);
            requireLzoRoundTrip("matched sample", expected);
            if (ra2::pack::decodeLzo1x(encodedMatched, expected.size()) != expected)
            {
                throw std::runtime_error("matched LZO encoder round-trip mismatch");
            }

            const std::vector<std::uint8_t> tailBoundary {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', '1', '2', '3', '4'};
            requireLzoRoundTrip("tail-boundary sample", tailBoundary);

            requireFormat5RoundTrip("matched sample", expected);
            std::vector<std::uint8_t> repeatedPayload;
            repeatedPayload.reserve(4096);
            for (int i = 0; i < 256; ++i)
            {
                repeatedPayload.insert(repeatedPayload.end(), {'I', 'S', 'O', static_cast<std::uint8_t>(i & 7)});
            }
            requireFormat5RoundTrip("repeated payload", repeatedPayload);
            if (ra2::pack::encodeFormat5Lzo(repeatedPayload).size() >= ra2::pack::encodeFormat5LzoLiteral(repeatedPayload).size())
            {
                throw std::runtime_error("compressed Format5/LZO payload was not smaller than literal-only encoding");
            }
            std::cout << "codec verification ok\n";
            return 0;
        }

        if (cli.mode == "verify-map-writer")
        {
            verifyMapWriter();
            std::cout << "map writer verification ok\n";
            return 0;
        }

        if (cli.mode == "verify-map-evaluation")
        {
            rmg::verifyMapEvaluation();
            std::cout << "map evaluation verification ok\n";
            return 0;
        }

        if (cli.mode == "verify-native-game-options")
        {
            rmg::verifyNativeGameOptions();
            std::cout << "native game options verification ok: selectorCases=3584\n";
            return 0;
        }

        if (cli.mode == "shore-atlas" || cli.mode == "cliff-atlas")
        {
            rmg::TileCatalog tileCatalog = loadTileCatalogForCli(cli, cli.theater);

            rmg::MapGrid map;
            if (cli.mode == "cliff-atlas")
            {
                map = makeCliffAtlasMap(cli.width, cli.height, cli.hasTerrainType ? cli.terrainType : -1);
            }
            else
            {
                const rmg::ShorePiece encodedPiece = rmg::decodeShorePiece(cli.terrainType);
                map = (cli.hasTerrainType && rmg::isEncodedShorePiece(cli.terrainType)) ? makeShoreSubtileMap(cli.width, cli.height, encodedPiece.tile, encodedPiece.subTile) : makeShoreAtlasMap(cli.width, cli.height, cli.hasTerrainType ? cli.terrainType : -1);
            }
            rmg::MapDocument document;
            document.mode = cli.mode;
            document.seed = cli.seed;
            document.players = 0;
            document.tileCatalog = tileCatalog;
            document.metrics = {
                {cli.mode == "cliff-atlas" ? "CliffVariants" : "ShoreVariants", cli.hasTerrainType ? "1" : (cli.mode == "cliff-atlas" ? "40" : "42")},
            };
            if (cli.hasTerrainType)
            {
                document.metrics.push_back({cli.mode == "cliff-atlas" ? "CliffVariant" : "ShoreVariant", std::to_string(cli.terrainType)});
            }
            writeOutputMapFile(cli.output, map, document, cli.format);
            std::cout << "generated " << cli.mode << " map: " << cli.output.string() << '\n';
            return 0;
        }

        if (cli.mode == "native" || cli.mode == "game-native" || cli.mode == "native-stage-report")
        {
            rmg::MapSeedOptions options;
            rmg::RmgConfig config;
            if (!cli.sedPath.empty())
            {
                options = rmg::loadMapSeedOptions(cli.sedPath, options);
            }
            if (!cli.rmgConfigPath.empty())
            {
                config = rmg::loadRmgConfig(cli.rmgConfigPath, config);
            }
            std::optional<rmg::NativeGameOptions> nativeGameOptions;
            std::optional<rmg::NativeGameOptionsResolution> nativeGameResolution;
            if (cli.mode == "game-native")
            {
                nativeGameOptions = rmg::NativeGameOptions {
                    cli.gameTerrainType,
                    cli.timeOfDay,
                    cli.climate,
                    cli.mapSize,
                    cli.resources,
                    cli.players,
                    cli.randomizationSeed,
                };
                nativeGameResolution = rmg::deriveNativeGameOptions(*nativeGameOptions, config);
                options = nativeGameResolution->mapSeedOptions;
            }
            else
            {
                if (cli.hasSeed)
                {
                    options.seed = cli.seed;
                }
                if (cli.hasWidth)
                {
                    if (options.nativeSeedWidth < 0)
                    {
                        options.nativeSeedWidth = cli.width;
                    }
                    options.width = cli.width;
                    options.nativeWidthInterpolation = false;
                }
                if (cli.hasHeight)
                {
                    if (options.nativeSeedHeight < 0)
                    {
                        options.nativeSeedHeight = cli.height;
                    }
                    options.height = cli.height;
                    options.nativeHeightInterpolation = false;
                }
                if (cli.hasPlayers)
                {
                    options.numPlayers = cli.players;
                }
                if (cli.hasTheater)
                {
                    options.theater = cli.theater;
                }
                if (cli.hasMapType)
                {
                    options.mapType = cli.mapType;
                }
                if (cli.hasResources)
                {
                    options.resources = cli.resources;
                }
                if (cli.hasTime)
                {
                    options.time = cli.time;
                }
                if (cli.hasWaterAmount)
                {
                    options.waterAmount = cli.waterAmount;
                }
                if (cli.hasTiberium)
                {
                    options.tiberium = cli.tiberium;
                }
                if (cli.hasTiberiumLayout)
                {
                    options.tiberiumLayout = cli.tiberiumLayout;
                }
                if (cli.hasVegetation)
                {
                    options.vegetation = cli.vegetation;
                }
                if (cli.hasRuggedness)
                {
                    options.ruggedness = cli.ruggedness;
                }
                if (cli.hasUrbanPresence)
                {
                    options.urbanPresence = cli.urbanPresence;
                }
                if (cli.hasAccessibility)
                {
                    options.accessibility = cli.accessibility;
                }
                if (cli.hasRegionSize)
                {
                    options.regionSize = cli.regionSize;
                }
                if (cli.hasTerrainType)
                {
                    options.terrainTypeOverride = cli.terrainType;
                }
            }
            if (cli.hasNativeScenarioUniqueId)
            {
                options.nativeScenarioUniqueId = cli.nativeScenarioUniqueId;
            }
            validateNativeOptions(options, cli.mode == "game-native");

            std::optional<rmg::NativeTheaterCatalog> nativeTheaterCatalog;
            if (!cli.gameResourceDir.empty())
            {
                nativeTheaterCatalog.emplace(rmg::loadNativeTheaterCatalog(cli.gameResourceDir, options.theater));
            }
            rmg::NativeMapGenerator generator(
                nativeTheaterCatalog.has_value() ? &*nativeTheaterCatalog : nullptr);
            rmg::MapGrid map = generator.generate(options, config);
            if (cli.mode == "native-stage-report")
            {
                writeTextOutputFile(cli.output, rmg::describeNativeStageReport(generator.report()));
                std::cout << "generated native stage report: " << cli.output.string() << '\n';
                return 0;
            }
            if (generator.report().startsPlaced != options.numPlayers)
            {
                throw rmg::GenerationError(
                    rmg::GenerationFailureCode::StartsInsufficient,
                    "map generation placed " + std::to_string(generator.report().startsPlaced) + " of " + std::to_string(options.numPlayers) + " requested starts");
            }
            validateNativeExportWaypoints(generator.report(), options.numPlayers);

            rmg::TileCatalog tileCatalog = nativeTheaterCatalog.has_value() ? nativeTheaterCatalog->exportCatalog : loadTileCatalogForCli(cli, options.theater);
            auto document = rmg::makeNativeDocument(options, config, generator.report(), map);
            document.tileCatalog = tileCatalog;
            if (!cli.nativeStructuresTracePath.empty())
            {
                applyNativeStructureSaveTrace(document, cli.nativeStructuresTracePath);
            }
            else
            {
                applyNativeTechTraceStructureKeys(document, cli.nativeTechTracePath);
            }
            const std::string nativeOptionsReport = nativeGameResolution.has_value() ? rmg::serializeNativeGameOptionsReport(*nativeGameOptions, *nativeGameResolution) : std::string {};
            writeNativeOutputFiles(cli, map, document, nativeOptionsReport);
            const bool evaluationWritten = writeOptionalMapEvaluation(cli, generator.report().evaluation);
            std::cout << "generated " << cli.mode << " map: " << cli.output.string();
            if (cli.hasProductOutput)
            {
                std::cout << " product=" << cli.productOutput.string();
            }
            if (evaluationWritten)
            {
                std::cout << " evaluation=" << cli.evaluationReportOutput.string();
            }
            if (cli.hasNativeOptionsReportOutput)
            {
                std::cout << " native_options=" << cli.nativeOptionsReportOutput.string();
            }
            std::cout << '\n';
            return 0;
        }

        throw std::runtime_error("unknown mode: " + cli.mode);
    }
    catch (const rmg::GenerationError &ex)
    {
        writeErrorReport(errorReportOutput, rmg::generationFailureCodeName(ex.code()), ex.what());
        std::cerr << "rmg: " << ex.what() << "\n\n";
        printUsage();
        return 1;
    }
    catch (const std::exception &ex)
    {
        writeErrorReport(errorReportOutput, "RMG_GENERATION_FAILED", ex.what());
        std::cerr << "rmg: " << ex.what() << "\n\n";
        printUsage();
        return 1;
    }
}
