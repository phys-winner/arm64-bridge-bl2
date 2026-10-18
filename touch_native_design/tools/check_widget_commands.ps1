param(
    [string]$NdkRoot = "$env:ANDROID_HOME\ndk\27.2.12479018",
    [string]$MsvcRoot = $env:VCToolsInstallDir,
    [string]$WindowsSdkRoot = $env:WindowsSdkDir,
    [string]$WindowsSdkVersion = "$env:WindowsSDKVersion".TrimEnd('\')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'check_gfx_values.ps1') -NdkRoot $NdkRoot -MsvcRoot $MsvcRoot `
    -WindowsSdkRoot $WindowsSdkRoot -WindowsSdkVersion $WindowsSdkVersion
& $compiler '--target=aarch64-linux-android24' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fPIC' `
    '-I' $sourceRoot '-c' (Join-Path $sourceRoot 'bl2_touch_widget_commands.cpp') `
    '-o' (Join-Path $buildRoot 'bl2_touch_widget_commands.android.o')
if ($LASTEXITCODE -ne 0) { throw 'Widget commands Android compilation failed' }
$commandExecutable = Join-Path $buildRoot 'widget_commands_test.exe'
$commandArgs = $widgetArgs | Where-Object { $_ -ne ('/Fe' + $widgetExecutable) -and $_ -ne (Join-Path $kitRoot 'tests\widget_gestures_test.cpp') }
$commandArgs = @($commandArgs | ForEach-Object { if ($_ -eq '/std:c++17') { '/std:c++20' } else { $_ } })
$linkAt = [Array]::IndexOf($commandArgs, '/link')
$commandArgs = @(('/Fe' + $commandExecutable)) + @($commandArgs[0..($linkAt - 1)]) + @((Join-Path $sourceRoot 'bl2_touch_widget_commands.cpp'), (Join-Path $kitRoot 'tests\widget_commands_test.cpp')) + @($commandArgs[$linkAt..($commandArgs.Length - 1)])
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @commandArgs
    if ($LASTEXITCODE -ne 0) { throw 'Widget command test compilation failed' }
} finally { $env:PATH = $previousPath }
& $commandExecutable (Join-Path $buildRoot 'gfx_abi_fixture.txt')
if ($LASTEXITCODE -ne 0) { throw 'Widget command test failed' }
# HIDE GAMEPAD BUTTONS: the Android object of the StringAliasMap rewrite and its host markup checks.
& $compiler '--target=aarch64-linux-android24' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fPIC' `
    '-I' $sourceRoot '-I' (Join-Path $projectRoot 'arm64_bridge\third_party\_deps\dynarmic-src\src') `
    '-c' (Join-Path $sourceRoot 'bl2_touch_gamepad_glyphs.cpp') '-o' (Join-Path $buildRoot 'bl2_touch_gamepad_glyphs.android.o')
if ($LASTEXITCODE -ne 0) { throw 'Gamepad glyph Android compilation failed' }
$glyphExecutable = Join-Path $buildRoot 'gamepad_glyphs_test.exe'
$glyphArgs = $hostArgs | Where-Object { $_ -ne ('/Fe' + $executable) -and $_ -ne (Join-Path $sourceRoot 'bl2_touch_gfx_values.cpp') -and
    $_ -ne (Join-Path $kitRoot 'tests\gfx_values_test.cpp') }
$linkAt = [Array]::IndexOf($glyphArgs, '/link')
$glyphArgs = @(('/Fe' + $glyphExecutable), '/DBL2_GAMEPAD_GLYPHS_HOST_TEST') + @($glyphArgs[0..($linkAt - 1)]) +
    @((Join-Path $sourceRoot 'bl2_touch_gamepad_glyphs.cpp'), (Join-Path $kitRoot 'tests\gamepad_glyphs_test.cpp')) +
    @($glyphArgs[$linkAt..($glyphArgs.Length - 1)])
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @glyphArgs
    if ($LASTEXITCODE -ne 0) { throw 'Gamepad glyph test compilation failed' }
} finally { $env:PATH = $previousPath }
& $glyphExecutable
if ($LASTEXITCODE -ne 0) { throw 'Gamepad glyph test failed' }
