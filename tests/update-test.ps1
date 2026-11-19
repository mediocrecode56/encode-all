# End-to-end self-update test on the host, no window shown: a fake newer build (9.9.9) is served
# from a local HTTP folder; the current build stages it, then failed starts must restore the old exe.
# Run after .\build.ps1:  powershell -NoProfile -File tests\update-test.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tools = if ($env:CODING_TOOLS) { $env:CODING_TOOLS } else { 'E:\CODING\TOOLS' }
if (Test-Path "$tools\winlibs\mingw64\bin") { $env:Path = "$tools\winlibs\mingw64\bin;$env:Path" }
$version = (Select-String -Path "$root\src\version.h" -Pattern '#define EA_VERSION "([^"]+)"').Matches[0].Groups[1].Value
$fails = 0
function Check($ok, $what) { if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what"; $script:fails++ } }

$work = Join-Path $env:TEMP "ea-update-test-$PID"
$install = Join-Path $work 'install'
$serve = Join-Path $work 'serve'
New-Item -ItemType Directory -Force $install, $serve | Out-Null
try {
  Write-Host '[build 9.9.9]'
  $b2 = Join-Path $root 'build-update-test'
  & cmake -S $root -B $b2 -G Ninja -DCMAKE_BUILD_TYPE=Release '-DEA_VERSION_OVERRIDE=9.9.9' | Out-Null
  & cmake --build $b2 --target EncodeAll | Out-Null
  if ($LASTEXITCODE) { & cmake --build $b2 --target EncodeAll | Out-Null }  # one retry: this PC can crash compilers
  Check ((& "$b2\EncodeAll.exe" --version | Out-String).Trim() -eq '9.9.9') 'fake 9.9.9 build reports 9.9.9'
  $stageDir = Join-Path $work 'pkg'
  New-Item -ItemType Directory -Force $stageDir | Out-Null
  Copy-Item "$b2\EncodeAll.exe" $stageDir
  $zip = Join-Path $serve 'EncodeAll-9.9.9-win64.zip'
  Compress-Archive -Path "$stageDir\EncodeAll.exe" -DestinationPath $zip
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
  [IO.File]::WriteAllText("$serve\SHA256SUMS", "$hash  EncodeAll-9.9.9-win64.zip`n")

  Copy-Item "$root\build\EncodeAll.exe" $install
  Set-Content "$install\portable" 'test' -Encoding ascii
  $exe = Join-Path $install 'EncodeAll.exe'
  $port = Get-Random -Minimum 20000 -Maximum 40000
  $py = Start-Process py -ArgumentList '-I', '-m', 'http.server', $port, '--bind', '127.0.0.1', '--directory', $serve -PassThru -WindowStyle Hidden
  Start-Sleep -Seconds 2
  $env:EA_UPDATE_BASE = "http://127.0.0.1:$port/"

  Write-Host '[tampered package is rejected]'
  [IO.File]::WriteAllText("$serve\SHA256SUMS", ('0' * 64) + "  EncodeAll-9.9.9-win64.zip`n")
  & $exe --update-now | Out-Null
  Check ($LASTEXITCODE -eq 2) 'checksum mismatch reported as an error'
  Check ((& $exe --version | Out-String).Trim() -eq $version) "exe unchanged ($version)"
  [IO.File]::WriteAllText("$serve\SHA256SUMS", "$hash  EncodeAll-9.9.9-win64.zip`n")

  Write-Host '[update]'
  $out = (& $exe --update-now | Out-String).Trim()
  Check ($LASTEXITCODE -eq 0) "staged ($out)"
  Check ((& $exe --version | Out-String).Trim() -eq '9.9.9') 'the exe on disk is now 9.9.9'
  Check (Test-Path "$exe.old") 'previous exe kept as EncodeAll.exe.old'
  Check (Test-Path "$install\data\update.json") 'update marker written'
  & $exe --update-now | Out-Null
  Check ($LASTEXITCODE -eq 1) 'a second check finds nothing newer'

  Write-Host '[failed starts roll back]'
  & $exe --startup-check | Out-Null; $c1 = $LASTEXITCODE
  & $exe --startup-check | Out-Null; $c2 = $LASTEXITCODE
  Check ($c1 -eq 0 -and $c2 -eq 0) 'first two unhealthy starts are allowed'
  & $exe --startup-check | Out-Null; $c3 = $LASTEXITCODE
  Check ($c3 -eq 3) 'third unhealthy start restores the previous version'
  Check ((& $exe --version | Out-String).Trim() -eq $version) "exe is $version again"
  Check (Test-Path "$exe.bad") 'failed build kept as EncodeAll.exe.bad for inspection'
  Check (-not (Test-Path "$install\data\update.json")) 'marker cleared'
} finally {
  if ($py) { Stop-Process -Id $py.Id -Force -ErrorAction SilentlyContinue }
  Remove-Item Env:\EA_UPDATE_BASE -ErrorAction SilentlyContinue
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Host ("{0} failed" -f $fails)
exit $fails
