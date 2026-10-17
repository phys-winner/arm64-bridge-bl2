$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $root 'work/evidence/touch-arm64-20261007'
$msvc = "$env:VCToolsInstallDir".TrimEnd('\')
$sdk = "$env:WindowsSdkDir".TrimEnd('\')
$version = "$env:WindowsSDKVersion".TrimEnd('\')
$previousPath = $env:PATH; $previousInclude = $env:INCLUDE; $previousLib = $env:LIB
try {
    $env:PATH = "$msvc/bin/Hostx64/x64;$env:PATH"
    $env:INCLUDE = "$msvc/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um"
    $env:LIB = "$msvc/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
    $exe = Join-Path $out 'bl2_graphics_settings_test.exe'
    & "$msvc/bin/Hostx64/x64/cl.exe" /nologo /std:c++20 /W4 /WX /EHsc /MT "/I$root/arm64_bridge/src" "/Fe$exe" "/Fo$out/bl2_graphics_settings_test.obj" "$PSScriptRoot/bl2_graphics_settings_test.cpp"
    if ($LASTEXITCODE -ne 0) { throw "Graphics check compilation failed: $LASTEXITCODE" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Graphics check failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $previousPath; $env:INCLUDE = $previousInclude; $env:LIB = $previousLib
}
