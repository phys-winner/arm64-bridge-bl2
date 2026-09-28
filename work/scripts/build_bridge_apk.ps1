param(
    [Parameter(Mandatory)][ValidateRange(1,99999)][int]$Seed,
    [ValidateRange(0,99999)][int]$PreviousSeed = 0,
    [ValidatePattern('^[a-zA-Z0-9-]+$')][string]$NativeBuildDir = 'bridge-seed2'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = Join-Path $root 'work\build'
$nativeBuild = Join-Path $build $NativeBuildDir
$java = 'C:\Portable\jdk-24.0.1\bin\java.exe'
$apktool = 'D:\ai_test\old_game_restoration\tools\android\apktool.jar'
$androidTools = 'D:\ai_test\old_game_restoration\tools\android\sdk\build-tools\37.0.0'
$signer = 'C:\Portable\Total.Commander\Utilites\Just Another Tool for APK\bin'
$unsigned = Join-Path $build "bl2-arm64-bridge-seed$Seed-unsigned.apk"
$aligned = Join-Path $build "bl2-arm64-bridge-seed$Seed-aligned.apk"
$signed = Join-Path $build "bl2-arm64-bridge-seed$Seed.apk"
if (Test-Path -LiteralPath $signed) { throw "Signed candidate already exists: $signed" }
$deleted = @()
$timer = [Diagnostics.Stopwatch]::StartNew()
try {
    if (Test-Path -LiteralPath (Join-Path $root 'android_port\src\com\tt2kgames\Borderlands2\LaunchControls.java')) {
        & (Join-Path $PSScriptRoot 'build_launch_controls.ps1')
    }
    & 'C:\Users\Xilonen\AppData\Local\Android\Sdk\cmake\3.22.1\bin\cmake.exe' --build $nativeBuild --config Release --parallel
    if ($LASTEXITCODE) { throw 'Native build failed' }
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'libUnrealEngine3.so') -Destination (Join-Path $root 'app\lib\arm64-v8a\libUnrealEngine3.so') -Force
    $nativePath = Join-Path $root 'app\lib\arm64-v8a\libUnrealEngine3.so'
    $nativeText = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($nativePath))
    if (!$nativeText.Contains('-nomovies -nostartupmovies') -or
        !$nativeText.Contains('debug.horn.play_movies')) {
        throw 'Arm64 bridge movie-skip option is missing from the native payload'
    }
    if (!$nativeText.Contains('bl2-native-surface-v1') -or
        !$nativeText.Contains('debug.bl2.legacy_resolution')) {
        throw 'Verified native 100% resolution policy is missing from the bridge'
    }
    $nativeText = $null
    # Apktool -f can retain deleted resource files in its APK staging directory.
    $appRoot = [IO.Path]::GetFullPath((Join-Path $root 'app'))
    $apkStaging = [IO.Path]::GetFullPath((Join-Path $appRoot 'build'))
    if (!$apkStaging.StartsWith($appRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($apkStaging) -ne 'build') { throw 'Unsafe apktool staging path' }
    if (Test-Path -LiteralPath $apkStaging) {
        if ((Get-Item -LiteralPath $apkStaging).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing an apktool staging directory link'
        }
        Remove-Item -LiteralPath $apkStaging -Recurse -Force
    }
    & $java -jar $apktool b -f (Join-Path $root 'app') -p (Join-Path $root 'work\tool-cache\apktool-framework') -o $unsigned
    if ($LASTEXITCODE) { throw 'Apktool build failed' }
    & "$androidTools\zipalign.exe" -P 16 -f 4 $unsigned $aligned
    if ($LASTEXITCODE) { throw 'ZIP alignment failed' }
    & $java -jar "$signer\apksigner.jar" sign --key "$signer\testkey.pk8" --cert "$signer\testkey.x509.pem" --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true --out $signed $aligned
    if ($LASTEXITCODE) { throw 'Signing failed' }
    $verification = & $java -jar "$signer\apksigner.jar" verify --verbose --print-certs $signed
    if ($LASTEXITCODE -or !($verification -match 'a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc')) { throw 'Signature verification failed' }
    & "$androidTools\zipalign.exe" -c -P 16 4 $signed
    if ($LASTEXITCODE) { throw 'Signed ZIP alignment failed' }
    $badging = & "$androidTools\aapt2.exe" dump badging $signed
    if ($LASTEXITCODE -or ($badging | Where-Object { $_ -match '^native-code:' }) -ne "native-code: 'arm64-v8a'") { throw 'Native ABI gate failed' }
    if (!($badging -match "uses-gl-es: '0x30002'")) { throw 'GLES 3.2 manifest gate failed' }
    if (!($badging -match "minSdkVersion:'24'")) { throw 'Minimum SDK 24 gate failed' }
    if (!($badging -match "targetSdkVersion:'37'")) { throw 'Target SDK 37 gate failed' }
    $zip = [IO.Compression.ZipFile]::OpenRead($signed)
    try {
        foreach ($name in @('assets/guest_libs/libUnrealEngine3.so','lib/arm64-v8a/libUnrealEngine3.so')) {
            $entry = $zip.GetEntry($name)
            if (!$entry -or $entry.Length -ne $entry.CompressedLength) { throw "STORED payload gate failed: $name" }
        }
    } finally { $zip.Dispose() }
    if ($PreviousSeed -gt 0 -and $PreviousSeed -ne $Seed) {
        $previous = Join-Path $build "bl2-arm64-bridge-seed$PreviousSeed.apk"
        if (Test-Path -LiteralPath $previous) {
            $deleted += [pscustomobject]@{path=$previous;bytes=(Get-Item -LiteralPath $previous).Length;sha256=(Get-FileHash -LiteralPath $previous).Hash}
            [IO.File]::Delete($previous)
        }
    }
} finally {
    foreach ($path in @($unsigned,$aligned)) {
        if (Test-Path -LiteralPath $path) {
            $deleted += [pscustomobject]@{path=$path;bytes=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path).Hash}
            [IO.File]::Delete($path)
        }
    }
}
$manifest = [ordered]@{seed=$Seed;native_build_dir=$NativeBuildDir;path=$signed;bytes=(Get-Item -LiteralPath $signed).Length;
    sha256=(Get-FileHash -LiteralPath $signed).Hash;
    bridge_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'app\lib\arm64-v8a\libUnrealEngine3.so')).Hash;
    guest_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'app\assets\guest_libs\libUnrealEngine3.so')).Hash;
    native_code='arm64-v8a';gles='3.2';zip_page_alignment=16384;
    movies_skip_default=$true;movies_playback_property='debug.horn.play_movies';
    render_scale_default='native-surface-1:1';render_scale_diagnostic_property='debug.bl2.render_scale';
    quality_default='saved-game SystemSettings';low_quality_diagnostic_property='debug.bl2.low_quality=1';
    fps_limit_default='Display.getRefreshRate';fps_limit_diagnostic_property='debug.bl2.fps_limit=900';
    legacy_resolution_diagnostic_property='debug.bl2.legacy_resolution';
    signature=$verification;elapsed_s=$timer.Elapsed.TotalSeconds;deleted=$deleted;
    source_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'arm64_bridge\src') -File | Get-FileHash | Select-Object Path,Hash);
    touch_core_source_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'touch_native_design\arm64_bridge') -File | Get-FileHash | Select-Object Path,Hash);
    android_source_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'android_port\src') -Recurse -File | Get-FileHash | Select-Object Path,Hash);
    app_hook_hashes=@(Get-FileHash -LiteralPath (Join-Path $root 'app\smali\com\tt2kgames\Borderlands2\UE3JavaApp.smali'),(Join-Path $root 'app\smali\com\tt2kgames\Borderlands2\Downloader.smali'),(Join-Path $root 'app\AndroidManifest.xml'),(Join-Path $root 'app\apktool.yml') | Select-Object Path,Hash)}
$manifest | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $build "seed$Seed-build.json") -Encoding utf8
"seed=$Seed apk=$signed sha256=$($manifest.sha256) elapsed_s=$($manifest.elapsed_s)"
