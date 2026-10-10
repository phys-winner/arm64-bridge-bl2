param(
    [string]$NdkRoot = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018',
    [string]$MsvcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231',
    [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$WindowsSdkVersion = '10.0.26100.0',
    [switch]$BackEdgesOnly
)
$ErrorActionPreference = 'Stop'
$backSource = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '../../arm64_bridge/src/bl2_touch_widget_sink.cpp')
$backCallers = [ordered]@{
    NativeBack = '(?s)if\s*\(frame\.fresh\s*&&\s*edit\.settings\.takeNativeBack\(\).*?\)\s*\{(?<body>.*?)\r?\n\s*\}'
    EditResume = '(?s)if\s*\(\s*back\s*\)\s*\{(?<body>.*?)\r?\n\s*\}'
}
$backEdge = 'edit\.(?<sink>router|navigation)\.action\s*\(\s*\{\s*Action::Back\s*,\s*(?<down>true|false)\s*,\s*frame\.epoch\s*,\s*0\s*\}\s*\)\s*;'
function Assert-Bl2SyntheticBackEdges([string]$Text) {
    foreach ($caller in $backCallers.GetEnumerator()) {
        $blocks = [regex]::Matches($Text,$caller.Value)
        if ($blocks.Count -ne 1) { throw "Synthetic Back caller missing/ambiguous: $($caller.Key)" }
        $body = [regex]::Replace($blocks[0].Groups['body'].Value,'(?s:/\*.*?\*/)|//[^\r\n]*','')
        $edges = ([regex]::Matches($body,$backEdge) | ForEach-Object { $_.Groups['sink'].Value + ':' + $_.Groups['down'].Value }) -join ','
        if ($edges -ne 'router:true,navigation:true,navigation:false,router:false') {
            throw "Synthetic Back edges unbalanced: $($caller.Key) ($edges)"
        }
    }
}
Assert-Bl2SyntheticBackEdges $backSource
# Remove each actual caller's router UP in memory; both faults must be caught.
foreach ($caller in $backCallers.GetEnumerator()) {
    $block = [regex]::Match($backSource,$caller.Value)
    $up = [regex]::Matches($block.Value,$backEdge) | Where-Object { $_.Groups['sink'].Value -eq 'router' -and $_.Groups['down'].Value -eq 'false' }
    $fault = $backSource.Remove($block.Index + $up.Index,$up.Length)
    $rejected = $false
    try { Assert-Bl2SyntheticBackEdges $fault } catch {
        if (-not $_.Exception.Message.StartsWith("Synthetic Back edges unbalanced: $($caller.Key) ")) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "Synthetic Back missing-UP fault was admitted: $($caller.Key)" }
}
Write-Output 'PASS: production NativeBack/EditResume edge balance; both missing-router-UP controls rejected'
if ($BackEdgesOnly) { return }
. (Join-Path $PSScriptRoot 'check_widget_commands.ps1') -NdkRoot $NdkRoot -MsvcRoot $MsvcRoot `
    -WindowsSdkRoot $WindowsSdkRoot -WindowsSdkVersion $WindowsSdkVersion
foreach ($name in @('bl2_touch_widget_router', 'bl2_touch_widget_sink')) {
    & $compiler '--target=aarch64-linux-android24' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fPIC' `
        '-I' $sourceRoot '-I' (Join-Path $projectRoot 'arm64_bridge\third_party\_deps\dynarmic-src\src') `
        '-c' (Join-Path $sourceRoot ($name + '.cpp')) `
        '-o' (Join-Path $buildRoot ($name + '.android.o'))
    if ($LASTEXITCODE -ne 0) { throw "Widget Android compilation failed: $name" }
}
$routerExecutable = Join-Path $buildRoot 'widget_router_test.exe'
$routerArgs = $commandArgs | Where-Object { $_ -ne ('/Fe' + $commandExecutable) -and $_ -ne (Join-Path $kitRoot 'tests\widget_commands_test.cpp') }
$linkAt = [Array]::IndexOf($routerArgs, '/link')
$routerArgs = @(('/Fe' + $routerExecutable)) + @($routerArgs[0..($linkAt - 1)]) + @((Join-Path $sourceRoot 'bl2_touch_widget_router.cpp'), (Join-Path $kitRoot 'tests\widget_router_test.cpp')) + @($routerArgs[$linkAt..($routerArgs.Length - 1)])
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @routerArgs
    if ($LASTEXITCODE -ne 0) { throw 'Widget router test compilation failed' }
} finally { $env:PATH = $previousPath }
& $routerExecutable (Join-Path $buildRoot 'gfx_abi_fixture.txt')
if ($LASTEXITCODE -ne 0) { throw 'Widget router test failed' }
