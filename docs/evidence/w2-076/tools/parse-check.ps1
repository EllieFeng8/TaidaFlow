# w2-076: PowerShell syntax check (the PowerShell parser, nothing is executed) of every .ps1 changed or added by
# w2-076, plus: ASCII only (Windows PowerShell 5.1 reads a .ps1 without BOM in the ANSI code page).
# Exit 0 = all files parse without error and are ASCII; 1 otherwise.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$files = @(
    'deploy\release\start-taidaflow.ps1', 'deploy\release\stop-taidaflow.ps1', 'deploy\release\register-autostart.ps1',
    'scripts\taidaflow-config.ps1', 'scripts\install-nginx-service.ps1', 'scripts\uninstall-nginx-service.ps1',
    'scripts\add-startup-shortcut.ps1', 'scripts\remove-startup-shortcut.ps1', 'scripts\package-release.ps1',
    'scripts\verify-release-package.ps1', 'scripts\check-package-deps.ps1',
    'docs\evidence\w2-076\tools\run-step.ps1', 'docs\evidence\w2-076\tools\parse-check.ps1',
    'docs\evidence\w2-076\tools\test-nginx-service-logic.ps1', 'docs\evidence\w2-076\tools\test-startup-shortcut.ps1',
    'docs\evidence\w2-076\tools\test-service-scripts-nonadmin.ps1', 'docs\evidence\w2-076\tools\check-winsw-xml.ps1',
    'docs\evidence\w2-076\tools\make-zip.ps1', 'docs\evidence\w2-076\tools\check-manifest.ps1',
    'docs\evidence\w2-076\tools\show-winsw-tools.ps1', 'docs\evidence\w2-076\tools\retire-dist.ps1', 'docs\evidence\w2-076\tools\show-package-winsw.ps1',
    'docs\evidence\w2-076\tools\final-state.ps1')
$bad = 0
foreach ($rel in $files) {
    $p = Join-Path $root $rel
    if (-not (Test-Path -LiteralPath $p)) { [Console]::Out.WriteLine("MISSING  $rel"); $bad++; continue }
    $tokens = $null; $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$tokens, [ref]$errors)
    $bytes = [System.IO.File]::ReadAllBytes($p)
    $nonAscii = @($bytes | Where-Object { $_ -gt 127 }).Count
    if ($errors.Count -or $nonAscii) {
        $bad++
        [Console]::Out.WriteLine("FAIL     $rel : $($errors.Count) parse error(s), $nonAscii non-ASCII byte(s)")
        foreach ($e in $errors) { [Console]::Out.WriteLine("           line $($e.Extent.StartLineNumber): $($e.Message)") }
    } else {
        [Console]::Out.WriteLine(("ok       {0} ({1} tokens, {2} bytes, ASCII)" -f $rel, $tokens.Count, $bytes.Length))
    }
}
[Console]::Out.WriteLine("checked $($files.Count) file(s), $bad with problems")
if ($bad) { exit 1 }
exit 0
