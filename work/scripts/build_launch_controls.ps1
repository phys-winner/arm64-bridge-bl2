$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root 'work\build'))
$temporary = Join-Path $buildRoot ('launch-controls-' + [guid]::NewGuid().ToString('N'))
$jdk = 'C:\Portable\jdk-24.0.1\bin'
$androidJar = 'C:\Users\Xilonen\AppData\Local\Android\Sdk\platforms\android-36\android.jar'
$d8Jar = 'D:\ai_test\old_game_restoration\tools\android\sdk\build-tools\37.0.0\lib\d8.jar'
$toolsRoot = 'D:\ai_test\old_game_restoration\tools\android'
$relativePackage = 'com\tt2kgames\Borderlands2'
$portNames = @('LaunchControls','GameStorage','DisplayModePolicy','Downloader','GameRestart',
    'LocalPlatform','LocalIdentity','LocalSaveGame','LocalAchievements')
$source = @($portNames | ForEach-Object { Join-Path $root "android_port\src\$relativePackage\$_.java" })
$touchNames = @('NativeTouchControls','TouchBridge','TouchContract','TouchInput','TouchLayout','TouchPreferences','TouchSources','TouchGyro')
foreach ($name in $touchNames) {
    $source += Join-Path $root "android_port\src\$relativePackage\touch\$name.java"
}
New-Item -ItemType Directory -Path (Join-Path $temporary 'classes'),(Join-Path $temporary 'dex') | Out-Null
try {
    & "$jdk\javac.exe" --release 8 -cp $androidJar -d (Join-Path $temporary 'classes') $source
    if ($LASTEXITCODE) { throw 'Launch controls javac failed' }
    & "$jdk\jar.exe" --create --file (Join-Path $temporary 'classes.jar') -C (Join-Path $temporary 'classes') .
    if ($LASTEXITCODE) { throw 'Launch controls jar failed' }
    & "$jdk\java.exe" -cp $d8Jar com.android.tools.r8.D8 --min-api 24 --lib $androidJar --output (Join-Path $temporary 'dex') (Join-Path $temporary 'classes.jar')
    if ($LASTEXITCODE) { throw 'Launch controls d8 failed' }
    & "$jdk\java.exe" -jar "$toolsRoot\baksmali.jar" d (Join-Path $temporary 'dex\classes.dex') -o (Join-Path $temporary 'smali')
    if ($LASTEXITCODE) { throw 'Launch controls baksmali failed' }
    # D8's synthetic lambda marker is not a runtime dependency of the game.
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $temporary 'smali') -Recurse -Filter '*.smali') {
        $text = [IO.File]::ReadAllText($file.FullName)
        $text = [regex]::Replace($text, '(?ms)^\.annotation runtime Lcom/android/tools/r8/annotations/LambdaMethod;\r?\n.*?^\.end annotation\r?\n', '')
        [IO.File]::WriteAllText($file.FullName, $text, [Text.UTF8Encoding]::new($false))
    }
    $generated = @(Get-ChildItem -LiteralPath (Join-Path $temporary "smali\$relativePackage") -File)
    $portPattern = '^(?:' + ($portNames -join '|') + ')(?:\$[^\\]+)?\.smali$'
    if (!$generated -or ($generated | Where-Object { $_.Name -notmatch $portPattern })) { throw 'Unexpected generated class ownership' }
    $portDestination = Join-Path $root "app\smali\$relativePackage"
    foreach ($old in Get-ChildItem -LiteralPath $portDestination -File) {
        if ($old.Name -match $portPattern -and $old.Name -notin $generated.Name) { Remove-Item -LiteralPath $old.FullName }
    }
    foreach ($file in $generated) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $root "app\smali\$relativePackage\$($file.Name)") -Force
    }
    $touchGenerated = @(Get-ChildItem -LiteralPath (Join-Path $temporary "smali\$relativePackage\touch") -File)
    if (!$touchGenerated -or ($touchGenerated | Where-Object { $_.Name -notmatch '^(?:NativeTouchControls|TouchBridge|TouchContract|TouchInput|TouchLayout|TouchPreferences|TouchSources|TouchGyro)(?:\$[^\\]+)?\.smali$' })) {
        throw 'Unexpected generated touch class ownership'
    }
    $touchDestination = Join-Path $root "app\smali\$relativePackage\touch"
    [IO.Directory]::CreateDirectory($touchDestination) | Out-Null
    foreach ($old in Get-ChildItem -LiteralPath $touchDestination -File) {
        if ($old.Name -match '^(?:NativeTouchControls|TouchBridge|TouchContract|TouchInput|TouchLayout|TouchPreferences|TouchSources|TouchGyro)(?:\$[^\\]+)?\.smali$' -and
            $old.Name -notin $touchGenerated.Name) { Remove-Item -LiteralPath $old.FullName }
    }
    foreach ($file in $touchGenerated) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $touchDestination $file.Name) -Force
    }
    "Android port helpers generated $($generated.Count) classes and $($touchGenerated.Count) touch classes"
} finally {
    $resolved = [IO.Path]::GetFullPath($temporary)
    if (!$resolved.StartsWith($buildRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^launch-controls-[0-9a-f]{32}$') { throw 'Unsafe temporary cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
