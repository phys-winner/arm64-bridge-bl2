param(
    [string]$NdkRoot = "$env:ANDROID_HOME\ndk\27.2.12479018",
    [string]$MsvcRoot = $env:VCToolsInstallDir,
    [string]$WindowsSdkRoot = $env:WindowsSdkDir,
    [string]$WindowsSdkVersion = "$env:WindowsSDKVersion".TrimEnd('\'),
    [switch]$SkipHost
)
$ErrorActionPreference = 'Stop'
$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sourceRoot = Join-Path $kitRoot 'arm64_bridge'
$buildRoot = Join-Path $sourceRoot 'build_checks'
$compilerPath = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) { throw "NDK clang not found: $compilerPath" }
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$common = @('-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', $sourceRoot)
$coreSources = @((Join-Path $sourceRoot 'bl2_touch_input.cpp'),
                 (Join-Path $sourceRoot 'bl2_touch_engine_adapter.cpp'))

# Actual AArch64 object generation checks JNI against the NDK jni.h and libc++.
# This does not build an APK, link the canonical bridge or exercise the guest.
foreach ($sourcePath in ($coreSources + (Join-Path $sourceRoot 'bl2_touch_jni.cpp'))) {
    $objectPath = Join-Path $buildRoot ([IO.Path]::GetFileNameWithoutExtension($sourcePath) + '.android.o')
    & $compilerPath @common '--target=aarch64-linux-android21' '-fPIC' '-c' $sourcePath '-o' $objectPath
    if ($LASTEXITCODE -ne 0) { throw "Android compile failed: $sourcePath" }
}
Write-Output 'PASS: Android AArch64 core, adapter and JNI object compilation'

if (-not $SkipHost) {
    # Host contract execution uses the matching local MSVC compiler/STL pair.
    # The NDK's Clang 18 cannot consume the installed MSVC STL (requires Clang 20).
    # Production Android sources above always use the recorded NDK toolchain.
    $testPath = Join-Path $kitRoot 'tests\native_input_test.cpp'
    $executablePath = Join-Path $buildRoot 'native_input_test.exe'
    $msvcBin = Join-Path $MsvcRoot 'bin\Hostx64\x64'
    $hostCompilerPath = Join-Path $msvcBin 'cl.exe'
    if (-not (Test-Path -LiteralPath $hostCompilerPath)) { throw "MSVC not found: $hostCompilerPath" }
    $hostArgs = @('/nologo', '/std:c++17', '/W4', '/WX', '/EHsc', '/MT',
                  ('/I' + $sourceRoot), ('/external:I' + (Join-Path $MsvcRoot 'include')),
                  '/external:W0', ('/Fo' + $buildRoot + '\'), ('/Fe' + $executablePath))
    foreach ($sdkInclude in @('ucrt', 'shared', 'um')) {
        $hostArgs += '/external:I' + (Join-Path $WindowsSdkRoot "Include\$WindowsSdkVersion\$sdkInclude")
    }
    $hostArgs += $coreSources + $testPath + '/link'
    $hostArgs += '/LIBPATH:' + (Join-Path $MsvcRoot 'lib\x64')
    foreach ($sdkLibrary in @('ucrt', 'um')) {
        $hostArgs += '/LIBPATH:' + (Join-Path $WindowsSdkRoot "Lib\$WindowsSdkVersion\$sdkLibrary\x64")
    }
    $oldExecutableSearchPath = $env:PATH
    try {
        $env:PATH = $msvcBin + ';' + $oldExecutableSearchPath
        & $hostCompilerPath @hostArgs
        if ($LASTEXITCODE -ne 0) { throw 'Host compile failed.' }
    } finally {
        $env:PATH = $oldExecutableSearchPath
    }
    & $executablePath
    if ($LASTEXITCODE -ne 0) { throw 'Native touch contract tests failed.' }
}
