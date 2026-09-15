# Test the installed program's real price parser before displaying evidence.
# The trailing unknown option stops parsing before any config, login or order.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Price)
$ErrorActionPreference = 'Stop'
$ctpRoot = Split-Path $PSScriptRoot -Parent
$ctpExe = Join-Path $ctpRoot 'build\bin\ctp_stock_connect.exe'
$ctpStopOption = '--price-format-test-stop'

function Invoke-PriceParserProbe([string]$Value) {
    # Native stderr is evidence, not a PowerShell terminating error.
    $ErrorActionPreference = 'Continue'
    $output = @(& $ctpExe '--price' $Value $ctpStopOption 2>&1 | ForEach-Object { $_.ToString() })
    $code = $LASTEXITCODE
    return [pscustomobject]@{ Code = $code; Text = ($output -join "`n") }
}
function Test-ParserError($Probe, [string]$Message) {
    return ($Probe.Code -eq 2 -and $Probe.Text -match ('(?m)FATAL ' + [regex]::Escape($Message) + '\s*$'))
}

try {
    if (-not (Test-Path -LiteralPath $ctpExe -PathType Leaf)) {
        throw 'Run build_windows.bat first; build\bin\ctp_stock_connect.exe is missing.'
    }
    if ($Price.Length -gt 80 -or $Price -match '[^\x20-\x7E]' -or $Price.Contains('"')) {
        throw 'Use at most 80 printable ASCII characters, without double quotes; example: 0.0427abc.'
    }
    # A valid control must reach the stop option. Incompatible binaries fail
    # closed: no test PASS and no fabricated price warning.
    $control = Invoke-PriceParserProbe '0.0427'
    if (-not (Test-ParserError $control 'Unknown option; use --help.')) {
        throw ('Parser control failed or this executable is incompatible. Output: ' + $control.Text)
    }
    $probe = Invoke-PriceParserProbe $Price
    $blocked = Test-ParserError $probe '--price must be a positive finite number.'
    $ctpLogDir = Join-Path $ctpRoot ('logs\price_format_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '_pid' + $PID)
    New-Item -ItemType Directory -Path $ctpLogDir | Out-Null
    $ctpLog = Join-Path $ctpLogDir 'run.log'
    @(
        'TEST price_format source=EXISTING_EXECUTABLE_PARSER'
        ('INPUT price=' + $Price)
        'MODE=OFFLINE_PARSER_TEST config=NOT_READ network=NOT_CONNECTED order_api=NOT_CALLED'
        'daily_count_delta=0 second_count_delta=0'
        ('CONTROL exit_code=' + $control.Code)
        $control.Text
        ('PROBE exit_code=' + $probe.Code)
        $probe.Text
    ) | Set-Content -LiteralPath $ctpLog -Encoding UTF8
    Write-Host ('Input price: ' + $Price)
    Write-Host $probe.Text
    Write-Host ('EVIDENCE log=' + $ctpLog)
    if (-not $blocked) {
        Add-Content -LiteralPath $ctpLog -Encoding UTF8 -Value 'RESULT FAIL expected_price_format_rejection_not_observed popup=NOT_SHOWN'
        Write-Host 'RESULT FAIL: no price-format rejection observed. No warning popup is displayed.'
        exit 1
    }
    Add-Type -AssemblyName System.Windows.Forms
    $text = "价格格式错误`r`n`r`n本次输入价格：$Price`r`n`r`n程序已拒绝该输入：价格必须是大于 0 的有效数字，不能包含无效字母后缀。`r`n`r`n测试方式：调用现有程序的价格解析，未读取连接配置、未连接柜台、未发送报单。`r`n每日／每秒报单计数：不增加`r`n`r`n请在关闭本提示前截图。"
    [void][System.Windows.Forms.MessageBox]::Show($text, 'CTPStockConnectivity 价格异常提示',
        [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Warning)
    Add-Content -LiteralPath $ctpLog -Encoding UTF8 -Value 'RESULT PASS price_format_rejected_by_program popup=DISPLAYED'
    Write-Host 'RESULT PASS: the program rejected the invalid price; warning displayed. No orders sent.'
    exit 0
} catch {
    Write-Host ('RESULT FAIL: ' + $_.Exception.Message)
    exit 2
}
