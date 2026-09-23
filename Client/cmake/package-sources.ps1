param([Parameter(Mandatory)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$build = (Resolve-Path $BuildDirectory).Path
$staging = Join-Path $build ('sources-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $staging | Out-Null
try {
    git -C $repo archive --format=zip -o "$staging/PixelMirroring-source.zip" HEAD
    if ($LASTEXITCODE) { throw 'Project source archive failed' }
    git -C "$repo/Client/vcpkg" archive --format=zip -o "$staging/vcpkg-build-recipes.zip" HEAD
    if ($LASTEXITCODE) { throw 'vcpkg source archive failed' }
    $sourceRoots = Get-ChildItem "$repo/Client/vcpkg/buildtrees" -Directory |
        ForEach-Object { Get-ChildItem "$($_.FullName)/src" -Directory -Filter '*.clean' -ErrorAction SilentlyContinue }
    foreach ($required in @('ffmpeg', 'x264', 'sdl2', 'brotli', 'cpp-httplib', 'nlohmann-json')) {
        $sourceMatches = @($sourceRoots | Where-Object { $_.Parent.Parent.Name -eq $required })
        if ($sourceMatches.Count -ne 1) { throw "Expected exactly one source tree for $required; build with an empty binary cache" }
        Copy-Item -LiteralPath $sourceMatches[0].FullName -Destination "$staging/$required" -Recurse
    }
    $manifest = @(
        ('Project: ' + (git -C $repo rev-parse HEAD))
        ('vcpkg: ' + (git -C "$repo/Client/vcpkg" rev-parse HEAD))
        'Configuration: Release, x64-windows, dynamic libraries'
        'Build: cmake --preset default -DCMAKE_BUILD_TYPE=Release; cmake --build build')
    $manifest += Get-Content "$repo/Client/vcpkg.json"
    $manifest | Set-Content "$staging/build-manifest.txt" -Encoding utf8
    $versionLine = Select-String -Path "$repo/Client/CMakeLists.txt" -Pattern 'project\(PixelMirroring VERSION ([0-9.]+)'
    $version = $versionLine.Matches[0].Groups[1].Value
    # libarchive includes hidden source files and avoids Compress-Archive's size limit.
    Push-Location $staging
    try {
        cmake -E tar cf "$build/PixelMirroring-$version-sources.zip" --format=zip .
        if ($LASTEXITCODE) { throw 'Source compression failed' }
    } finally { Pop-Location }
} finally {
    # Only the unique staging directory created above is removed.
    if ([IO.Path]::GetDirectoryName($staging) -ne $build) { throw 'Invalid staging directory' }
    Remove-Item -LiteralPath $staging -Recurse -Force
}
