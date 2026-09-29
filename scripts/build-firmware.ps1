[CmdletBinding()]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Python {
  param([Parameter(Mandatory = $true)][string[]]$Arguments)

  & $script:PythonCommand @script:PythonPrefix @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Python command failed with exit code $LASTEXITCODE."
  }
}

function Write-Utf8File {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Content
  )

  $encoding = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Get-WebContentType {
  param([Parameter(Mandatory = $true)][string]$Extension)

  switch ($Extension.ToLowerInvariant()) {
    '.html' { return 'text/html; charset=utf-8' }
    '.css'  { return 'text/css; charset=utf-8' }
    '.js'   { return 'text/javascript; charset=utf-8' }
    '.json' { return 'application/json' }
    '.svg'  { return 'image/svg+xml' }
    '.png'  { return 'image/png' }
    '.jpg'  { return 'image/jpeg' }
    '.jpeg' { return 'image/jpeg' }
    '.ico'  { return 'image/x-icon' }
    '.webp' { return 'image/webp' }
    '.woff2' { return 'font/woff2' }
    default { return 'application/octet-stream' }
  }
}

function Write-EmbeddedWebHeader {
  param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$HeaderPath
  )

  $files = @(Get-ChildItem -LiteralPath $SourceDirectory -File -Recurse |
      Sort-Object FullName)
  $entryPath = Join-Path $SourceDirectory 'device.html'
  if (-not (Test-Path -LiteralPath $entryPath)) {
    $entryPath = Join-Path $SourceDirectory 'index.html'
  }
  if ($files.Count -eq 0 -or -not (Test-Path -LiteralPath $entryPath)) {
    throw 'The device web build did not produce device.html (or index.html).'
  }

  $builder = New-Object System.Text.StringBuilder
  [void]$builder.AppendLine('#pragma once')
  [void]$builder.AppendLine('')
  [void]$builder.AppendLine('#include <stddef.h>')
  [void]$builder.AppendLine('#include <stdint.h>')
  [void]$builder.AppendLine('')
  [void]$builder.AppendLine('namespace cyberclip {')
  [void]$builder.AppendLine('struct EmbeddedWebFile {')
  [void]$builder.AppendLine('  const char *path;')
  [void]$builder.AppendLine('  const char *contentType;')
  [void]$builder.AppendLine('  const uint8_t *data;')
  [void]$builder.AppendLine('  size_t size;')
  [void]$builder.AppendLine('};')
  [void]$builder.AppendLine('')

  for ($fileIndex = 0; $fileIndex -lt $files.Count; $fileIndex++) {
    $inputBytes = [System.IO.File]::ReadAllBytes($files[$fileIndex].FullName)
    $memory = New-Object System.IO.MemoryStream
    $gzip = New-Object System.IO.Compression.GZipStream(
      $memory, [System.IO.Compression.CompressionMode]::Compress, $true)
    try {
      $gzip.Write($inputBytes, 0, $inputBytes.Length)
    } finally {
      $gzip.Dispose()
    }
    $compressed = $memory.ToArray()
    $memory.Dispose()

    [void]$builder.AppendLine("static const uint8_t kDeviceWebFile$fileIndex[] = {")
    for ($offset = 0; $offset -lt $compressed.Length; $offset += 12) {
      $last = [Math]::Min($offset + 11, $compressed.Length - 1)
      $values = for ($byteIndex = $offset; $byteIndex -le $last; $byteIndex++) {
        '0x{0:X2}' -f $compressed[$byteIndex]
      }
      [void]$builder.AppendLine('    ' + ($values -join ', ') + ',')
    }
    [void]$builder.AppendLine('};')
    [void]$builder.AppendLine('')
  }

  [void]$builder.AppendLine('static const EmbeddedWebFile kDeviceWebFiles[] = {')
  for ($fileIndex = 0; $fileIndex -lt $files.Count; $fileIndex++) {
    $relativePath = $files[$fileIndex].FullName.Substring(
      $SourceDirectory.Length).TrimStart('\').Replace('\', '/')
    if ($files[$fileIndex].FullName -eq $entryPath) {
      $relativePath = 'index.html'
    }
    $contentType = Get-WebContentType $files[$fileIndex].Extension
    [void]$builder.AppendLine(
      "    {`"/$relativePath`", `"$contentType`", kDeviceWebFile$fileIndex, sizeof(kDeviceWebFile$fileIndex)},")
  }
  [void]$builder.AppendLine('};')
  [void]$builder.AppendLine('constexpr size_t kDeviceWebFileCount =')
  [void]$builder.AppendLine('    sizeof(kDeviceWebFiles) / sizeof(kDeviceWebFiles[0]);')
  [void]$builder.AppendLine('}  // namespace cyberclip')
  Write-Utf8File $HeaderPath $builder.ToString()
}

