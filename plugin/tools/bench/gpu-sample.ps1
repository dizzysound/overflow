# Samples NVIDIA GPU utilization once a second for -Minutes and writes a CSV.
# Read-only: runs nvidia-smi queries only. Usage (on the streaming PC):
#   powershell -ExecutionPolicy Bypass -File C:\airplay-helper\bench\gpu-sample.ps1 -Minutes 100
param([int]$Minutes = 100, [string]$OutDir = "C:\airplay-helper\bench")
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir ("gpu-" + (Get-Date -Format "yyyyMMdd-HHmm") + ".csv")
$fields = "timestamp,utilization.gpu,utilization.encoder,utilization.decoder,encoder.stats.sessionCount,encoder.stats.averageFps,encoder.stats.averageLatency,memory.used,temperature.gpu,clocks.video"
$job = Start-Process -FilePath nvidia-smi -ArgumentList @("--query-gpu=$fields", "--format=csv", "-lms", "1000", "-f", $out) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds ($Minutes * 60)
Stop-Process -Id $job.Id -Force
Write-Output "wrote $out"
