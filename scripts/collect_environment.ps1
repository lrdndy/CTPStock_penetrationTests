# Read-only local evidence. Run on the SAME physical PC as the program.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$ctpRoot = Split-Path $PSScriptRoot -Parent
$ctpLogDir = Join-Path $ctpRoot 'logs'
New-Item -ItemType Directory -Force -Path $ctpLogDir | Out-Null
$ctpFile = Join-Path $ctpLogDir ('environment_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '.txt')
$ctpIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$ctpPrincipal = New-Object Security.Principal.WindowsPrincipal($ctpIdentity)
$ctpLines = @(
    'CTPStockConnectivity local environment evidence',
    ('LocalTime=' + (Get-Date -Format o)),
    ('ComputerName=' + $env:COMPUTERNAME),
    ('OS=' + [Environment]::OSVersion.VersionString),
    ('64BitOS=' + [Environment]::Is64BitOperatingSystem),
    ('Administrator=' + $ctpPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)),
    'PhysicalMachine=MANUAL_CONFIRMATION_REQUIRED',
    'BackendDeviceCollection=NOT_VERIFIED_BY_THIS_SCRIPT',
    '',
    'Current IPv4 addresses on active network adapters:'
)
$ctpLines += Get-NetIPConfiguration | Where-Object { $_.NetAdapter.Status -eq 'Up' } | ForEach-Object {
    'Interface=' + $_.InterfaceAlias + '; Index=' + $_.InterfaceIndex + '; IPv4=' + ($_.IPv4Address.IPAddress -join ',') + '; MAC=' + $_.NetAdapter.MacAddress
}
$ctpLines += ''
$ctpLines += 'Local route selection for evaluation front 101.226.254.157:'
try {
    $ctpLines += (Find-NetRoute -RemoteIPAddress '101.226.254.157' | Format-List IPAddress,InterfaceAlias,InterfaceIndex,DestinationPrefix,NextHop | Out-String).TrimEnd()
} catch {
    $ctpLines += 'Route selection unavailable. Check Get-NetIPConfiguration manually.'
}
$ctpLines | Set-Content -LiteralPath $ctpFile -Encoding UTF8
$ctpLines | ForEach-Object { Write-Output $_ }
Write-Output ('Saved: ' + $ctpFile)
