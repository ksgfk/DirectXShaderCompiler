@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Users\xiaoxs\Desktop\DirectXShaderCompiler
cl /std:c++17 /EHsc /nologo /MD /Iinclude utils\radray_wire_probe.cpp /Fo:build_radray\probe. /Fe:build_radray\Release\bin\radray_wire_probe.exe /link build_radray\Release\lib\dxcompiler.lib
