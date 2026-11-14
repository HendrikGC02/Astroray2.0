# Builds native\sbnative.dll (MSVC cl.exe, direct command; no CMake needed for two files).
. "$PSScriptRoot\env.ps1"
New-Item -ItemType Directory -Force "$PSScriptRoot\build" | Out-Null
Push-Location "$PSScriptRoot\build"
$cudaInc = "$env:CUDA_PATH\include"; $cudaLib = "$env:CUDA_PATH\lib\x64"
cl /nologo /O2 /EHsc /std:c++17 /LD /MD /I"$PSScriptRoot\native" /I"$cudaInc" /I"$OPTIX\include" `
   "$PSScriptRoot\native\sb.cpp" "$PSScriptRoot\native\gas.cpp" /Fe:sbnative.dll `
   /link /LIBPATH:"$cudaLib" cudart_static.lib
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { throw "build failed $rc" }
Get-Item "$PSScriptRoot\build\sbnative.dll" | Select-Object FullName, Length, LastWriteTime
