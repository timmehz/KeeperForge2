@echo off
rem Runs compile_vulkan_shaders.ps1 (see there). Needs the Vulkan SDK's glslangValidator.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0compile_vulkan_shaders.ps1"
if errorlevel 1 pause
