param([Parameter(Mandatory)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path $BuildDirectory).Path
$work = Join-Path $build ('verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
try {
    cmake --install $build --config Release --prefix "$work/reference"
    if ($LASTEXITCODE) { throw 'Reference install failed' }
    $required = @('PixelMirroring.exe', 'PixelMirroring.apk', 'FossifyLauncher.apk',
        'scrcpy-server.jar', 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll')
    $required += @('licenses/GPL-3.0.txt', 'licenses/FossifyLauncher.md',
        'licenses/ThirdParty.md', 'licenses/Android-Platform-Tools-NOTICE.txt',
        'licenses/Pixel-Mirroring-LICENSE.txt')
    foreach ($dependency in @('ffmpeg', 'x264', 'sdl2', 'brotli', 'cpp-httplib', 'nlohmann-json')) {
        $required += "licenses/vcpkg/$dependency/copyright"
    }
    $required += Get-ChildItem "$work/reference/licenses" -File -Recurse |
        ForEach-Object { [IO.Path]::GetRelativePath("$work/reference", $_.FullName) }
    $required = @($required | ForEach-Object { $_.Replace('\', '/') } | Select-Object -Unique)
    foreach ($packageType in @('zip', 'exe', 'msi')) {
        $packages = @(Get-ChildItem "$build/PixelMirroring-*.$packageType" |
            Where-Object { $_.Name -notlike '*-sources.zip' })
        if ($packages.Count -ne 1) { throw "Expected one $packageType package, found $($packages.Count)" }
        $destination = Join-Path $work $packageType
        New-Item -ItemType Directory $destination | Out-Null
        if ($packageType -eq 'zip') {
            Expand-Archive -LiteralPath $packages[0].FullName -DestinationPath $destination
        } elseif ($packageType -eq 'exe') {
            & 7z x $packages[0].FullName "-o$destination" -y | Out-Null
            if ($LASTEXITCODE) { throw 'NSIS extraction failed' }
        } else {
            $log = Join-Path $build 'verify-msi.log'
            $process = Start-Process msiexec.exe -ArgumentList @('/a', ('"' + $packages[0].FullName + '"'),
                '/qn', ('TARGETDIR="' + $destination + '"'), '/l*v', ('"' + $log + '"')) -Wait -PassThru -WindowStyle Hidden
            if ($process.ExitCode -notin @(0, 3010)) { throw "MSI extraction failed: $($process.ExitCode); see $log" }
        }
        $files = @(Get-ChildItem $destination -File -Recurse)
        foreach ($relative in $required) {
            $reference = Join-Path "$work/reference" $relative
            if (!(Test-Path -LiteralPath $reference)) { throw "Reference is missing $relative" }
            $suffix = '/' + $relative.Replace('\', '/')
            $found = @($files | Where-Object { $_.FullName.Replace('\', '/').EndsWith($suffix) })
            if (!$found.Count) { throw "$packageType is missing $relative" }
            $hash = (Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash
            if (!($found | Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $hash })) {
                throw "$packageType has incorrect contents for $relative"
            }
        }
        Write-Host "$packageType verified: $($required.Count) payload and licence files"
    }
} finally {
    if ([IO.Path]::GetDirectoryName($work) -ne $build) { throw 'Invalid verification directory' }
    Remove-Item -LiteralPath $work -Recurse -Force
}
