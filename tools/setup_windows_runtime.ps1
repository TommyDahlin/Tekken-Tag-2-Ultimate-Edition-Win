param([Parameter(Mandatory=$true)][string]$ToolsRoot)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    python tools/setup_deps.py --windows
    if ($LASTEXITCODE -ne 0) { throw 'Dependency fetch failed' }
    $native=Join-Path $root 'third_party/windows'
    New-Item -ItemType Directory -Force -Path $native | Out-Null
    $archive=Join-Path $native 'SDL3.zip'
    if (!(Test-Path -LiteralPath $archive)) {
        curl.exe -L --fail --silent --show-error -o $archive https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-mingw.zip
        if ($LASTEXITCODE -ne 0) { throw 'SDL download failed' }
    }
    $sdlHash=& python -c "import hashlib,sys;print(hashlib.file_digest(open(sys.argv[1],'rb'),'sha256').hexdigest())" $archive
    if ($LASTEXITCODE -ne 0 -or $sdlHash -ne 'cd98158c8d025a816f430a600b4e9526524e6219ee884ec72bd498b80fd3bf58') { throw 'SDL checksum mismatch' }
    if (!(Test-Path -LiteralPath (Join-Path $native 'SDL3-3.4.18'))) {
        & python -m zipfile -e $archive $native
        if ($LASTEXITCODE -ne 0) { throw 'SDL extraction failed' }
    }
    $compiler=Join-Path $ToolsRoot 'llvm-mingw-20261020-ucrt-x86_64/bin'
    $exportLines=& "$compiler/llvm-readobj.exe" --coff-exports "$env:WINDIR/System32/vulkan-1.dll"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read installed Vulkan loader' }
    $names=@($exportLines | ForEach-Object { if ($_ -match '^  Name: (vk\w+)$') { $Matches[1] } })
    if ($names.Count -lt 100) { throw 'Unexpected Vulkan loader export table' }
    $definition=Join-Path $native 'vulkan-1.def'
    @('LIBRARY vulkan-1.dll','EXPORTS') + $names | Set-Content -LiteralPath $definition -Encoding ascii
    & "$compiler/llvm-dlltool.exe" -m i386:x86-64 -d $definition -l "$native/libvulkan-1.dll.a"
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan import library failed' }
    $cmake=Join-Path $ToolsRoot 'python/cmake/data/bin/cmake.exe'
    $ninja=Join-Path $ToolsRoot 'python/bin/ninja.exe'
    & $cmake -S third_party/shaderc -B build-windows-shaderc -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_EXECUTABLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_WERROR_COMPILE=OFF -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$compiler/clang.exe" "-DCMAKE_CXX_COMPILER=$compiler/clang++.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Shader library configure failed' }
    & $cmake --build build-windows-shaderc --target shaderc_shared -j 12
    if ($LASTEXITCODE -ne 0) { throw 'Shader library build failed' }
} finally { Pop-Location }
