param([string]$DeviceId = '', [string]$WslBackend = '/home/gwb/oh_code/eldercare-backend')
. (Join-Path $PSScriptRoot 'Common.ps1')
$tools = Get-DemoTools
Start-DemoHdc $tools
$phoneId = Get-PhoneTarget $tools $DeviceId
& wsl.exe -e "$WslBackend/.venv/bin/python" "$WslBackend/tools_demo_start.py"
if ($LASTEXITCODE -ne 0) { throw 'Local backend preparation failed. Check WSL backend configuration.' }
$ipOutput = (& wsl.exe -e hostname -I) -join ' '
$wslIp = [regex]::Match($ipOutput, '\b\d{1,3}(?:\.\d{1,3}){3}\b').Value
if (-not $wslIp) { throw 'Could not determine current WSL address.' }
if (-not (Test-LocalPort 18443)) {
    $relay = Join-Path $PSScriptRoot 'usb-relay.cjs'
    $logDir = Join-Path $script:DemoRoot 'local-logs'
    New-Item -ItemType Directory -Force $logDir | Out-Null
    Start-Process -FilePath $tools.Node -ArgumentList @("`"$relay`"", $wslIp) -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $logDir 'relay.log') -RedirectStandardError (Join-Path $logDir 'relay-error.log') | Out-Null
    Start-Sleep -Milliseconds 600
}
$caFile = Join-Path $script:DemoRoot 'certificates\development-ca.crt'
$healthText = (& curl.exe --silent --show-error --max-time 8 --cacert $caFile 'https://127.0.0.1:18443/healthz') -join ''
if ($LASTEXITCODE -ne 0) { throw 'WSL relay health check failed. Check local relay log and current WSL address.' }
$health = $healthText | ConvertFrom-Json
if ($health.mode -ne 'live') { throw 'Backend is not in live mode.' }
$forwardings = (& $tools.Hdc -s $script:TaskHdcPort fport ls) -join "`n"
$expected = [regex]::Escape($phoneId) + '\s+tcp:18443 tcp:18443\s+\[Reverse\]'
if ($forwardings -notmatch $expected) {
    $result = (& $tools.Hdc -s $script:TaskHdcPort -t $phoneId rport tcp:18443 tcp:18443) -join "`n"
    if ($result -notmatch 'Forwardport result:OK') { throw "USB forwarding failed: $result" }
}
Write-Host "Phone connected: $phoneId"
Write-Host 'Backend ready. App URL: https://127.0.0.1:18443'
Write-Host 'Keep USB connected and keep this computer awake during the demo.'
