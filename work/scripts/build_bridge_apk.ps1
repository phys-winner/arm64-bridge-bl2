param(
    [Parameter(Mandatory)][ValidateRange(1,99999)][int]$Seed,
    [ValidateRange(0,99999)][int]$PreviousSeed = 0,
    [ValidatePattern('^[a-zA-Z0-9-]+$')][string]$NativeBuildDir = 'bridge-seed2',
    [switch]$Diagnostics,
    [switch]$PhysxCookieWatch,
    [switch]$Release,
    # A release whose bridge never writes to logcat and whose process drops the
    # BL2*/Arm32Bridge* Java tags on Android 11+ (HORN_SILENT_LOGCAT).
    [switch]$Final
)
$ErrorActionPreference = 'Stop'
if ($PhysxCookieWatch -and !$Diagnostics) { throw 'PhysX cookie watch requires a diagnostic build' }
if ($Final -and $Diagnostics) { throw 'A final build excludes diagnostics' }
$releaseBuild = $Release.IsPresent -or $Final.IsPresent
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = Join-Path $root 'work\build'
$stagePack = Get-Item -LiteralPath (Join-Path $root 'app/assets/bl2_shader_stages.bin')
if ($stagePack.Length -le 56 -or $stagePack.Length -gt 256MB) { throw 'Invalid full shader-stage asset size' }
$stagePackHash = (Get-FileHash -LiteralPath $stagePack.FullName).Hash
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
[IO.Directory]::CreateDirectory($build) | Out-Null
try {
    if (Test-Path -LiteralPath (Join-Path $root 'android_port\src\com\tt2kgames\Borderlands2\LaunchControls.java')) {
        & (Join-Path $PSScriptRoot 'build_launch_controls.ps1')
    }
    $cmake = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\cmake\3.22.1\bin\cmake.exe'
    $diagnosticFlag = if ($Diagnostics) { 'ON' } else { 'OFF' }
    $physxDiagnosticFlag = if ($PhysxCookieWatch) { 'ON' } else { 'OFF' }
    $debugMenuFlag = if ($releaseBuild) { 'OFF' } else { 'ON' }
    $silentFlag = if ($Final) { 'ON' } else { 'OFF' }
    if (!(Test-Path -LiteralPath (Join-Path $nativeBuild 'CMakeCache.txt'))) {
        $ndk = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018'
        & $cmake -S (Join-Path $root 'arm64_bridge') -B $nativeBuild -G Ninja `
            "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" `
            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release `
            '-DCMAKE_MAKE_PROGRAM=C:\Users\Xilonen\AppData\Local\Android\Sdk\cmake\3.22.1\bin\ninja.exe'
        if ($LASTEXITCODE) { throw 'Fresh offline native configuration failed' }
    }
    & $cmake -S (Join-Path $root 'arm64_bridge') -B $nativeBuild "-DHORN_ENABLE_LOGGING=$diagnosticFlag" "-DHORN_ENABLE_PERF_PROBE=$diagnosticFlag" "-DHORN_ENABLE_PHYSX_COOKIE_WATCH=$physxDiagnosticFlag" '-DHORN_GL_SELF_CHECK=OFF' "-DBL2_ENABLE_DEBUG_MENU_DEFAULT=$debugMenuFlag" "-DHORN_SILENT_LOGCAT=$silentFlag"
    if ($LASTEXITCODE) { throw 'Native configuration failed' }
    & $cmake --build $nativeBuild --config Release --parallel 8
    if ($LASTEXITCODE) { throw 'Native build failed' }
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'libUnrealEngine3.so') -Destination (Join-Path $root 'app\lib\arm64-v8a\libUnrealEngine3.so') -Force
    $nativePath = Join-Path $root 'app\lib\arm64-v8a\libUnrealEngine3.so'
    $nativeText = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($nativePath))
    if (!$nativeText.Contains('-nomovies -nostartupmovies') -or
        !$nativeText.Contains('debug.horn.play_movies')) {
        throw 'Arm64 bridge movie-skip option is missing from the native payload'
    }
    # The policy label is log-only and is removed from a silent build.
    # These properties are used by the actual resolution policy.
    if (!$nativeText.Contains('debug.bl2.render_scale') -or
        !$nativeText.Contains('debug.bl2.legacy_resolution')) {
        throw 'Verified native 100% resolution policy is missing from the bridge'
    }
    foreach ($bufferNameHook in @('bl2_fresh_persistent_vertex_name',
            'bl2_fresh_persistent_index_name', 'bl2_fresh_persistent_pixel_name')) {
        if (!$nativeText.Contains($bufferNameHook)) {
            throw "Required persistent buffer allocation hook is missing: $bufferNameHook"
        }
    }
    if ($PhysxCookieWatch -and
        (!$nativeText.Contains('debug.bl2.physx_cookie_watch') -or
         !$nativeText.Contains('PHYSX_COOKIE cause='))) {
        throw 'Requested PhysX diagnostic is missing from the native payload'
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
        foreach ($name in @('assets/guest_libs/libUnrealEngine3.so','lib/arm64-v8a/libUnrealEngine3.so',
                           'assets/bl2_cache/WillowGame/Movies/Attract.bik',
                           'assets/bl2_cache/WillowGame/CookedAndroid_DXT/Textures.tfc',
                           'assets/bl2_cache/WillowGame/CookedAndroid_DXT/CharTextures.tfc')) {
            $entry = $zip.GetEntry($name)
            if (!$entry -or $entry.Length -ne $entry.CompressedLength) { throw "STORED payload gate failed: $name" }
        }
        $stageEntry = $zip.GetEntry('assets/bl2_shader_stages.bin')
        if (!$stageEntry -or $stageEntry.Length -ne $stagePack.Length) { throw 'Full shader-stage asset missing from ARM64 APK' }
        $stageStream = $stageEntry.Open()
        $stageHasher = [Security.Cryptography.SHA256]::Create()
        try {
            $packedStageHash = [BitConverter]::ToString($stageHasher.ComputeHash($stageStream)).Replace('-','')
            if ($packedStageHash -ne $stagePackHash) { throw 'Packed full shader-stage asset differs' }
        } finally { $stageStream.Dispose(); $stageHasher.Dispose() }
    } finally { $zip.Dispose() }
    if ((Get-Item -LiteralPath $signed).Length -ge 2000000000L) { throw 'Base APK exceeds decimal 2 GB' }
    & 'C:\Program Files\Python313\python.exe' -X utf8 -B (Join-Path $PSScriptRoot 'verify_embedded_locale.py') --apk $signed
    if ($LASTEXITCODE) { throw 'Embedded Russian text payload gate failed' }
    if (!$Diagnostics) {
        $llvmBin = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin'
        $cache = Get-Content -LiteralPath (Join-Path $nativeBuild 'CMakeCache.txt') -Raw
        foreach ($flag in @('HORN_ENABLE_LOGGING','HORN_ENABLE_PERF_PROBE','HORN_ENABLE_PHYSX_COOKIE_WATCH','HORN_GL_SELF_CHECK')) {
            if ($cache -notmatch "(?m)^${flag}:BOOL=OFF\r?$") { throw "Quiet build flag gate failed: $flag" }
        }
        $exports = & "$llvmBin\llvm-nm.exe" -D --defined-only $nativePath
        if ($LASTEXITCODE -or ($exports -match 'Bl2Arm64GlParitySelfCheck|Bl2FreshPersistentBufferNamesSelfCheck')) { throw 'Gameplay bridge retains a GPU self-check entry' }
        if ($cache -notmatch "(?m)^HORN_SILENT_LOGCAT:BOOL=${silentFlag}\r?$") { throw 'Silent logcat flag gate failed' }
        if ($Final) {
            # --wrap leaves no import of a liblog write function behind.
            $imports = & "$llvmBin\llvm-nm.exe" -D --undefined-only $nativePath
            if ($LASTEXITCODE -or ($imports -match '\s__android_log_(write|print|vprint|buf_write|buf_print)(@\S*)?\s*$')) {
                throw 'Final bridge still imports a liblog write function'
            }
        }
    }
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
    bridge_logging=[bool]$Diagnostics.IsPresent;perf_probe_compiled=[bool]$Diagnostics.IsPresent;
    release_build=$releaseBuild;debug_menu_default_enabled=(!$releaseBuild);
    final_build=[bool]$Final.IsPresent;silent_logcat=[bool]$Final.IsPresent;
    russian_text_in_base=$true;app_locales=@('en','ru');cache_layout='rebalanced';
    bounded_backend_cache_display_logs=(!$Final.IsPresent);gl_self_check_compiled=$false;
    physx_cookie_watch_compiled=[bool]$PhysxCookieWatch.IsPresent;
    cmake_source_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'arm64_bridge\CMakeLists.txt')).Hash;
    fastmem_fpsr_patch_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'arm64_bridge\patches\dynarmic-arm64-fastmem-fpsr.patch')).Hash;
    sha256=(Get-FileHash -LiteralPath $signed).Hash;
    bridge_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'app\lib\arm64-v8a\libUnrealEngine3.so')).Hash;
    guest_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'app\assets\guest_libs\libUnrealEngine3.so')).Hash;
    stage_table_sha256=$stagePackHash;stage_table_bytes=$stagePack.Length;
    native_code='arm64-v8a';gles='3.2';zip_page_alignment=16384;
    movies_skip_default=$false;movies_skip_property='debug.horn.play_movies=0';
    render_scale_default='50%';render_scale_diagnostic_property='debug.bl2.render_scale';
    render_scale_user_min_percent=50;render_scale_user_max_percent=150;render_scale_user_step_percent=10;
    render_scale_user_preference='touch-controls.properties:renderPercent';
    ui_scale_default_percent=125;ui_scale_user_min_percent=100;ui_scale_user_max_percent=200;
    ui_scale_user_step_percent=10;ui_scale_user_preference='touch-controls.properties:uiPercent';
    ui_scale_default_first_decrement=120;ui_scale_default_first_increment=130;
    ui_scale_independent_from_render_resolution=$true;touch_settings_wire_version=5;
    performance_default_mask=33;performance_preferences=@('fasterVisibility','fasterAudioLimiter');
    quality_default='minimum original Android video options';low_quality_diagnostic_property='debug.bl2.low_quality=1';
    additional_graphics_defaults='original runtime values';potato_default=$false;potato_preserves_render_scale=$true;
    fps_limit_default='Display.getRefreshRate';fps_limit_diagnostic_property='debug.bl2.fps_limit=900';
    legacy_resolution_diagnostic_property='debug.bl2.legacy_resolution';
    signature=$verification;elapsed_s=$timer.Elapsed.TotalSeconds;deleted=$deleted;
    source_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'arm64_bridge\src') -File | Get-FileHash | Select-Object Path,Hash);
    bridge_patch_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'arm64_bridge\patches') -File | Get-FileHash | Select-Object Path,Hash);
    bridge_build_support_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'arm64_bridge\cmake') -File | Get-FileHash | Select-Object Path,Hash);
    bridge_test_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'arm64_bridge\tests') -File | Get-FileHash | Select-Object Path,Hash);
    warmup_asset_hashes=@(Get-FileHash -LiteralPath (Join-Path $root 'app\assets\bl2_shader_warmup.bin'),(Join-Path $root 'app\assets\bl2_jit_warmup.bin'),$stagePack.FullName | Select-Object Path,Hash);
    touch_core_source_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'touch_native_design\arm64_bridge') -File | Get-FileHash | Select-Object Path,Hash);
    android_source_hashes=@(Get-ChildItem -LiteralPath (Join-Path $root 'android_port\src') -Recurse -File | Get-FileHash | Select-Object Path,Hash);
    app_hook_hashes=@(Get-FileHash -LiteralPath (Join-Path $root 'app\smali\com\tt2kgames\Borderlands2\UE3JavaApp.smali'),(Join-Path $root 'app\smali\com\tt2kgames\Borderlands2\Downloader.smali'),(Join-Path $root 'app\AndroidManifest.xml'),(Join-Path $root 'app\apktool.yml') | Select-Object Path,Hash)}
$manifest | ConvertTo-Json -Depth 5 | Out-File -LiteralPath (Join-Path $build "seed$Seed-build.json") -Encoding utf8
"seed=$Seed apk=$signed sha256=$($manifest.sha256) elapsed_s=$($manifest.elapsed_s)"
