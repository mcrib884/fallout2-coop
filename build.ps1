# Fallout 2 Coop Release Build & Deploy Script
$ErrorActionPreference = "Stop"

Write-Host "Building Fallout 2 CE (Release)..." -ForegroundColor Cyan
cmake --preset windows-x64
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed."
    exit $LASTEXITCODE
}

cmake --build --preset windows-x64-release
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake build failed."
    exit $LASTEXITCODE
}

# Locate built executable
$ExePath = Get-ChildItem -Path "out" -Filter "*fallout2*.exe" -Recurse | Where-Object { $_.DirectoryName -like "*RelWithDebInfo*" -or $_.DirectoryName -like "*Release*" } | Select-Object -First 1 -ExpandProperty FullName

if (-not $ExePath) {
    $ExePath = Get-ChildItem -Path "out" -Filter "*fallout2*.exe" -Recurse | Select-Object -First 1 -ExpandProperty FullName
}

if (-not $ExePath) {
    $ExePath = Get-ChildItem -Path "build" -Filter "*fallout2*.exe" -Recurse | Select-Object -First 1 -ExpandProperty FullName
}

if ($ExePath) {
    $DistDir = Join-Path -Path $PSScriptRoot -ChildPath "fallout2coopdist"
    if (-not (Test-Path $DistDir)) {
        New-Item -ItemType Directory -Path $DistDir | Out-Null
    }
    
    Write-Host "Copying built executable ($ExePath) to $DistDir..." -ForegroundColor Green
    Copy-Item -Path $ExePath -Destination $DistDir -Force
    Write-Host "Build and deploy completed successfully!" -ForegroundColor Green
} else {
    Write-Error "Could not locate built fallout2 executable."
}
