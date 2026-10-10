param(
    [string]$NdkRoot = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018',
    [string]$MsvcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231',
    [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$WindowsSdkVersion = '10.0.26100.0'
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
