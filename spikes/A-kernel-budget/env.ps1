# Imports the MSVC x64 environment (BuildTools 17.14) and puts CUDA 12.8 first on PATH. Idempotent
# (re-sourcing used to grow PATH until commands vanished).
if (-not $env:KL_ENV_DONE) {
  $bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
  $cuda = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8'
  cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
  $env:CUDA_PATH = $cuda
  $env:PATH = "$cuda\bin;$env:PATH"
  $env:KL_ENV_DONE = '1'
}
$global:OPTIX = 'C:\ProgramData\NVIDIA Corporation\OptiX SDK 9.1.0'
