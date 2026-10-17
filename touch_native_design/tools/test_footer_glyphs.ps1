param(
    [string]$NdkRoot = "$env:ANDROID_HOME\ndk\27.2.12479018",
    [string]$MsvcRoot = $env:VCToolsInstallDir,
    [string]$WindowsSdkRoot = $env:WindowsSdkDir,
    [string]$WindowsSdkVersion = "$env:WindowsSDKVersion".TrimEnd('\'),
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectRoot = (Resolve-Path (Join-Path $kitRoot '..')).Path
$sourceRoot = Join-Path $projectRoot 'arm64_bridge\src'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot 'work\evidence\touch-arm64-20261007\footer-checks'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$compiler = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
foreach ($abi in @('aarch64')) {
    $androidArgs = @("--target=${abi}-linux-android24", '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC',
        '-DHORN_ENABLE_LOGGING=0', '-I', $sourceRoot)
    & $compiler @androidArgs '-c' (Join-Path $sourceRoot 'bl2_touch_widget_commands.cpp') `
        '-o' (Join-Path $OutputDirectory ('widget_commands.' + $abi + '.o'))
    if ($LASTEXITCODE -ne 0) { throw "Footer Android compile failed: $abi" }
}
$msvcBin = Join-Path $MsvcRoot 'bin\Hostx64\x64'
$hostArgs = @('/nologo', '/std:c++20', '/W4', '/WX', '/EHsc', '/MT', ('/I' + $sourceRoot),
    ('/external:I' + (Join-Path $MsvcRoot 'include')), '/external:W0', ('/Fo' + $OutputDirectory + '\'))
foreach ($part in @('ucrt','shared','um')) {
    $hostArgs += '/external:I' + (Join-Path $WindowsSdkRoot "Include\$WindowsSdkVersion\$part")
}
$common = @('bl2_touch_gfx_values.cpp', 'bl2_touch_widget_registry.cpp', 'bl2_touch_widget_gestures.cpp',
    'bl2_touch_widget_commands.cpp', 'bl2_touch_ui_transactions.cpp') | ForEach-Object { Join-Path $sourceRoot $_ }
$link = @('/link', ('/LIBPATH:' + (Join-Path $MsvcRoot 'lib\x64')))
foreach ($part in @('ucrt','um')) {
    $link += '/LIBPATH:' + (Join-Path $WindowsSdkRoot "Lib\$WindowsSdkVersion\$part\x64")
}
$fixture = Join-Path $OutputDirectory 'gfx_abi_fixture.txt'
python (Join-Path $PSScriptRoot 'verify_gfx_values.py') '--output' $fixture
if ($LASTEXITCODE -ne 0) { throw 'Footer original ELF ABI verification failed' }
$previousPath = $env:PATH
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    foreach ($name in @('ui_footer_glyph_test','ui_map_test','widget_commands_test')) {
        $executable = Join-Path $OutputDirectory ($name + '.exe')
        & (Join-Path $msvcBin 'cl.exe') @hostArgs ('/Fe' + $executable) @common `
            (Join-Path $kitRoot ('tests\' + $name + '.cpp')) @link
        if ($LASTEXITCODE -ne 0) { throw "Footer host compile failed: $name" }
        & $executable $fixture
        if ($LASTEXITCODE -ne 0) { throw "Footer host check failed: $name" }
    }
} finally { $env:PATH = $previousPath }
