param(
    [ValidateRange(1,99999)][int]$Seed = 1,
    [switch]$Quiet,
    [switch]$QaConsole,
    [switch]$BaseOnly,
    [switch]$Release,
    [switch]$PerfProbe,
    [string]$VulkanAsset
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = Join-Path $root 'work\build'
$native = Join-Path $build 'native-arm32'
$stage = Join-Path $build 'native-arm32-app'
$warmup = Join-Path $root 'app/assets/bl2_shader_warmup.bin'
$warmupHash = (Get-FileHash -LiteralPath $warmup).Hash
$stagePack = Get-Item -LiteralPath (Join-Path $root 'app/assets/bl2_shader_stages.bin')
if ($stagePack.Length -le 56 -or $stagePack.Length -gt 256MB) { throw 'Invalid full shader-stage asset size' }
$stagePackHash = (Get-FileHash -LiteralPath $stagePack.FullName).Hash
$output = Join-Path $build "bl2-arm32-native-seed$Seed.apk"
if (Test-Path -LiteralPath $output) { throw "Candidate already exists: $output" }
$vulkanPack = $null
$vulkanHash = $null
$vulkanMetadata = $null
if ($VulkanAsset) {
    $vulkanPack = Get-Item -LiteralPath $VulkanAsset
    if ($vulkanPack.Length -le 256 -or $vulkanPack.Length -gt 768MB) { throw 'Invalid Vulkan shader asset size' }
    $vulkanHash = (Get-FileHash -LiteralPath $vulkanPack.FullName).Hash
    $vulkanMetadata = Get-Content -LiteralPath ($vulkanPack.FullName + '.json') -Raw | ConvertFrom-Json
    if ($vulkanMetadata.status -ne 'COMPLETE' -or $vulkanMetadata.normalized_stages -ne 38980 -or
        $vulkanMetadata.raw_stages -ne 38980 -or !$vulkanMetadata.all_spirv_resources_reflected -or
        $vulkanMetadata.bytes -ne $vulkanPack.Length -or $vulkanMetadata.sha256 -ne $vulkanHash) {
        throw 'Vulkan pack provenance/coverage gate failed'
    }
}
$cmake = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\cmake\3.22.1\bin\cmake.exe'
$ndk = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018'
$java = 'C:\Portable\jdk-24.0.1\bin\java.exe'
$tools = 'D:\ai_test\old_game_restoration\tools\android'
$android = Join-Path $tools 'sdk\build-tools\37.0.0'
$signer = 'C:\Portable\Total.Commander\Utilites\Just Another Tool for APK\bin'
$logging = if ($Quiet) { 'OFF' } else { 'ON' }
$qaConsoleFlag = if ($QaConsole) { 'ON' } else { 'OFF' }
$dlcContentFlag = if ($BaseOnly) { 'OFF' } else { 'ON' }
$debugMenuDefault = if ($Release) { 'OFF' } else { 'ON' }
$performanceProbe = if ($PerfProbe) { 'ON' } else { 'OFF' }
$timer = [Diagnostics.Stopwatch]::StartNew()
& $cmake -S (Join-Path $root 'arm32_compat') -B $native -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release '-DCMAKE_C_FLAGS_RELEASE:STRING=-O2 -DNDEBUG' '-DCMAKE_CXX_FLAGS_RELEASE:STRING=-O2 -DNDEBUG' '-DCMAKE_ASM_FLAGS_RELEASE:STRING=-O2 -DNDEBUG' "-DHORN_ENABLE_LOGGING=$logging" "-DBL2_ENABLE_QA_CONSOLE=$qaConsoleFlag" "-DBL2_ENABLE_DLC_CONTENT=$dlcContentFlag" "-DBL2_ENABLE_DEBUG_MENU_DEFAULT=$debugMenuDefault" "-DBL2_NATIVE_PERF_PROBE=$performanceProbe" '-DCMAKE_MAKE_PROGRAM=C:/Users/Xilonen/AppData/Local/Android/Sdk/cmake/3.22.1/bin/ninja.exe'
if ($LASTEXITCODE) { throw 'ARM32 native configuration failed' }
$targets = @('UnrealEngine3','tsec_wrapper','native_host_check','native_ui_control_check')
if ($PerfProbe) { $targets += 'native_perf_check' }
& $cmake --build $native --parallel 8 --target @targets
if ($LASTEXITCODE) { throw 'ARM32 native build failed' }
# The per-ABI packaging tree is a generated derivative of canonical app/.
# It contains no independently edited application sources.
if (!$stage.StartsWith($build + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe ARM32 staging path' }
if (Test-Path -LiteralPath $stage) {
    if ((Get-Item -LiteralPath $stage).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'ARM32 stage is a link' }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'app') | Where-Object { $_.Name -notin @('build','dist','lib') } | Copy-Item -Destination $stage -Recurse -Force
$libraryDir = Join-Path $stage 'lib\armeabi-v7a'
New-Item -ItemType Directory -Path $libraryDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'app\assets\guest_libs\libUnrealEngine3.so') -Destination (Join-Path $libraryDir 'libUE3Runtime.so')
foreach ($name in @('libUnrealEngine3.so','libtsec_wrapper.so')) {
    Copy-Item -LiteralPath (Join-Path $native $name) -Destination (Join-Path $libraryDir $name)
    & "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-debug (Join-Path $libraryDir $name)
    if ($LASTEXITCODE) { throw "Native strip failed: $name" }
}
$guestDir = [IO.Path]::GetFullPath((Join-Path $stage 'assets\guest_libs'))
if (!$guestDir.StartsWith($stage + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe derived guest path' }
if (Test-Path -LiteralPath $guestDir) { Remove-Item -LiteralPath $guestDir -Recurse -Force }
$manifestPath = Join-Path $stage 'AndroidManifest.xml'
Copy-Item -LiteralPath $warmup -Destination (Join-Path $stage 'assets/bl2_shader_warmup.bin') -Force
if ($vulkanPack) {
    Copy-Item -LiteralPath $vulkanPack.FullName -Destination (Join-Path $stage 'assets/bl2_vulkan_stages.bin')
    $apktoolPath = Join-Path $stage 'apktool.yml'
    $apktoolConfig = [IO.File]::ReadAllText($apktoolPath)
    if ($apktoolConfig -notmatch '(?m)^doNotCompress:$') { throw 'APK compression list is missing' }
    $apktoolConfig = $apktoolConfig.Replace('doNotCompress:', "doNotCompress:`n- assets/bl2_vulkan_stages.bin")
    [IO.File]::WriteAllText($apktoolPath, $apktoolConfig, [Text.UTF8Encoding]::new($false))
}
$manifest = [IO.File]::ReadAllText($manifestPath)
if ($manifest -notmatch '<application ' -or $manifest -match 'android:extractNativeLibs=') { throw 'Unexpected canonical application manifest' }
$manifest = $manifest.Replace('<application ', '<application android:extractNativeLibs="true" ')
# Diagnostic Vulkan APKs permit the Android loader's app-scoped validation layer.
if ($PerfProbe -and $vulkanPack) {
    if ($manifest -notmatch '        <profileable ') { throw 'Missing diagnostic profileable manifest anchor' }
    $manifest = $manifest.Replace('    <application ',
        '    <queries><package android:name="com.tt2kgames.Borderlands2.vulkanlayer20261009"/></queries>' + "`n" + '    <application ')
    $manifest = $manifest.Replace('        <profileable ',
        '        <meta-data android:name="com.android.graphics.injectLayers.enable" android:value="true"/>' + "`n" + '        <profileable ')
}
[IO.File]::WriteAllText($manifestPath,$manifest,[Text.UTF8Encoding]::new($false))
$unsigned = Join-Path $build "bl2-arm32-native-seed$Seed-unsigned.apk"
$aligned = Join-Path $build "bl2-arm32-native-seed$Seed-aligned.apk"
try {
    & $java -jar (Join-Path $tools 'apktool.jar') b -f $stage -p (Join-Path $root 'work\tool-cache\apktool-framework') -o $unsigned
    if ($LASTEXITCODE) { throw 'ARM32 APK packaging failed' }
    & "$android\zipalign.exe" -P 16 -f 4 $unsigned $aligned
    if ($LASTEXITCODE) { throw 'ARM32 APK alignment failed' }
    & $java -jar "$signer\apksigner.jar" sign --key "$signer\testkey.pk8" --cert "$signer\testkey.x509.pem" --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true --out $output $aligned
    if ($LASTEXITCODE) { throw 'ARM32 APK signing failed' }
    $signature = & $java -jar "$signer\apksigner.jar" verify --verbose --print-certs $output
    if ($LASTEXITCODE -or !($signature -match 'a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc')) { throw 'ARM32 signature check failed' }
    & "$android\zipalign.exe" -c -P 16 4 $output
    if ($LASTEXITCODE) { throw 'Signed ARM32 alignment check failed' }
    $badging = & "$android\aapt2.exe" dump badging $output
    if ($LASTEXITCODE -or ($badging | Where-Object { $_ -match '^native-code:' }) -ne "native-code: 'armeabi-v7a'") { throw 'ARM32 APK ABI check failed' }
    $zip = [IO.Compression.ZipFile]::OpenRead($output)
    try {
        foreach ($name in @('libUE3Runtime.so','libUnrealEngine3.so','libtsec_wrapper.so')) {
            $entry = $zip.GetEntry("lib/armeabi-v7a/$name")
            if (!$entry -or $entry.Length -ne $entry.CompressedLength) { throw "Uncompressed native payload required: $name" }
        }
        if ($zip.GetEntry('assets/guest_libs/libUnrealEngine3.so')) { throw 'Duplicate JIT guest payload in native APK' }
        if ($vulkanPack) {
            $vulkanEntry = $zip.GetEntry('assets/bl2_vulkan_stages.bin')
            if (!$vulkanEntry -or $vulkanEntry.Length -ne $vulkanPack.Length -or
                $vulkanEntry.Length -ne $vulkanEntry.CompressedLength) { throw 'Vulkan pack must be stored without ZIP compression' }
            $vulkanStream = $vulkanEntry.Open()
            $vulkanHasher = [Security.Cryptography.SHA256]::Create()
            try {
                $packedVulkanHash = [BitConverter]::ToString($vulkanHasher.ComputeHash($vulkanStream)).Replace('-','')
                if ($packedVulkanHash -ne $vulkanHash) { throw 'Packaged Vulkan shader asset differs' }
            } finally { $vulkanStream.Dispose(); $vulkanHasher.Dispose() }
        }
        $stageEntry = $zip.GetEntry('assets/bl2_shader_stages.bin')
        if (!$stageEntry -or $stageEntry.Length -ne $stagePack.Length) { throw 'Full shader-stage asset missing from native APK' }
        $stageStream = $stageEntry.Open()
        $stageHasher = [Security.Cryptography.SHA256]::Create()
        try {
            $packedStageHash = [BitConverter]::ToString($stageHasher.ComputeHash($stageStream)).Replace('-','')
            if ($packedStageHash -ne $stagePackHash) { throw 'Packed full shader-stage asset differs' }
        } finally { $stageStream.Dispose(); $stageHasher.Dispose() }
        $warmupEntry = $zip.GetEntry('assets/bl2_shader_warmup.bin')
        if (!$warmupEntry -or $warmupEntry.Length -ne (Get-Item -LiteralPath $warmup).Length) { throw 'Hot shader-stage asset missing from native APK' }
        $warmupStream = $warmupEntry.Open()
        $warmupHasher = [Security.Cryptography.SHA256]::Create()
        try {
            $packedWarmupHash = [BitConverter]::ToString($warmupHasher.ComputeHash($warmupStream)).Replace('-','')
            if ($packedWarmupHash -ne $warmupHash) { throw 'Packed hot shader-stage asset differs' }
        } finally { $warmupStream.Dispose(); $warmupHasher.Dispose() }
    } finally { $zip.Dispose() }
    $receipt = [ordered]@{
        abi='armeabi-v7a'; seed=$Seed; signed_apk=$output; sha256=(Get-FileHash -LiteralPath $output).Hash;
        russian_text_in_base=$true; app_locales=@('en','ru'); cache_layout='full'; russian_voice_revision='r3-audio-only'; debug_menu_default_enabled=(!$Release);
        logging=(!$Quiet); qa_console_compiled=(!$Quiet -or $QaConsole.IsPresent); elapsed_seconds=[Math]::Round($timer.Elapsed.TotalSeconds,3);
        native_performance_probe=[bool]$PerfProbe;
        base_only=[bool]$BaseOnly.IsPresent;dlc_content_enabled=(!$BaseOnly.IsPresent);
        engine=(Get-FileHash -LiteralPath (Join-Path $libraryDir 'libUE3Runtime.so')).Hash;
        warmup_sha256=$warmupHash;
        stage_table_sha256=$stagePackHash; stage_table_bytes=$stagePack.Length;
        vulkan_asset_sha256=$vulkanHash; vulkan_asset_metadata=$vulkanMetadata;
        vulkan_build_tools=@(Get-Item -LiteralPath (Join-Path $root 'work/scripts/bake_bl2_vulkan_stages.py'),(Join-Path $root 'work/scripts/pack_bl2_vulkan_stages.py') | Get-FileHash | Select-Object Path,Hash);
        native=@(Get-ChildItem -LiteralPath $libraryDir -File | Get-FileHash | Select-Object Path,Hash);
        source=@(@(Get-ChildItem -LiteralPath (Join-Path $root 'arm32_compat\src'),(Join-Path $root 'arm32_compat\include'),(Join-Path $root 'arm32_compat\tests') -Recurse -File) + @(Get-Item -LiteralPath (Join-Path $root 'arm32_compat\CMakeLists.txt'),$PSCommandPath) | Get-FileHash | Select-Object Path,Hash);
        shared_source=@(Get-ChildItem -LiteralPath (Join-Path $root 'arm64_bridge\src'),(Join-Path $root 'touch_native_design\arm64_bridge') -File | Get-FileHash | Select-Object Path,Hash)
    }
    $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build "arm32-seed$Seed-build.json") -Encoding utf8
    "APK=$output SHA256=$($receipt.sha256) elapsed_s=$($receipt.elapsed_seconds)"
} finally {
    foreach ($path in @($unsigned,$aligned)) { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } }
}
