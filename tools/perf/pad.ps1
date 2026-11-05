param(
    # Commands for the emulator input pipe (KYTY_INPUT_PIPE), e.g. 'tap triangle' 'wait 2000'.
    # 'wait <ms>' is handled here. See src/graphics/presentation/window/inputPipe.cpp.
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]] $Commands = @('ping'),
    [string] $Pipe = 'kyty-input',
    [int] $ConnectMs = 5000
)
if (-not ($Commands | Where-Object { $_ -notmatch '^\s*wait\s+\d+\s*$' })) {
    foreach ($c in $Commands) { Start-Sleep -Milliseconds ([int]($c -replace '\D', '')) }
    exit 0
}
$client = New-Object System.IO.Pipes.NamedPipeClientStream '.', $Pipe, 'InOut'
$client.Connect($ConnectMs)
$writer = New-Object System.IO.StreamWriter $client
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader $client
try {
    foreach ($c in $Commands) {
        if ($c -match '^\s*wait\s+(\d+)\s*$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
        $writer.WriteLine($c)
        $reply = $reader.ReadLine()
        "$c -> $reply"
        if ($reply -like 'err*') { exit 1 }
    }
} finally { $client.Dispose() }
