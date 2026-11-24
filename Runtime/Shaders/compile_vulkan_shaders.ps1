# Compiles the Vulkan shaders in this folder to SPIR-V and writes them into Runtime/Source/Renderer/VulkanShaders.h,
# which the build uses as it is (no shader compiler is needed to build KeeperForge2). Needs glslangValidator from the
# Vulkan SDK (on the PATH, or found through the VULKAN_SDK environment variable). Run it after changing a shader.
# smaa.frag is built once per pass (its preset comes in the push constants).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$output = Join-Path $here '..\Source\Renderer\VulkanShaders.h'

$compiler = (Get-Command glslangValidator -ErrorAction SilentlyContinue).Source
if (-not $compiler -and $env:VULKAN_SDK) { $compiler = Join-Path $env:VULKAN_SDK 'Bin\glslangValidator.exe' }
if (-not $compiler -or -not (Test-Path $compiler)) { throw 'glslangValidator not found: install the Vulkan SDK or put it on the PATH.' }

function Compile([string]$name, [string]$source, [string[]]$defines = @()) {
	$path = Join-Path $here $source
	$spirv = Join-Path ([IO.Path]::GetTempPath()) ("KeeperForge2-$name.spv")
	$arguments = @('-V') + ($defines | ForEach-Object { "-D$_" }) + @($path, '-o', $spirv)
	& $compiler @arguments | Out-Host
	if ($LASTEXITCODE -ne 0) { throw "Compiling $source ($name) failed." }
	$bytes = [IO.File]::ReadAllBytes($spirv)
	Remove-Item $spirv
	$words = for ($i = 0; $i -lt $bytes.Length; $i += 4) { '0x{0:x8}' -f [BitConverter]::ToUInt32($bytes, $i) }
	$lines = for ($i = 0; $i -lt $words.Count; $i += 8) { "`t`t" + (($words[$i..([Math]::Min($i + 7, $words.Count - 1))]) -join ', ') + ',' }
	return "`tinline constexpr std::uint32_t $name[] = {`n" + ($lines -join "`n") + "`n`t};"
}

$arrays = @(
	(Compile 'DrawVertex' 'draw.vert'),
	(Compile 'DrawFragment' 'draw.frag'),
	(Compile 'PostVertex' 'post.vert'),
	(Compile 'CopyFragment' 'copy.frag'),
	(Compile 'FxaaFragment' 'fxaa.frag'),
	(Compile 'SmaaEdgeFragment' 'smaa.frag' @('SMAA_PASS_EDGE')),
	(Compile 'SmaaWeightFragment' 'smaa.frag' @('SMAA_PASS_WEIGHT')),
	(Compile 'SmaaBlendFragment' 'smaa.frag' @('SMAA_PASS_BLEND')),
	(Compile 'RayAoCompute' 'ray_ao.comp'),
	(Compile 'RayBlurCompute' 'ray_blur.comp')
)
$body = $arrays -join "`n"
$header = @"
#pragma once
#include <cstdint>

// SPIR-V of the shaders in Runtime/Shaders, written by Runtime/Shaders/compile_vulkan_shaders.ps1.
// Do not edit by hand: change the shaders and run the script again.
namespace KeeperForge2::Render::VulkanShaders {
$body
}
"@
[IO.File]::WriteAllText((Resolve-Path (Split-Path $output)).Path + '\VulkanShaders.h', $header.Replace("`r`n", "`n") + "`n")
Write-Host "Wrote $output"
