# Builds build\EncodeAll.exe and build\unit_tests.exe (static, no DLLs), and copies the exe to the project root.
# -Release also stages build\stage (portable, for WinTest) and writes dist\ (installer, zip, SHA256SUMS).
param([switch]$Release, [switch]$Clean, [switch]$Test)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$tools = if ($env:CODING_TOOLS) { $env:CODING_TOOLS } elseif (Test-Path 'E:\CODING\TOOLS') { 'E:\CODING\TOOLS' } else { '' }
if ($tools -and (Test-Path "$tools\winlibs\mingw64\bin")) { $env:Path = "$tools\winlibs\mingw64\bin;$env:Path" }
foreach ($t in 'g++', 'cmake', 'ninja', 'windres') {
  if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t not found (WinLibs in TOOLS or MSYS2 UCRT64 on PATH)" }
}
$version = (Select-String -Path "$root\src\version.h" -Pattern '#define EA_VERSION "([^"]+)"').Matches[0].Groups[1].Value
$build = Join-Path $root 'build'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
& cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release | Out-Null
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& cmake --build $build
if ($LASTEXITCODE) { throw 'Build failed' }
Write-Host "built EncodeAll $version"
# The latest build also goes to the project root (EncodeAll.exe, git-ignored) for the owner to run.
# Not portable there: it uses %LOCALAPPDATA%\Encode All. A running copy is left alone.
try { Copy-Item "$build\EncodeAll.exe" "$root\EncodeAll.exe" -Force; Write-Host "copied to $root\EncodeAll.exe" }
catch { Write-Warning "EncodeAll.exe in the project root is in use; close it and build again to update it" }
if ($Test -or $Release) {
  & "$build\unit_tests.exe"
  if ($LASTEXITCODE) { throw 'Unit tests failed' }
}
if ($Release) {
  $stage = Join-Path $build 'stage'
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  New-Item -ItemType Directory -Force $stage | Out-Null
  Copy-Item "$build\EncodeAll.exe", "$root\LICENSE", "$root\THIRD_PARTY.md" $stage
  Set-Content -Path "$stage\portable" -Value 'Data lives in data\ beside the exe.' -Encoding ascii
  $dist = Join-Path $root 'dist'
  New-Item -ItemType Directory -Force $dist | Out-Null
  Get-ChildItem $dist -File | Remove-Item -Force
  $zip = Join-Path $dist "EncodeAll-$version-win64.zip"
  Compress-Archive -Path "$build\EncodeAll.exe", "$root\LICENSE", "$root\THIRD_PARTY.md" -DestinationPath $zip
  $iscc = if ($env:ISCC) { $env:ISCC } elseif ($tools -and (Test-Path "$tools\InnoSetup\ISCC.exe")) { "$tools\InnoSetup\ISCC.exe" } else { 'ISCC.exe' }
  & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$build" "/DRootDir=$root" "/O$dist" "$root\installer\EncodeAll.iss"
  if ($LASTEXITCODE) { throw 'Installer build failed' }
  $lines = Get-ChildItem $dist -File | Where-Object { $_.Name -ne 'SHA256SUMS' } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
  }
  [IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS'), (($lines -join "`n") + "`n"))
  Get-ChildItem $dist | Format-Table Name, Length -AutoSize
}
