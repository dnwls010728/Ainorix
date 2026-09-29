@echo off
rem Regenerates engine\render\shaders\Shaders.glsl.h from Shaders.glsl.
rem Needs sokol-shdc.exe (https://github.com/floooh/sokol-tools-bin, bin\win32\):
rem put it on PATH or set SOKOL_SHDC=C:\path\to\sokol-shdc.exe
setlocal
if "%SOKOL_SHDC%"=="" set SOKOL_SHDC=sokol-shdc
pushd "%~dp0..\..\engine\render\shaders"
"%SOKOL_SHDC%" --input Shaders.glsl --output Shaders.glsl.h --slang hlsl5:glsl300es:glsl430 --no-log-cmdline || (popd & exit /b 1)
popd
echo wrote engine\render\shaders\Shaders.glsl.h
