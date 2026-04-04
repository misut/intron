$ErrorActionPreference = "Stop"

$Repo = "misut/intron"
$InstallDir = "$env:USERPROFILE\.intron\bin"
$Platform = "x86_64-pc-windows-msvc"

Write-Host "fetching latest release..."
$Release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest"
$Tag = $Release.tag_name

if (-not $Tag) {
    Write-Error "failed to fetch latest release"
    exit 1
}

Write-Host "installing intron $Tag for $Platform..."

$Url = "https://github.com/$Repo/releases/download/$Tag/intron-$Tag-$Platform.zip"
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "intron-install"
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
$Archive = Join-Path $TmpDir "intron.zip"

Invoke-WebRequest -Uri $Url -OutFile $Archive
Expand-Archive -Path $Archive -DestinationPath $TmpDir -Force

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Path (Join-Path $TmpDir "intron.exe") -Destination (Join-Path $InstallDir "intron.exe") -Force
Remove-Item -Recurse -Force $TmpDir

Write-Host "installed intron $Tag to $InstallDir\intron.exe"
Write-Host ""

# PATH check
if ($env:PATH -notlike "*$InstallDir*") {
    Write-Host "add this to your PowerShell profile:"
    Write-Host "  `$env:PATH = `"$InstallDir;`$env:PATH`""
    Write-Host ""
    Write-Host "or add to system PATH permanently:"
    Write-Host "  [Environment]::SetEnvironmentVariable('PATH', `"$InstallDir;`" + [Environment]::GetEnvironmentVariable('PATH', 'User'), 'User')"
}
