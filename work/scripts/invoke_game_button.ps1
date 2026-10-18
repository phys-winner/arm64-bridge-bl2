param(
    [Parameter(Mandatory)][string]$RunDirectory,
    [Parameter(Mandatory)][ValidateRange(1,255)][int]$KeyCode,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9-]+$')][string]$Label,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9._:-]+$')]
    [string]$Serial,
    [ValidateRange(100,2000)][int]$HoldMs = 250
)
$ErrorActionPreference = 'Stop'
$adb = 'adb'
$runPath = (Resolve-Path -LiteralPath $RunDirectory).Path
$trace = Join-Path $runPath 'trace.txt'
$recordPath = Join-Path $runPath "manual-$Label.json"
if (Test-Path -LiteralPath $recordPath) { throw "Input record already exists: $recordPath" }
$bounds = Select-String -LiteralPath $trace -Pattern (
    'bounds key=' + $KeyCode + ' screen=(\d+),(\d+) size=(\d+)x(\d+)') | Select-Object -Last 1
if (!$bounds) { throw "No current-run measured bounds for key $KeyCode" }
$match = $bounds.Matches[0]
$buttonX = [int]$match.Groups[1].Value + [int]([int]$match.Groups[3].Value / 2)
$buttonY = [int]$match.Groups[2].Value + [int]([int]$match.Groups[4].Value / 2)
$gamePid = ((& $adb -s $Serial shell pidof com.tt2kgames.Borderlands2) -join ' ').Trim()
$focusBefore = (& $adb -s $Serial shell dumpsys window displays) -join "`n"
if (!$gamePid -or $focusBefore -notmatch 'mCurrentFocus=.*com\.tt2kgames\.Borderlands2/') {
    throw 'The game is not alive and focused; no input was sent'
}
$started = Get-Date -Format o
$response = (& $adb -s $Serial shell input touchscreen -d 0 swipe `
    $buttonX $buttonY $buttonX $buttonY $HoldMs 2>&1) -join "`n"
$inputExit = $LASTEXITCODE
$focusAfter = (& $adb -s $Serial shell dumpsys window displays) -join "`n"
$record = [pscustomobject]@{
    started=$started; finished=(Get-Date -Format o); serial=$Serial; pid=$gamePid;
    key_code=$KeyCode; x=$buttonX; y=$buttonY; hold_ms=$HoldMs;
    bounds_trace_line=$bounds.Line; focused_before=$true;
    focused_after=($focusAfter -match 'mCurrentFocus=.*com\.tt2kgames\.Borderlands2/');
    input_exit=$inputExit; input_output=$response
}
$record | ConvertTo-Json | Set-Content -LiteralPath $recordPath -Encoding utf8
$record | ConvertTo-Json
if ($inputExit -ne 0) { throw "Input command failed: $response" }
if (!$record.focused_after) { throw 'The game lost focus after the recorded input' }
