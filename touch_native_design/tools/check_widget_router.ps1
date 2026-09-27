param(
    [string]$NdkRoot = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018',
    [string]$MsvcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231',
    [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$WindowsSdkVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
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
