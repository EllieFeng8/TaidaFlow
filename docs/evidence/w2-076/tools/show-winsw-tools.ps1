# w2-076 D1 evidence: the WinSW files on the development PC (C:\tools\winsw, outside the repository).
# Prints every file with size and SHA-256, the Authenticode status and version of the exe, SOURCE.txt, and that
# nothing of WinSW is tracked or pending in git.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$dir = 'C:\tools\winsw'
foreach ($f in @(Get-ChildItem -LiteralPath $dir -File | Sort-Object Name)) {
    [Console]::Out.WriteLine(("{0,-26} {1,10} bytes  SHA-256 {2}" -f $f.Name, $f.Length, (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash))
}
$exe = Join-Path $dir 'WinSW-x64-2.12.0.exe'
$sig = Get-AuthenticodeSignature -LiteralPath $exe
$vi = (Get-Item -LiteralPath $exe).VersionInfo
[Console]::Out.WriteLine("Authenticode : $($sig.Status) ($($sig.StatusMessage))")
[Console]::Out.WriteLine("version info : ProductName '$($vi.ProductName)', FileVersion $($vi.FileVersion), ProductVersion $($vi.ProductVersion), CompanyName '$($vi.CompanyName)', LegalCopyright '$($vi.LegalCopyright)'")
[Console]::Out.WriteLine("expected SHA-256 (SOURCE.txt): 05B82D46AD331CC16BDC00DE5C6332C1EF818DF8CEEFCD49C726553209B3A0DA -> $(if ((Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash -eq '05B82D46AD331CC16BDC00DE5C6332C1EF818DF8CEEFCD49C726553209B3A0DA') { 'MATCH' } else { 'DIFFERENT' })")
[Console]::Out.WriteLine("--- $dir\SOURCE.txt")
Get-Content -LiteralPath (Join-Path $dir 'SOURCE.txt') | ForEach-Object { [Console]::Out.WriteLine("  $_") }
[Console]::Out.WriteLine("--- $dir\LICENSE.txt (first 3 lines)")
Get-Content -LiteralPath (Join-Path $dir 'LICENSE.txt') -TotalCount 3 | ForEach-Object { [Console]::Out.WriteLine("  $_") }
$tracked = @(& git -C $root ls-files | Where-Object { $_ -match '(?i)winsw|nginx-service\.(exe|xml)' })
$pending = @(& git -C $root status --porcelain --untracked-files=all | Where-Object { $_ -match '(?i)winsw.*\.exe|nginx-service\.(exe|xml)' })
[Console]::Out.WriteLine("git: files named *winsw* / nginx-service.exe|xml tracked: $($tracked.Count); pending (status): $($pending.Count)  (C:\tools\winsw is outside the repository)")
$tracked + $pending | ForEach-Object { [Console]::Out.WriteLine("  $_") }
if ($tracked.Count -or $pending.Count) { exit 1 }
exit 0
