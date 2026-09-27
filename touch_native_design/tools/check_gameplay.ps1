param(
    [string]$NdkRoot = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018',
    [string]$MsvcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231',
    [string]$WindowsSdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$WindowsSdkVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectRoot = (Resolve-Path (Join-Path $kitRoot '..')).Path
$canonicalSource = Join-Path $projectRoot 'arm64_bridge\src'
$coreSource = Join-Path $kitRoot 'arm64_bridge'
$buildRoot = Join-Path $coreSource 'build_checks'
$compilerPath = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
foreach ($name in @('bl2_touch_gameplay_sink', 'bl2_touch_gameplay_hooks', 'bl2_touch_physical')) {
    $source = Join-Path $canonicalSource ($name + '.cpp')
    $object = Join-Path $buildRoot ($name + '.android.o')
    & $compilerPath '--target=aarch64-linux-android24' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fPIC' `
        '-I' $canonicalSource '-I' (Join-Path $projectRoot 'arm64_bridge\third_party\_deps\dynarmic-src\src') `
        '-c' $source '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "Android compile failed: $source" }
}
Write-Output 'PASS: gameplay sink, native axis hooks, physical ownership compile for AArch64'
$msvcBin = Join-Path $MsvcRoot 'bin\Hostx64\x64'
$executable = Join-Path $buildRoot 'physical_ownership_test.exe'
$hostArgs = @('/nologo', '/std:c++17', '/W4', '/WX', '/EHsc', '/MT', ('/I' + $canonicalSource),
              ('/external:I' + (Join-Path $MsvcRoot 'include')), '/external:W0',
              ('/Fo' + $buildRoot + '\'), ('/Fe' + $executable))
foreach ($sdkInclude in @('ucrt','shared','um')) {
    $hostArgs += '/external:I' + (Join-Path $WindowsSdkRoot "Include\$WindowsSdkVersion\$sdkInclude")
}
$hostArgs += @((Join-Path $canonicalSource 'bl2_touch_physical.cpp'), (Join-Path $coreSource 'bl2_touch_input.cpp'),
               (Join-Path $kitRoot 'tests\physical_ownership_test.cpp'), '/link',
               ('/LIBPATH:' + (Join-Path $MsvcRoot 'lib\x64')))
foreach ($sdkLibrary in @('ucrt','um')) {
    $hostArgs += '/LIBPATH:' + (Join-Path $WindowsSdkRoot "Lib\$WindowsSdkVersion\$sdkLibrary\x64")
}
$previousPath = $env:PATH
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @hostArgs
    if ($LASTEXITCODE -ne 0) { throw 'Physical ownership test compilation failed' }
} finally { $env:PATH = $previousPath }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Physical ownership test failed' }
