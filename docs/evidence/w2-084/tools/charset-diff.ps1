# w2-084 D0: characters added / removed between two charset.txt files (UTF-8), plus the sizes of
# the font files next to them.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-084\tools\charset-diff.ps1
#            -Before build\w2-084-font-before -After App\fonts [-OutFile docs\evidence\w2-084\d0-charset-diff.txt]
param(
    [Parameter(Mandatory = $true)][string]$Before,
    [Parameter(Mandatory = $true)][string]$After,
    [string]$OutFile = ''
)
$ErrorActionPreference = 'Stop'
function CodePoints([string]$path) {
    $text = [System.IO.File]::ReadAllText((Resolve-Path $path).Path, [System.Text.Encoding]::UTF8)
    $set = New-Object 'System.Collections.Generic.SortedSet[int]'
    for ($i = 0; $i -lt $text.Length; $i++) {
        $cp = [char]::ConvertToUtf32($text, $i)
        if ($cp -gt 0xFFFF) { $i++ }
        if ($cp -ne 10 -and $cp -ne 13) { [void]$set.Add($cp) }
    }
    return ,$set
}
$b = CodePoints (Join-Path $Before 'charset.txt')
$a = CodePoints (Join-Path $After 'charset.txt')
$added = @($a | Where-Object { -not $b.Contains($_) })
$removed = @($b | Where-Object { -not $a.Contains($_) })
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("charset before=$($b.Count) after=$($a.Count)")
$lines.Add("added($($added.Count))=" + (($added | ForEach-Object { [char]::ConvertFromUtf32($_) }) -join ''))
$lines.Add("added code points: " + (($added | ForEach-Object { 'U+{0:X4}' -f $_ }) -join ' '))
$lines.Add("removed($($removed.Count))=" + (($removed | ForEach-Object { [char]::ConvertFromUtf32($_) }) -join ''))
foreach ($name in @('charset.txt', 'TaidaFlowNotoSansTC-Regular.ttf', 'TaidaFlowNotoSansTC-Bold.ttf')) {
    $sb = (Get-Item (Join-Path $Before $name)).Length
    $sa = (Get-Item (Join-Path $After $name)).Length
    $lines.Add(("{0}: {1} -> {2} bytes ({3:+#;-#;0})" -f $name, $sb, $sa, ($sa - $sb)))
}
if ($OutFile) { [System.IO.File]::WriteAllLines($OutFile, $lines, (New-Object System.Text.UTF8Encoding($false))) }
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$lines | ForEach-Object { [Console]::Out.WriteLine($_) }
exit 0
