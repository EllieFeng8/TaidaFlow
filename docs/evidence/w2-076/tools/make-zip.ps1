# w2-076 copy of docs\evidence\w2-074\tools\make-zip.ps1.
# w2-074 D6: zip a package folder next to it with Compress-Archive (the folder itself is the root entry
# of the zip) and print the SHA-256 of the zip. Refuses to overwrite an existing zip.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File make-zip.ps1 -Package <dist\TaidaFlow-...>
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
$zip = $Package + '.zip'
if (Test-Path -LiteralPath $zip) { [Console]::Out.WriteLine("exists: $zip (delete it first)"); exit 4 }
[Console]::Out.WriteLine("Compress-Archive (Microsoft.PowerShell.Archive $((Get-Module -ListAvailable Microsoft.PowerShell.Archive | Select-Object -First 1).Version)) $Package -> $zip")
Compress-Archive -Path $Package -DestinationPath $zip -CompressionLevel Optimal
$z = Get-Item -LiteralPath $zip
[Console]::Out.WriteLine(("zip     : {0}" -f $z.FullName))
[Console]::Out.WriteLine(("bytes   : {0} ({1:N1} MB)" -f $z.Length, ($z.Length / 1MB)))
[Console]::Out.WriteLine(("SHA-256 : {0}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash))
exit 0
