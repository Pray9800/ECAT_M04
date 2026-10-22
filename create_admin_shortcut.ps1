# Create VS2026 admin shortcut (English file name to avoid encoding issue)
$desktop = [Environment]::GetFolderPath('Desktop')
Write-Host "Desktop: $desktop"

$WshShell = New-Object -ComObject WScript.Shell
$lnkPath = Join-Path $desktop 'Visual Studio 2026 - Admin.lnk'

$Shortcut = $WshShell.CreateShortcut($lnkPath)
$Shortcut.TargetPath = 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\devenv.exe'
$Shortcut.WorkingDirectory = 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE'
$Shortcut.IconLocation = 'D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\devenv.exe,0'
$Shortcut.Save()

# Set lnk Flags byte(0x15) + 0x20 = RunAsUser (run as administrator)
$bytes = [System.IO.File]::ReadAllBytes($lnkPath)
$bytes[0x15] = $bytes[0x15] -bor 0x20
[System.IO.File]::WriteAllBytes($lnkPath, $bytes)

Write-Host "Shortcut created: $lnkPath"
Write-Host "Done."
