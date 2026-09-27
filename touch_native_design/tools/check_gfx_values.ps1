param(
    [string]$NdkRoot = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018',
    [string]$MsvcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231',
    [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$WindowsSdkVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectRoot = (Resolve-Path (Join-Path $kitRoot '..')).Path
$sourceRoot = Join-Path $projectRoot 'arm64_bridge\src'
$buildRoot = Join-Path $kitRoot 'arm64_bridge\build_checks'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
python (Join-Path $PSScriptRoot 'verify_gfx_values.py')
if ($LASTEXITCODE -ne 0) { throw 'GFx preimage verification failed' }
$compiler = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
foreach ($name in @('bl2_touch_gfx_values', 'bl2_touch_gfx_transport', 'bl2_touch_widget_registry',
                   'bl2_touch_widget_probe', 'bl2_touch_widget_gestures')) {
    & $compiler '--target=aarch64-linux-android24' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fPIC' `
        '-I' $sourceRoot '-I' (Join-Path $projectRoot 'arm64_bridge\third_party\_deps\dynarmic-src\src') `
        '-c' (Join-Path $sourceRoot ($name + '.cpp')) '-o' (Join-Path $buildRoot ($name + '.android.o'))
    if ($LASTEXITCODE -ne 0) { throw "Android compile failed: $name" }
}
$msvcBin = Join-Path $MsvcRoot 'bin\Hostx64\x64'
$executable = Join-Path $buildRoot 'gfx_values_test.exe'
$hostArgs = @('/nologo', '/std:c++17', '/W4', '/WX', '/EHsc', '/MT', ('/I' + $sourceRoot),
              ('/external:I' + (Join-Path $MsvcRoot 'include')), '/external:W0',
              ('/Fo' + $buildRoot + '\'), ('/Fe' + $executable))
foreach ($sdkInclude in @('ucrt','shared','um')) {
    $hostArgs += '/external:I' + (Join-Path $WindowsSdkRoot "Include\$WindowsSdkVersion\$sdkInclude")
}
$hostArgs += @((Join-Path $sourceRoot 'bl2_touch_gfx_values.cpp'),
               (Join-Path $kitRoot 'tests\gfx_values_test.cpp'), '/link',
               ('/LIBPATH:' + (Join-Path $MsvcRoot 'lib\x64')))
foreach ($sdkLibrary in @('ucrt','um')) {
    $hostArgs += '/LIBPATH:' + (Join-Path $WindowsSdkRoot "Lib\$WindowsSdkVersion\$sdkLibrary\x64")
}
$previousPath = $env:PATH
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @hostArgs
    if ($LASTEXITCODE -ne 0) { throw 'GFx session test compilation failed' }
} finally { $env:PATH = $previousPath }
& $executable (Join-Path $buildRoot 'gfx_abi_fixture.txt')
if ($LASTEXITCODE -ne 0) { throw 'GFx session test failed' }
$widgetExecutable = Join-Path $buildRoot 'widget_gestures_test.exe'
$widgetArgs = $hostArgs | Where-Object {
    $_ -ne ('/Fe' + $executable) -and $_ -ne (Join-Path $kitRoot 'tests\gfx_values_test.cpp')
}
$linkAt = [Array]::IndexOf($widgetArgs, '/link')
$widgetArgs = @(('/Fe' + $widgetExecutable)) + @($widgetArgs[0..($linkAt - 1)]) + @(
    (Join-Path $sourceRoot 'bl2_touch_widget_registry.cpp'),
    (Join-Path $sourceRoot 'bl2_touch_widget_gestures.cpp'),
    (Join-Path $kitRoot 'tests\widget_gestures_test.cpp')) + @($widgetArgs[$linkAt..($widgetArgs.Length - 1)])
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @widgetArgs
    if ($LASTEXITCODE -ne 0) { throw 'Widget gesture test compilation failed' }
} finally { $env:PATH = $previousPath }
& $widgetExecutable
if ($LASTEXITCODE -ne 0) { throw 'Widget gesture test failed' }
