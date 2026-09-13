param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [string]$Cmake = "cmake"
)

if (-not (Test-Path -LiteralPath $SourceDir)) {
    Write-Output "BUILD-002 FAIL: SourceDir not found: $SourceDir"
    exit 2
}

if (Test-Path -LiteralPath $BuildRoot) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
$failDir = Join-Path $BuildRoot "fail"
New-Item -ItemType Directory -Path $failDir | Out-Null
$depsDir = Join-Path $failDir "deps"

$output = & $Cmake -S $SourceDir -B $failDir -G "Visual Studio 17 2022" -A x64 `
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" `
    "-DFETCHCONTENT_BASE_DIR=$depsDir" 2>&1
$exitCode = $LASTEXITCODE
$message = ($output | Out-String).Trim()

$expectedTokens = @("FetchContent", "googletest", "vst3", "clone", "populate")
$matched = @($expectedTokens | Where-Object {
        $message.IndexOf($_, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    })

if ($exitCode -ne 0 -and $matched.Count -gt 0) {
    Write-Output "BUILD-002 PASS: configure failed (exit $exitCode) with a clear missing-dependency error."
    Write-Output "Matched tokens: $($matched -join ', ')"
    Write-Output $message
    exit 0
}

Write-Output "BUILD-002 FAIL: expected a clear missing-dependency configure failure."
Write-Output "exit=$exitCode"
Write-Output $message
exit 1