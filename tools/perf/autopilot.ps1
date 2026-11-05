param(
    # Save the current screen's prompt-bar signature under this name in nav-refs.txt and exit.
    [string] $Record = '',
    # Print the best-matching reference for the current screen and exit.
    [switch] $Identify,
    [int] $TimeoutSeconds = 600,
    # Gameplay is detected from this perf log (draws per flip); optional.
    [string] $PerfLog = '',
    [string] $Pipe = 'kyty-input',
    [string] $ShotDir = 'C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX\autopilot-shots'
)
# Drives NHL 27 from boot to Play Now CPU-vs-CPU gameplay through the emulator input pipe
# (KYTY_INPUT_PIPE=1); no window focus needed. Screens are recognised by their button-prompt bar.
# Path: sign-in notice (Cross) -> main menu, Play Now (Cross) -> Select Team (Triangle = CPU vs CPU)
# -> team/jersey confirmations (Cross x4) -> Start Game (Cross) -> gameplay.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'screen.ps1')
$refsPath = Join-Path $PSScriptRoot 'nav-refs.txt'
$threshold = 0.25

# Pipe client kept open for the whole run.
$client = $null; $writer = $null; $reader = $null
function Send-Pad([string] $cmd) {
    if (-not $script:client -or -not $script:client.IsConnected) {
        $script:client = New-Object System.IO.Pipes.NamedPipeClientStream '.', $Pipe, 'InOut'
        $script:client.Connect(2000)
        $script:writer = New-Object System.IO.StreamWriter $script:client
        $script:writer.AutoFlush = $true
        $script:reader = New-Object System.IO.StreamReader $script:client
    }
    $script:writer.WriteLine($cmd)
    $r = $script:reader.ReadLine()
    if ($r -like 'err*' -and $cmd -notlike 'shot *') { throw "pad '$cmd' -> $r" }
    $r
}


function Read-Refs {
    $refs = [ordered]@{}
    if (Test-Path $refsPath) {
        foreach ($line in Get-Content $refsPath) {
            if ($line -match '^(\w+)=([01]+)$') { $refs[$Matches[1]] = $Matches[2] }
        }
    }
    $refs
}

$shotPath = Join-Path $env:TEMP 'kyty-autopilot.bmp'
function Get-Screen {
    # Emulator-side capture of the guest frame: works minimized/unfocused, no host overlays.
    $r = Send-Pad "shot $shotPath"
    if ($r -notlike 'ok*') { return $null }
    $bytes = [System.IO.File]::ReadAllBytes($shotPath)
    $ms = New-Object System.IO.MemoryStream (, $bytes)
    $bmp = New-Object System.Drawing.Bitmap $ms
    $sig = [KytyScreen]::Signature($bmp)
    [pscustomobject]@{ Bitmap = $bmp; Sig = $sig }
}

function Find-Match($sig, $refs) {
    $best = $null; $bestD = 2.0
    foreach ($k in $refs.Keys) {
        $d = [KytyScreen]::Distance($sig, $refs[$k])
        if ($d -lt $bestD) { $bestD = $d; $best = $k }
    }
    if ($sig.IndexOf('1') -lt 0) { return @('blank', 0.0) }
    if ($bestD -le $threshold) { @($best, $bestD) } else { @('unknown', $bestD) }
}

function Close-Pad { if ($script:client) { $script:client.Dispose(); $script:client = $null } }

if ($Record) {
    $s = Get-Screen; if (-not $s) { Close-Pad; throw 'no emulator screenshot' }
    $lines = @(if (Test-Path $refsPath) { Get-Content $refsPath | Where-Object { $_ -notmatch "^$Record=" } })
    $lines += "$Record=$($s.Sig)"
    Set-Content -Path $refsPath -Value $lines -Encoding ascii
    New-Item -ItemType Directory -Force $ShotDir | Out-Null
    Save-KytyFrame $s.Bitmap (Join-Path $ShotDir "ref-$Record.png")
    Close-Pad
    "recorded $Record ($(($s.Sig.ToCharArray() | Where-Object { $_ -eq '1' }).Count) lit cells)"
    exit 0
}

