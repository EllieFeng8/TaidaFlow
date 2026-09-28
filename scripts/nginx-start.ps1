# Start the nginx web front end on 0.0.0.0:80 (w2-050; port 80 since w2-057, -Port to change it)
# = scripts\nginx-web.ps1 -Action start.
# Deploy the page first:  scripts\deploy-web.ps1   (build\wasm-release -> <exe folder>\web + .gz)
# Usage: powershell -ExecutionPolicy Bypass -File scripts\nginx-start.ps1
#            [-WebRoot <folder>] [-ExeDir <folder>] [-ExportDir <folder>] [-RuntimeDir <folder>]
#            [-Nginx <nginx.exe or folder>] [-Port 80] [-RestPort 18080] [-Test]   (-Test: only render the configuration, run nginx -t)
# -RestPort: the app's internal REST port on 127.0.0.1 that http://<host>/api/ is proxied to (w2-060;
#   default 18080 or TAIDAFLOW_REST_PORT - the variable the app reads).
# -Port: listen port, default 80 (http://<IP>/); e.g. -Port 8123 for the port used before w2-057.
# -ExportDir: the app's export folder (default build\runtime-cwd\exports); nginx serves
#   /exports/<file> from it. Start the app with TAIDAFLOW_DOWNLOAD_PORT=<the same port, 80> so that
#   the download links point to nginx (without it the links keep using the app's own port 8124).
# Exit codes: see scripts\nginx-web.ps1 (0 started, 2 missing, 3 nginx -t failed, 4 port busy (owner listed),
# 5 link/junction in the web root or export folder, 6 not listening in time).
param(
    [string]$WebRoot = "",
    [string]$ExeDir = "",
    [string]$ExportDir = "",
    [string]$RuntimeDir = "",
    [string]$Nginx = "",
    [ValidateRange(1, 65535)]
    [int]$Port = 80,
    [ValidateRange(1, 65535)]
    [int]$RestPort = 18080,
    [int]$TimeoutSec = 20,
    [switch]$Test
)
$forward = @{} + $PSBoundParameters
$forward.Remove('Test') | Out-Null
$action = if ($Test) { 'test' } else { 'start' }
& (Join-Path $PSScriptRoot 'nginx-web.ps1') -Action $action @forward
exit $LASTEXITCODE
