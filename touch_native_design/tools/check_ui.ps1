param(
    [string]$NdkRoot = "$env:ANDROID_HOME\ndk\27.2.12479018",
    [string]$MsvcRoot = $env:VCToolsInstallDir,
    [string]$WindowsSdkRoot = $env:WindowsSdkDir,
    [string]$WindowsSdkVersion = "$env:WindowsSDKVersion".TrimEnd('\')
)
$ErrorActionPreference = 'Stop'
$kitRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$projectRoot = (Resolve-Path (Join-Path $kitRoot '..')).Path
$sourceRoot = Join-Path $projectRoot 'arm64_bridge\src'
$buildRoot = Join-Path $kitRoot 'arm64_bridge\build_checks'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$compiler = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
foreach ($name in @('bl2_touch_ui_sink', 'bl2_touch_ui_transactions', 'bl2_touch_widget_sink')) {
    & $compiler '--target=aarch64-linux-android24' '-std=c++20' '-Wall' '-Wextra' '-Werror' '-fPIC' `
        '-I' $sourceRoot '-I' (Join-Path $projectRoot 'arm64_bridge\third_party\_deps\dynarmic-src\src') `
        '-c' (Join-Path $sourceRoot ($name + '.cpp')) '-o' (Join-Path $buildRoot ($name + '.android.o'))
    if ($LASTEXITCODE -ne 0) { throw "Android compile failed: $name" }
}
Write-Output 'PASS: native UI sink and transaction module compile for AArch64'
$msvcBin = Join-Path $MsvcRoot 'bin\Hostx64\x64'
$executable = Join-Path $buildRoot 'ui_transactions_test.exe'
$hostArgs = @('/nologo', '/std:c++20', '/W4', '/WX', '/EHsc', '/MT', ('/I' + $sourceRoot),
              ('/external:I' + (Join-Path $MsvcRoot 'include')), '/external:W0',
              ('/Fo' + $buildRoot + '\'), ('/Fe' + $executable))
foreach ($sdkInclude in @('ucrt','shared','um')) {
    $hostArgs += '/external:I' + (Join-Path $WindowsSdkRoot "Include\$WindowsSdkVersion\$sdkInclude")
}
$hostArgs += @((Join-Path $sourceRoot 'bl2_touch_ui_transactions.cpp'),
               (Join-Path $kitRoot 'tests\ui_transactions_test.cpp'), '/link',
               ('/LIBPATH:' + (Join-Path $MsvcRoot 'lib\x64')))
foreach ($sdkLibrary in @('ucrt','um')) {
    $hostArgs += '/LIBPATH:' + (Join-Path $WindowsSdkRoot "Lib\$WindowsSdkVersion\$sdkLibrary\x64")
}
$previousPath = $env:PATH
try {
    $env:PATH = $msvcBin + ';' + $previousPath
    & (Join-Path $msvcBin 'cl.exe') @hostArgs
    if ($LASTEXITCODE -ne 0) { throw 'UI transaction test compilation failed' }
} finally { $env:PATH = $previousPath }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'UI transaction test failed' }
& python -X utf8 -B (Join-Path $PSScriptRoot 'test_footer_keys.py') $buildRoot
if ($LASTEXITCODE -ne 0) { throw 'Production footer key extraction failed' }
$footerExecutable = Join-Path $buildRoot 'ui_footer_key_test.exe'
$footerArgs = @($hostArgs | Where-Object {
    $_ -ne ('/Fe' + $executable) -and $_ -ne (Join-Path $sourceRoot 'bl2_touch_ui_transactions.cpp') -and
    $_ -ne (Join-Path $kitRoot 'tests\ui_transactions_test.cpp')
})
$linkAt = [Array]::IndexOf($footerArgs, '/link')
$footerArgs = @(('/I' + $buildRoot), ('/Fe' + $footerExecutable)) + @($footerArgs[0..($linkAt-1)]) +
    @(Join-Path $kitRoot 'tests\ui_footer_key_test.cpp') + @($footerArgs[$linkAt..($footerArgs.Length-1)])
& (Join-Path $msvcBin 'cl.exe') @footerArgs
if ($LASTEXITCODE -ne 0) { throw 'Footer key test compilation failed' }
& $footerExecutable
if ($LASTEXITCODE -ne 0) { throw 'Footer key test failed' }
