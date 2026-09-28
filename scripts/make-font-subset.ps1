# Runs scripts\make_font_subset.py (the embedded CJK font subset of the web page) with Python 3.
# The ONLY Python tool of this project (w2-062); a normal desktop / WebAssembly build and every
# deployment need no Python: App\fonts\TaidaFlowNotoSansTC-*.ttf and charset.txt are committed.
# Only needed to REGENERATE the subset after the UI texts changed (docs\BUILD.md).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make-font-subset.ps1 [--check] [--source <font>]
#   (the arguments are passed to make_font_subset.py unchanged; --check only verifies)
# Python: the Python launcher "py -3" when present, otherwise "python" from PATH (the Microsoft
# Store alias in ...\WindowsApps is skipped). Requires fontTools (python -m pip install fonttools).
# Exit code: the one of make_font_subset.py; 9 = no usable Python 3 found.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'make_font_subset.py'
function Say([string]$m) { [Console]::Out.WriteLine($m) }

$exe = $null; $prefix = @()
$py = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($py) {
    & $py.Source -3 -c "import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)" 2>$null
    if ($LASTEXITCODE -eq 0) { $exe = $py.Source; $prefix = @('-3') }
}
if (-not $exe) {
    foreach ($c in @(Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue)) {
        if ($c.Source -match '\\WindowsApps\\') { continue }     # Microsoft Store alias, not a Python
        & $c.Source -c "import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)" 2>$null
        if ($LASTEXITCODE -eq 0) { $exe = $c.Source; break }
    }
}
if (-not $exe) {
    Say "make-font-subset: no Python 3 found (neither 'py -3' nor a python.exe on PATH outside WindowsApps)."
    Say "  Install Python 3 from https://www.python.org/downloads/windows/ (tick 'Add python.exe to PATH'),"
    Say "  then: python -m pip install fonttools   (docs\BUILD.md, section on the embedded font subset)."
    Say "  Not needed for building or deploying: the font files in App\fonts are already committed."
    exit 9
}
Say ("make-font-subset: {0} {1}-B {2} {3}" -f $exe, $(if ($prefix) { ($prefix -join ' ') + ' ' } else { '' }), $script, ($args -join ' '))
& $exe @prefix -B $script @args
exit $LASTEXITCODE
