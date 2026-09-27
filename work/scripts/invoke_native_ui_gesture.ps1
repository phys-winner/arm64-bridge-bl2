param(
    [Parameter(Mandatory)][string]$RunDirectory,
    [Parameter(Mandatory)][int]$ExpectedPid,
    [Parameter(Mandatory)][string]$SourceScreen,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9-]+$')][string]$Label,
    [Parameter(Mandatory)][ValidateRange(0,8191)][int]$X,
    [Parameter(Mandatory)][ValidateRange(0,8191)][int]$Y,
    [ValidateRange(-1,8191)][int]$EndX = -1,
    [ValidateRange(-1,8191)][int]$EndY = -1,
    [ValidateRange(50,3000)][int]$HoldMs = 100
)
$ErrorActionPreference = 'Stop'
$adb = 'C:/Users/Xilonen/AppData/Local/Android/Sdk/platform-tools/adb.exe'
$serial = '61151JEA326613'
$package = 'com.tt2kgames.Borderlands2'
$runPath = (Resolve-Path -LiteralPath $RunDirectory).Path
$screenPath = (Resolve-Path -LiteralPath $SourceScreen).Path
if (!$screenPath.StartsWith($runPath + '\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Gesture must refer to a screenshot from this run'
}
$sidecar = Get-Content -LiteralPath ($screenPath + '.json') -Raw | ConvertFrom-Json
if ($sidecar.pid -ne $ExpectedPid -or $sidecar.serial -ne $serial -or
    $sidecar.sha256 -ne (Get-FileHash -LiteralPath $screenPath).Hash) {
    throw 'Screenshot provenance does not match the expected game'
}
if (((Get-Date) - [datetime]$sidecar.captured).TotalSeconds -gt 180) {
    throw 'Screenshot is stale; capture the current UI before sending input'
}
$bytes = [IO.File]::ReadAllBytes($screenPath)
if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') {
    throw 'Source is not a PNG'
}
$width = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes,16))
$height = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes,20))
if ($EndX -lt 0) { $EndX = $X }; if ($EndY -lt 0) { $EndY = $Y }
if ($X -ge $width -or $EndX -ge $width -or $Y -ge $height -or $EndY -ge $height) {
    throw 'Gesture lies outside the observed surface'
}
$recordPath = Join-Path $runPath ('gesture-' + $Label + '.json')
if (Test-Path -LiteralPath $recordPath) { throw 'Gesture record already exists' }
$actualPid = ((& $adb -s $serial shell pidof $package) -join ' ').Trim()
$focus = (& $adb -s $serial shell dumpsys window displays) -join "`n"
if ($actualPid -ne $ExpectedPid.ToString() -or $focus -notmatch 'mCurrentFocus=.*com\.tt2kgames\.Borderlands2/') {
    throw 'Expected game process is not focused; no input sent'
}
$started = Get-Date -Format o
$response = (& $adb -s $serial shell input touchscreen -d 0 swipe $X $Y $EndX $EndY $HoldMs 2>&1) -join "`n"
$inputExit = $LASTEXITCODE
$focusAfter = (& $adb -s $serial shell dumpsys window displays) -join "`n"
$record = [pscustomobject]@{
    started=$started; finished=(Get-Date -Format o); serial=$serial; pid=$ExpectedPid;
    source_screen=$screenPath; source_sha256=$sidecar.sha256; width=$width; height=$height;
    x=$X; y=$Y; end_x=$EndX; end_y=$EndY; hold_ms=$HoldMs;
    input_exit=$inputExit; input_output=$response;
    focused_after=($focusAfter -match 'mCurrentFocus=.*com\.tt2kgames\.Borderlands2/')
}
$record | ConvertTo-Json | Set-Content -LiteralPath $recordPath -Encoding utf8
$record | ConvertTo-Json
if ($inputExit -ne 0) { throw 'ADB input failed; see the preserved gesture record' }
if (!$record.focused_after) { throw 'Game lost focus after recorded input' }