$refs = Read-Refs
if ($Identify) {
    $s = Get-Screen; Close-Pad; if (-not $s) { throw 'no emulator screenshot' }
    $m = Find-Match $s.Sig $refs
    New-Item -ItemType Directory -Force $ShotDir | Out-Null
    $peek = Join-Path $ShotDir 'peek.png'
    Save-KytyFrame $s.Bitmap $peek 800
    "screen=$($m[0]) distance=$([math]::Round($m[1], 3)) image=$peek"
    exit 0
}

function Test-Gameplay {
    # In-arena rendering: >= 1500 draws per flip and >= 3 flips in each of the last 10 logged
    # seconds. The post-menu loading screen is shorter and stalls with flip-less seconds.
    if (-not $PerfLog -or -not (Test-Path $PerfLog)) { return $false }
    $lines = @(Get-Content $PerfLog -Tail 10 | Where-Object { $_ -like 't=*' })
    if ($lines.Count -lt 10) { return $false }
    foreach ($line in $lines) {
        $d = 0; $f = 0
        if ($line -match '\bDraws=(\d+)') { $d = [int]$Matches[1] }
        if ($line -match '\bFlipsRequested=(\d+)') { $f = [int]$Matches[1] }
        if ($f -lt 3 -or $d / $f -lt 1500) { return $false }
    }
    $true
}

# Action per screen: button and minimum seconds before pressing again on the same screen.
$actions = @{
    signin     = @('cross', 2.0)
    mainmenu   = @('cross', 3.0)
    selectteam = @('triangle', 2.0)
    cpuselect  = @('cross', 1.2)   # team and jersey confirmations share this prompt bar
    startgame  = @('cross', 4.0)
}
$start = Get-Date
$lastScreen = ''; $lastPress = [datetime]::MinValue; $presses = 0; $started = $false
$log = New-Object System.Collections.Generic.List[string]
function Say([string] $msg) {
    $line = '{0,6:N1}s {1}' -f ((Get-Date) - $start).TotalSeconds, $msg
    $log.Add($line); Write-Output $line
}
New-Item -ItemType Directory -Force $ShotDir | Out-Null
try {
    while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSeconds) {
        $s = $null
        try { $s = Get-Screen } catch { $script:client = $null }
        if (-not $s) {
            if (-not (Get-Process kyty_emulator -ErrorAction SilentlyContinue)) { Say 'emulator exited'; exit 2 }
            Start-Sleep -Milliseconds 500; continue
        }
        $m = Find-Match $s.Sig $refs
        $screen = $m[0]
        if ($screen -ne $lastScreen) {
            Say ("screen {0} (d={1:N2})" -f $screen, $m[1])
            $lastScreen = $screen; $presses = 0
            Save-KytyFrame $s.Bitmap (Join-Path $ShotDir ("{0:000}-{1}.png" -f [int]((Get-Date) - $start).TotalSeconds, $screen)) 640
        }
        if ($started) {
            if ($screen -in 'unknown', 'blank' -and (Test-Gameplay)) {
                Save-KytyFrame $s.Bitmap (Join-Path $ShotDir 'gameplay.png') 960
                Say 'gameplay'; exit 0
            }
            if (-not $PerfLog -and ((Get-Date) - $lastPress).TotalSeconds -gt 20 -and $screen -in 'unknown', 'blank') {
                Say 'left the menus (no perf log to confirm gameplay)'; exit 0
            }
        }
        if ($actions.ContainsKey($screen)) {
            $button, $gap = $actions[$screen]
            if (((Get-Date) - $lastPress).TotalSeconds -ge $gap -and $presses -lt 8) {
                Send-Pad "tap $button" | Out-Null
                $lastPress = Get-Date; $presses++
                Say "  tap $button"
                if ($screen -eq 'startgame') { $started = $true }
            }
        }
        $s.Bitmap.Dispose()
        Start-Sleep -Milliseconds 250
    }
    Say 'timeout'; exit 1
} finally {
    try { if ($client) { Send-Pad 'release' | Out-Null; $client.Dispose() } } catch { }
    $log | Set-Content (Join-Path $ShotDir 'autopilot.log')
}
