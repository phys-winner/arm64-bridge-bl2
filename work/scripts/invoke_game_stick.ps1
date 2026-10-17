param(
    [Parameter(Mandatory)][string]$RunDirectory,
    [Parameter(Mandatory)][string]$BoundsTrace,
    [Parameter(Mandatory)][ValidateSet('left','right')][string]$Stick,
    [Parameter(Mandatory)][ValidateSet('up','down','left','right')][string]$Direction,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9-]+$')][string]$Label,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9._:-]+$')]
    [string]$Serial,
    [ValidateRange(100,5000)][int]$HoldMs = 750
)
$ErrorActionPreference = 'Stop'
$adb = 'adb'
$runPath = (Resolve-Path -LiteralPath $RunDirectory).Path
$tracePath = (Resolve-Path -LiteralPath $BoundsTrace).Path
$recordPath = Join-Path $runPath "manual-$Label.json"
if (Test-Path -LiteralPath $recordPath) { throw "Input record already exists: $recordPath" }
$gamePid = ((& $adb -s $Serial shell pidof com.tt2kgames.Borderlands2) -join ' ').Trim()
if ($gamePid -notmatch '^\d+$') { throw 'Expected one live game process' }
$pattern = '\s' + $gamePid + '\s+\d+\s+I BL2LaunchControls: bounds ' + $Stick +
    '-stick screen=(\d+),(\d+) size=(\d+)x(\d+)'
$bounds = Select-String -LiteralPath $tracePath -Pattern $pattern | Select-Object -Last 1
if (!$bounds) { throw "No measured $Stick stick bounds for live PID $gamePid" }
$match = $bounds.Matches[0]
$left = [int]$match.Groups[1].Value
$top = [int]$match.Groups[2].Value
$width = [int]$match.Groups[3].Value
$height = [int]$match.Groups[4].Value
if ($width -le 0 -or $height -le 0) { throw 'Invalid measured stick size' }
$fractionX = 0.5
$fractionY = 0.5
switch ($Direction) {
    'up' { $fractionY = 0.2 }
    'down' { $fractionY = 0.8 }
    'left' { $fractionX = 0.2 }
    'right' { $fractionX = 0.8 }
}
$stickX = $left + [int][math]::Floor($width * $fractionX)
$stickY = $top + [int][math]::Floor($height * $fractionY)
$focusBefore = (& $adb -s $Serial shell dumpsys window displays) -join "`n"
if ($focusBefore -notmatch 'mCurrentFocus=.*com\.tt2kgames\.Borderlands2/') {
    throw 'Game is not focused; no input was sent'
}
$started = Get-Date -Format o
$response = (& $adb -s $Serial shell input touchscreen -d 0 swipe `
    $stickX $stickY $stickX $stickY $HoldMs 2>&1) -join "`n"
$inputExit = $LASTEXITCODE
$focusAfter = (& $adb -s $Serial shell dumpsys window displays) -join "`n"
$record = [pscustomobject]@{
    started=$started; finished=(Get-Date -Format o); serial=$Serial; pid=$gamePid;
    stick=$Stick; direction=$Direction; x=$stickX; y=$stickY; hold_ms=$HoldMs;
    bounds_trace=$tracePath; bounds_trace_line=$bounds.Line; focused_before=$true;
    focused_after=($focusAfter -match 'mCurrentFocus=.*com\.tt2kgames\.Borderlands2/');
    input_exit=$inputExit; input_output=$response
}
$record | ConvertTo-Json | Set-Content -LiteralPath $recordPath -Encoding utf8
$record | ConvertTo-Json
if ($inputExit -ne 0) { throw "Input command failed: $response" }
if (!$record.focused_after) { throw 'Game lost focus after recorded input' }
