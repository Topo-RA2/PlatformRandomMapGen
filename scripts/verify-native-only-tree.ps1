$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

function Assert-NoPath([string]$RelativePath) {
    $path = Join-Path $repoRoot $RelativePath
    if (Test-Path -LiteralPath $path) {
        throw "RMG 仓库不应内嵌独立组件: $RelativePath"
    }
}

function Assert-RequiredPath([string]$RelativePath) {
    $path = Join-Path $repoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "RMG 缺少必需实现: $RelativePath"
    }
}

Assert-NoPath "src/CompetitiveMapGen"
Assert-NoPath "src/competitive_map_gen.py"
Assert-NoPath "docs/native-rmg-analysis.md"
Assert-NoPath "docs/native-rmg-call-tree.generated.json"
Assert-NoPath "src/RA2AssetRenderer"
Assert-NoPath "src/advanced"
Assert-NoPath "include/rmg/advanced"
Assert-NoPath "src/advanced_strategy.cpp"
Assert-NoPath "include/rmg/advanced_strategy.h"
Assert-NoPath "src/main_native_legacy.cpp"

foreach ($path in @(
    "src/main.cpp",
    "src/yrini_validator.cpp",
    "include/rmg/yrini_validator.h"
)) {
    Assert-RequiredPath $path
}

$mainSource = Get-Content -LiteralPath (Join-Path $repoRoot "src/main.cpp") -Raw
foreach ($capability in @(
    "--error-report-out",
    "--native-options-report-hex",
    "verify-native-game-options",
    "verify-native-options-report",
    "verify-yrini-product",
    "verify-competitive-yrini"
)) {
    if (!$mainSource.Contains($capability)) {
        throw "RandomMapGen CLI is missing required capability: $capability"
    }
}

$assetDirectories = @("include", "src", "docs", "examples", "third_party", "tests")
$forbiddenExtensions = @(".mix", ".shp", ".vxl", ".pal", ".tmp", ".map")
$forbiddenAssets = @(
    $assetDirectories |
        ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $repoRoot $_) -Recurse -File } |
        Where-Object {
            $forbiddenExtensions -contains $_.Extension.ToLowerInvariant()
        }
)
if ($forbiddenAssets.Count -gt 0) {
    throw "仓库包含不允许分发的游戏或地图资源: $($forbiddenAssets.FullName -join ', ')"
}

$markdownFiles = Get-ChildItem -LiteralPath $repoRoot -Recurse -File -Filter "*.md" |
    Where-Object {
        $relativePath = [IO.Path]::GetRelativePath($repoRoot, $_.FullName)
        $relativePath -notmatch '^(build|\.git)([\\/]|$)'
    }
foreach ($file in $markdownFiles) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($match in [regex]::Matches($text, '\[[^\]]+\]\(([^)#]+)(?:#[^)]+)?\)')) {
        $target = $match.Groups[1].Value
        if ($target -match "^[a-z]+:" -or $target.StartsWith("#")) {
            continue
        }
        $resolved = [IO.Path]::GetFullPath((Join-Path $file.DirectoryName $target))
        if (!(Test-Path -LiteralPath $resolved)) {
            throw "Markdown 链接不存在: $($file.FullName) -> $target"
        }
    }
}

& (Join-Path $PSScriptRoot "verify-native-generator-layout.ps1")
Write-Host "RandomMapGen tree verification passed"
