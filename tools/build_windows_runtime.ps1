param([Parameter(Mandatory=$true)][string]$ToolsRoot, [string]$Rpx)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
& "$PSScriptRoot/build_windows_tools.ps1" -ToolsRoot $ToolsRoot
$cmake=Join-Path $ToolsRoot 'python/cmake/data/bin/cmake.exe'
$rpxOptions=@()
if ($Rpx) { $rpxOptions=@("-DTTT2_WINDOWS_TEST_RPX=$Rpx") }
& $cmake -S $root -B "$root/build-windows-tools" -DTTT2_TOOLS_ONLY=ON -DTTT2_WINDOWS_RUNTIME=ON @rpxOptions
if ($LASTEXITCODE -ne 0) { throw 'Runtime configure failed' }
& $cmake --build "$root/build-windows-tools" --target ttt2-ultimate ttt2-play ttt2-launcher windows_checks -j 12
if ($LASTEXITCODE -ne 0) { throw 'Runtime build failed; close the game before rebuilding' }

$compiler=Join-Path $ToolsRoot 'llvm-mingw-20261020-ucrt-x86_64/bin'
$env:PATH="$compiler;"+$env:PATH
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir "$root/build-windows-tools" --output-on-failure -LE launcher
if ($LASTEXITCODE -ne 0) { throw 'Windows regression checks failed' }
