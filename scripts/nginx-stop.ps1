# Stop the nginx web front end started by scripts\nginx-start.ps1 (w2-050) = nginx-web.ps1 -Action stop.
# Graceful "nginx -s quit" of OUR instance only (state file + pid file + image + start time must
# match); any other nginx on this machine is never touched.
# w2-062: stop does not read config.json (the running instance is found through its state file).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\nginx-stop.ps1 [-RuntimeDir <folder>] [-TimeoutSec 20]
# Exit codes: 0 stopped; 1 no nginx started by nginx-start.ps1 is running; 7 stop failed.
param(
    [string]$RuntimeDir = "",
    [int]$TimeoutSec = 20
)
& (Join-Path $PSScriptRoot 'nginx-web.ps1') -Action stop @PSBoundParameters
exit $LASTEXITCODE
