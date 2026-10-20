param(
    [string]$Jdk = "$env:JAVA_HOME\bin",
    [string]$AndroidJar = "$env:ANDROID_HOME\platforms\android-36\android.jar",
    [string]$D8Jar = (Join-Path $PSScriptRoot '..\..\..\tools\android\sdk\build-tools\37.0.0\lib\d8.jar')
)
$ErrorActionPreference = 'Stop'
$kit = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = Join-Path $kit 'build\java'
New-Item -ItemType Directory -Force -Path "$output\classes", "$output\dex", "$output\tests" | Out-Null
$source = @(Get-ChildItem -LiteralPath (Join-Path $kit '..\android_port\src') -Recurse -Filter '*.java' -File | ForEach-Object FullName)
& "$Jdk\javac.exe" --release 8 -encoding UTF-8 -Xlint:all -cp $AndroidJar -d "$output\classes" $source
if ($LASTEXITCODE) { throw 'Java compilation failed' }
$test = @('TouchInputTest.java','GameStorageTest.java','storage_stubs\android\util\Log.java') |
    ForEach-Object { Join-Path $kit "tests\$_" }
& "$Jdk\javac.exe" --release 8 -encoding UTF-8 -cp "$output\classes;$AndroidJar" -d "$output\tests" $test
if ($LASTEXITCODE) { throw 'Java test compilation failed' }
& "$Jdk\java.exe" -cp "$output\classes;$output\tests;$AndroidJar" com.tt2kgames.Borderlands2.touch.TouchInputTest
if ($LASTEXITCODE) { throw 'Java behavioral tests failed' }
& "$Jdk\java.exe" -cp "$output\classes;$output\tests;$AndroidJar" com.tt2kgames.Borderlands2.GameStorageTest
if ($LASTEXITCODE) { throw 'Save migration tests failed' }
& "$Jdk\jar.exe" --create --file "$output\touch-classes.jar" -C "$output\classes" .
if ($LASTEXITCODE) { throw 'JAR creation failed' }
& "$Jdk\java.exe" -cp $D8Jar com.android.tools.r8.D8 --min-api 24 --lib $AndroidJar --output "$output\dex" "$output\touch-classes.jar"
if ($LASTEXITCODE) { throw 'D8 verification failed' }
Write-Output 'Java, behavioral checks and D8 passed; nothing copied to app/.'