function Replace-SingleMatch {
  param(
    [Parameter(Mandatory = $true)][string]$Content,
    [Parameter(Mandatory = $true)][string]$Pattern,
    [Parameter(Mandatory = $true)][string]$Replacement,
    [Parameter(Mandatory = $true)][string]$Description
  )

  $regex = New-Object System.Text.RegularExpressions.Regex($Pattern)
  if ($regex.Matches($Content).Count -ne 1) {
    throw "Expected exactly one $Description."
  }
  return $regex.Replace($Content, $Replacement)
}

function Assert-EmbeddedFile {
  param(
    [Parameter(Mandatory = $true)][string]$ImagePath,
    [Parameter(Mandatory = $true)][string]$ComponentPath,
    [Parameter(Mandatory = $true)][long]$Offset
  )

  $image = [System.IO.File]::OpenRead($ImagePath)
  $component = [System.IO.File]::OpenRead($ComponentPath)
  try {
    if ($image.Length -lt $Offset + $component.Length) {
      throw "Merged image is too small to contain $ComponentPath at offset 0x$($Offset.ToString('x'))."
    }

    $null = $image.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    $imageBuffer = New-Object byte[] 8192
    $componentBuffer = New-Object byte[] 8192
    while (($componentRead = $component.Read($componentBuffer, 0, $componentBuffer.Length)) -gt 0) {
      $imageRead = $image.Read($imageBuffer, 0, $componentRead)
      if ($imageRead -ne $componentRead) {
        throw "Merged image ended while checking $ComponentPath."
      }
      for ($index = 0; $index -lt $componentRead; $index++) {
        if ($imageBuffer[$index] -ne $componentBuffer[$index]) {
          throw "Merged image does not contain $ComponentPath at offset 0x$($Offset.ToString('x'))."
        }
      }
    }
  } finally {
    $component.Dispose()
    $image.Dispose()
  }
}

if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
  throw "Version must use major.minor.patch format, for example 2.0.6."
}

$versionParts = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
if (($versionParts | Measure-Object -Maximum).Maximum -gt 255) {
  throw "Each firmware version component must fit in a uint8_t (0-255)."
}

$python = Get-Command python -ErrorAction SilentlyContinue
if ($null -ne $python) {
  $script:PythonCommand = $python.Source
  $script:PythonPrefix = @()
} else {
  $pythonLauncher = Get-Command py -ErrorAction SilentlyContinue
  if ($null -eq $pythonLauncher) {
    throw "Python 3 is required. Install it from https://www.python.org/downloads/windows/."
  }
  $script:PythonCommand = $pythonLauncher.Source
  $script:PythonPrefix = @('-3')
}

& $script:PythonCommand @script:PythonPrefix -m platformio --version *> $null
if ($LASTEXITCODE -ne 0) {
  Write-Host 'PlatformIO Core was not found; installing it with pip...'
  Invoke-Python @('-m', 'pip', 'install', '--user', 'platformio')
}

