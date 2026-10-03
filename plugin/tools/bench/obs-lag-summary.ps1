# Prints OBS's encode/render lag counters and AirPlay lines from the newest OBS log. Read-only.
param([string]$LogDir = "$env:APPDATA\obs-studio\logs")
$log = Get-ChildItem $LogDir -Filter *.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
"LOG " + $log.FullName
Select-String -Path $log.FullName -Pattern @(
  "skipped frames due to encoding lag",
  "lagged frames due to rendering lag",
  "Number of lagged frames",
  "frames missed due to rendering lag",
  "Output '.*': Total frames output",
  "Output '.*': Number of dropped frames",
  "\[obs-(airplay|overflow)\]"
) | ForEach-Object { $_.Line }
