# Create a runtime snapshot after a successful Windows build.
# This does not certify the program or perform a virus scan.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$ctpRoot = Split-Path $PSScriptRoot -Parent
$ctpBin = Join-Path $ctpRoot 'build\bin'
$ctpRequired = @('ctp_stock_connect.exe','soptthosttraderapi_se.dll','soptthostmduserapi_se.dll')
foreach ($ctpName in $ctpRequired) {
    if (-not (Test-Path -LiteralPath (Join-Path $ctpBin $ctpName))) {
        throw ('Missing ' + $ctpName + '. Run build_windows.bat first.')
    }
}
$ctpTag = 'CTPStockConnectivity_runtime_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff')
$ctpDist = Join-Path $ctpRoot 'dist'
$ctpStage = Join-Path $ctpDist $ctpTag
New-Item -ItemType Directory -Force -Path (Join-Path $ctpStage 'build\bin') | Out-Null
foreach ($ctpName in $ctpRequired) {
    Copy-Item -LiteralPath (Join-Path $ctpBin $ctpName) -Destination (Join-Path $ctpStage 'build\bin')
}
$ctpConfig = Join-Path $ctpRoot 'config\connection.ini'
New-Item -ItemType Directory -Path (Join-Path $ctpStage 'config') | Out-Null
# Strip credential entries even if the operator filled the main config.
# Only the empty local-config example is copied; never include *.local.ini.
Get-Content -LiteralPath $ctpConfig -Encoding UTF8 |
    Where-Object { $_ -notmatch '^\s*(password|auth_code|authcode)\s*=' } |
    Set-Content -LiteralPath (Join-Path $ctpStage 'config\connection.ini') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $ctpRoot 'config\connection.local.ini.example') -Destination (Join-Path $ctpStage 'config')
Copy-Item -LiteralPath (Join-Path $ctpRoot 'docs') -Destination $ctpStage -Recurse
Copy-Item -LiteralPath (Join-Path $ctpRoot 'run_windows.bat') -Destination $ctpStage
$ctpRuntimeReadme = @'
# CTPStockConnectivity runtime snapshot

This package contains the executable already built on the packaging machine.
It is a phase-1 connectivity tool, not a completed evaluation submission.

On the evaluation physical Windows PC, open an elevated PowerShell terminal,
change to this package directory, then run:

    .\run_windows.bat --mode all

For one-time setup, copy config/connection.local.ini.example to
config/connection.local.ini and fill password= and auth_code= there.
Future runs load those values automatically without prompting.
The runtime package never includes credentials from the packaging machine.
Without configured values, CTP_PASSWORD / CTP_AUTH_CODE and hidden prompts
remain available as fallbacks. MD-only mode needs no AuthCode.
The config/connection.ini contains account identifiers and evaluation fronts.
Read docs/CODE_GUIDE.md and docs/REPORT_ROADMAP.md for scope and next steps.
No orders, cancels, password changes or settlement confirmations are sent.
The logs and flow directories will be created during the run.

The original source package is required for rebuilding; it is separate from
this runtime snapshot. This script did not run a virus scan or certify results.
'@
$ctpRuntimeReadme | Set-Content -LiteralPath (Join-Path $ctpStage 'README.md') -Encoding UTF8
New-Item -ItemType Directory -Path (Join-Path $ctpStage 'scripts') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'collect_environment.ps1') -Destination (Join-Path $ctpStage 'scripts')
$ctpZip = Join-Path $ctpDist ($ctpTag + '.zip')
Compress-Archive -LiteralPath $ctpStage -DestinationPath $ctpZip -CompressionLevel Optimal
$ctpChecksums = @(
    'Package=' + (Split-Path $ctpZip -Leaf),
    'MD5=' + (Get-FileHash -LiteralPath $ctpZip -Algorithm MD5).Hash,
    'SHA256=' + (Get-FileHash -LiteralPath $ctpZip -Algorithm SHA256).Hash,
    'CreatedLocalTime=' + (Get-Date -Format o),
    'VirusScan=NOT_PERFORMED_BY_THIS_SCRIPT'
)
$ctpChecksums | Set-Content -LiteralPath ($ctpZip + '.checksums.txt') -Encoding UTF8
$ctpChecksums | ForEach-Object { Write-Output $_ }
Write-Output ('Package ready: ' + $ctpZip)