$platformInfoJson = & $script:PythonCommand @script:PythonPrefix -m platformio system info --json-output
if ($LASTEXITCODE -ne 0) {
  throw "Unable to read PlatformIO system information."
}
$platformInfo = $platformInfoJson | ConvertFrom-Json
$platformCoreDirectory = $platformInfo.core_dir.value

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$firmwareDirectory = Join-Path $repositoryRoot 'firmware'
$firmwareSourcePath = Join-Path $firmwareDirectory 'matrix_display.ino'
$deviceBuildDirectory = Join-Path $repositoryRoot 'dist-device'
$deviceWebHeaderPath = Join-Path $firmwareDirectory 'generated\device_web.h'
$manifestPath = Join-Path $repositoryRoot 'public\firmware\manifest.json'
$serviceWorkerPath = Join-Path $repositoryRoot 'public\service-worker.js'
$buildRoot = Join-Path $firmwareDirectory '.pio\build'
$bootAppPath = Join-Path $platformCoreDirectory 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
$esptoolPath = Join-Path $platformCoreDirectory 'packages\tool-esptoolpy\esptool.py'

# The first target is also published through the legacy top-level manifest
# fields so older web app builds keep flashing the original ESP32 board.
$firmwareTargets = @(
  [pscustomobject]@{
    Environment = 'ideaspark_esp32'
    Chip = 'ESP32'
    EsptoolChip = 'esp32'
    Board = 'ideaspark ESP32 ST7789'
    FileStem = 'cyberclip'
    BootloaderOffset = 0x1000
    FlashMode = 'dio'
    FlashFreq = '40m'
  },
  [pscustomobject]@{
    Environment = 'lilygo_t_display_s3'
    Chip = 'ESP32-S3'
    EsptoolChip = 'esp32s3'
    Board = 'LilyGO T-Display-S3'
    FileStem = 'cyberclip-lilygo-t-display-s3'
    BootloaderOffset = 0x0
    FlashMode = 'keep'
    FlashFreq = 'keep'
  }
)

foreach ($requiredPath in @($firmwareSourcePath, $deviceWebHeaderPath,
    $manifestPath, $serviceWorkerPath)) {
  if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
    throw "Required file not found: $requiredPath"
  }
}

$originalFirmwareSource = [System.IO.File]::ReadAllText($firmwareSourcePath)
$originalDeviceWebHeader = [System.IO.File]::ReadAllText($deviceWebHeaderPath)
$originalManifest = [System.IO.File]::ReadAllText($manifestPath)
$originalServiceWorker = [System.IO.File]::ReadAllText($serviceWorkerPath)
$manifest = $originalManifest | ConvertFrom-Json
$currentVersion = [version]$manifest.version
$requestedVersion = [version]$Version
if ($requestedVersion -le $currentVersion) {
  throw "Version $Version must be newer than the current published version $currentVersion."
}

$oldWebPaths = @([string]$manifest.path)
if ($manifest.PSObject.Properties.Name -contains 'builds') {
  $oldWebPaths += @($manifest.builds | ForEach-Object { [string]$_.path })
}
$oldWebPaths = @($oldWebPaths | Select-Object -Unique)
foreach ($oldWebPath in $oldWebPaths) {
  if ($oldWebPath -notmatch '^/firmware/[^/]+\.bin$') {
    throw "Manifest firmware path is invalid: $oldWebPath"
  }
}

