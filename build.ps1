$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$outputDir = Join-Path $projectDir 'dist'
$resourceObject = Join-Path $env:TEMP ("ScreenDimmer-" + [guid]::NewGuid().ToString('N') + '.o')
$gcc = (Get-Command x86_64-w64-mingw32-gcc.exe -ErrorAction Stop).Source
$windres = (Get-Command x86_64-w64-mingw32-windres.exe -ErrorAction Stop).Source

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
Push-Location $projectDir
try {
    & $windres -O coff 'screendimmer.rc' -o $resourceObject
    if ($LASTEXITCODE -ne 0) { throw "windres failed with exit code $LASTEXITCODE" }

    & $gcc -std=c11 -Os -ffunction-sections -fdata-sections `
        -Wall -Wextra -Wpedantic -mwindows -static -s `
        '-Wl,--gc-sections' 'screendimmer.c' $resourceObject `
        -o (Join-Path $outputDir 'ScreenDimmer.exe') `
        -lcomctl32 -lshell32 -ladvapi32 -lgdi32 -luser32 -lole32 -luuid
    if ($LASTEXITCODE -ne 0) { throw "gcc failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
    Remove-Item -LiteralPath $resourceObject -ErrorAction SilentlyContinue
}

Get-Item (Join-Path $outputDir 'ScreenDimmer.exe') | Select-Object FullName, Length
