param([int]$Delay = 40, [int]$Duration = 10)
# Captures NHL 27 under Nsight Systems: CPU sampling of the emulator plus the Vulkan GPU
# workload trace. Run from an elevated (administrator) PowerShell; the game closes when done.
# Report: nsys-gameplay.nsys-rep in the workspace root.
$root = 'C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX'
$kyty = Join-Path $root 'KytyPS5-2026-11-01-telemetry-Windows-x64'
$nsys = 'C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.1.3\target-windows-x64\nsys.exe'
Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item Env:VK_LOADER_LAYERS_DISABLE, Env:KYTY_RELAXED_CS_BARRIERS, Env:KYTY_SAMPLE_GPU -ErrorAction SilentlyContinue
$env:KYTY_PERF_CSV = Join-Path $root 'perf-nsys.log'
Set-Location $kyty
& $nsys profile --trace=vulkan --vulkan-gpu-workload=individual --sample=process-tree `
    --sampling-frequency=2000 --cpuctxsw=none --delay=$Delay --duration=$Duration --kill=true `
    --force-overwrite=true "--output=$(Join-Path $root 'nsys-gameplay')" `
    .\kyty_emulator.exe --game (Join-Path $root 'PPSA34063-app0') --no-splash `
    --printf-direction Silent --shader-cache --shader-optimization-type None `
    --screen-width 1280 --screen-height 720 --present-mode Mailbox --perf-overlay
Write-Host "nsys exit code $LASTEXITCODE; report: $(Join-Path $root 'nsys-gameplay.nsys-rep')"