foreach ($target in $firmwareTargets) {
  $webPath = "/firmware/$($target.FileStem)-$Version.bin"
  $binaryPath = Join-Path $repositoryRoot ('public' + $webPath.Replace('/', '\'))
  $target | Add-Member -NotePropertyName WebPath -NotePropertyValue $webPath
  $target | Add-Member -NotePropertyName BinaryPath -NotePropertyValue $binaryPath
  $target | Add-Member -NotePropertyName TemporaryPath -NotePropertyValue "$binaryPath.tmp"
  if (Test-Path -LiteralPath $binaryPath) {
    throw "Refusing to overwrite existing release image: $binaryPath"
  }
}

$updatedFirmwareSource = Replace-SingleMatch $originalFirmwareSource `
  'constexpr uint8_t kFirmwareMajor = \d+;' `
  "constexpr uint8_t kFirmwareMajor = $($versionParts[0]);" `
  'firmware major version constant'
$updatedFirmwareSource = Replace-SingleMatch $updatedFirmwareSource `
  'constexpr uint8_t kFirmwareMinor = \d+;' `
  "constexpr uint8_t kFirmwareMinor = $($versionParts[1]);" `
  'firmware minor version constant'
$updatedFirmwareSource = Replace-SingleMatch $updatedFirmwareSource `
  'constexpr uint8_t kFirmwarePatch = \d+;' `
  "constexpr uint8_t kFirmwarePatch = $($versionParts[2]);" `
  'firmware patch version constant'

$releaseCompleted = $false
try {
  Write-Utf8File $firmwareSourcePath $updatedFirmwareSource

  $npm = Get-Command npm -ErrorAction SilentlyContinue
  if ($null -eq $npm) {
    throw 'npm is required to build the embedded device web page.'
  }
  Write-Host 'Building the embedded device web page...'
  Push-Location $repositoryRoot
  try {
    & $npm.Source run build:device -- --outDir $deviceBuildDirectory --emptyOutDir
    if ($LASTEXITCODE -ne 0) {
      throw "Device web build failed with exit code $LASTEXITCODE."
    }
  } finally {
    Pop-Location
  }
  Write-EmbeddedWebHeader $deviceBuildDirectory $deviceWebHeaderPath

  foreach ($target in $firmwareTargets) {
    Write-Host "Building Cyberclip firmware $Version for $($target.Board)..."
    Push-Location $firmwareDirectory
    try {
      Invoke-Python @('-m', 'platformio', 'run', '-e', $target.Environment)
    } finally {
      Pop-Location
    }

    $buildDirectory = Join-Path $buildRoot $target.Environment
    $bootloaderPath = Join-Path $buildDirectory 'bootloader.bin'
    $partitionsPath = Join-Path $buildDirectory 'partitions.bin'
    $applicationPath = Join-Path $buildDirectory 'firmware.bin'
    foreach ($artifactPath in @($bootloaderPath, $partitionsPath, $applicationPath, $bootAppPath, $esptoolPath)) {
      if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
        throw "Required build artifact not found: $artifactPath"
      }
    }

    Remove-Item -LiteralPath $target.TemporaryPath -Force -ErrorAction SilentlyContinue
    Write-Host "Merging $($target.Chip) bootloader, partitions, boot app, and application..."
    $bootloaderOffset = '0x{0:x}' -f $target.BootloaderOffset
    Invoke-Python @(
      $esptoolPath, '--chip', $target.EsptoolChip, 'merge_bin',
      '-o', $target.TemporaryPath,
      '--flash_mode', $target.FlashMode,
      '--flash_freq', $target.FlashFreq,
      '--flash_size', '16MB',
      $bootloaderOffset, $bootloaderPath,
      '0x8000', $partitionsPath,
      '0xe000', $bootAppPath,
      '0x10000', $applicationPath
    )

    if ($target.FlashMode -eq 'keep' -and $target.FlashFreq -eq 'keep') {
      Assert-EmbeddedFile $target.TemporaryPath $bootloaderPath $target.BootloaderOffset
    }
    if ($target.BootloaderOffset -eq 0) {
      $header = New-Object byte[] 1
      $stream = [System.IO.File]::OpenRead($target.TemporaryPath)
      try { $null = $stream.Read($header, 0, 1) } finally { $stream.Dispose() }
      if ($header[0] -ne 0xE9) {
        throw "Merged $($target.Chip) image does not start with a bootloader."
      }
    }
    Assert-EmbeddedFile $target.TemporaryPath $partitionsPath 0x8000
    Assert-EmbeddedFile $target.TemporaryPath $bootAppPath 0xe000
    Assert-EmbeddedFile $target.TemporaryPath $applicationPath 0x10000

    $target | Add-Member -NotePropertyName Size -NotePropertyValue `
      (Get-Item -LiteralPath $target.TemporaryPath).Length -Force
  }

  $legacyTarget = $firmwareTargets[0]
  $manifestLines = @(
    '{',
    "  `"version`": `"$Version`",",
    "  `"path`": `"$($legacyTarget.WebPath)`",",
    '  "address": 0,',
    "  `"size`": $($legacyTarget.Size),",
    '  "builds": ['
  )
  for ($index = 0; $index -lt $firmwareTargets.Count; $index++) {
    $target = $firmwareTargets[$index]
    $separator = if ($index -lt $firmwareTargets.Count - 1) { ',' } else { '' }
    $manifestLines += @(
      '    {',
      "      `"chip`": `"$($target.Chip)`",",
      "      `"board`": `"$($target.Board)`",",
      "      `"path`": `"$($target.WebPath)`",",
      '      "address": 0,',
      "      `"size`": $($target.Size),",
      "      `"flashMode`": `"$($target.FlashMode)`",",
      "      `"flashFreq`": `"$($target.FlashFreq)`"",
      "    }$separator"
    )
  }
  $manifestLines += @('  ]', '}')
  $updatedManifest = ($manifestLines -join [Environment]::NewLine) + [Environment]::NewLine

  $cacheMatch = [regex]::Match($originalServiceWorker, "const CACHE_NAME = 'cyberclip-v(\d+)';")
  if (-not $cacheMatch.Success) {
    throw "Unable to find the service-worker cache version."
  }
  $nextCacheVersion = [int]$cacheMatch.Groups[1].Value + 1
  $updatedServiceWorker = Replace-SingleMatch $originalServiceWorker `
    "const CACHE_NAME = 'cyberclip-v\d+';" `
    "const CACHE_NAME = 'cyberclip-v$nextCacheVersion';" `
    'service-worker cache version'
  $updatedServiceWorker = [regex]::Replace($updatedServiceWorker,
    "(?m)^[ \t]*'/firmware/[^']+\.bin',\r?\n", '')
  $firmwareCacheEntries = ($firmwareTargets | ForEach-Object {
      "  '$($_.WebPath)',"
    }) -join "`n"
  $updatedServiceWorker = Replace-SingleMatch $updatedServiceWorker `
    "(?m)^([ \t]*'/firmware/manifest\.json',)\r?\n" `
    "`$1`n$firmwareCacheEntries`n" `
    'service-worker firmware manifest entry'

  foreach ($target in $firmwareTargets) {
    Move-Item -LiteralPath $target.TemporaryPath -Destination $target.BinaryPath
  }
  Write-Utf8File $manifestPath $updatedManifest
  Write-Utf8File $serviceWorkerPath $updatedServiceWorker

  $newBinaryPaths = @($firmwareTargets | ForEach-Object { $_.BinaryPath })
  foreach ($oldWebPath in $oldWebPaths) {
    $oldBinaryPath = Join-Path $repositoryRoot ('public' + $oldWebPath.Replace('/', '\'))
    if ($newBinaryPaths -notcontains $oldBinaryPath -and
        (Test-Path -LiteralPath $oldBinaryPath -PathType Leaf)) {
      Remove-Item -LiteralPath $oldBinaryPath
    }
  }

  $releaseCompleted = $true
  foreach ($target in $firmwareTargets) {
    Write-Host "Created $($target.WebPath) for $($target.Board) ($($target.Size) bytes)."
  }
  Write-Host "Firmware $Version is ready for the web installer."
} finally {
  if (-not $releaseCompleted) {
    Write-Utf8File $firmwareSourcePath $originalFirmwareSource
    Write-Utf8File $deviceWebHeaderPath $originalDeviceWebHeader
    Write-Utf8File $manifestPath $originalManifest
    Write-Utf8File $serviceWorkerPath $originalServiceWorker
    foreach ($target in $firmwareTargets) {
      Remove-Item -LiteralPath $target.TemporaryPath -Force -ErrorAction SilentlyContinue
      Remove-Item -LiteralPath $target.BinaryPath -Force -ErrorAction SilentlyContinue
    }
  }
}