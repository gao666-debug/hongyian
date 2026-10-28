$ErrorActionPreference = 'Stop'
$script:DemoRoot = Split-Path -Parent $PSScriptRoot
$script:TaskHdcPort = 18710
function Get-DemoTools {
    $sdkCandidates = @('F:\DevEco Studio', 'C:\Program Files\Huawei\DevEco Studio')
    foreach ($sdkRoot in $sdkCandidates) {
        $hdcPath = Join-Path $sdkRoot 'sdk\default\openharmony\toolchains\hdc.exe'
        $nodePath = Join-Path $sdkRoot 'tools\node\node.exe'
        if ((Test-Path $hdcPath) -and (Test-Path $nodePath)) {
            return @{ Hdc = $hdcPath; Node = $nodePath }
        }
    }
    throw 'DevEco tools not found. Update SDK paths in scripts/Common.ps1.'
}
function Test-LocalPort([int]$Port) {
    $client = New-Object System.Net.Sockets.TcpClient
    try { return $client.ConnectAsync('127.0.0.1', $Port).Wait(500) -and $client.Connected }
    catch { return $false }
    finally { $client.Dispose() }
}
function Start-DemoHdc($Tools) {
    if (-not (Test-LocalPort $script:TaskHdcPort)) {
        Start-Process -FilePath $Tools.Hdc -ArgumentList @('-s', "$script:TaskHdcPort", '-m') -WindowStyle Hidden | Out-Null
        for ($attempt = 0; $attempt -lt 20; $attempt++) {
            if (Test-LocalPort $script:TaskHdcPort) { return }
            Start-Sleep -Milliseconds 250
        }
        throw 'HDC server failed to start on port 18710.'
    }
}
function Get-PhoneTarget($Tools, [string]$RequestedId) {
    $rawTargets = @(& $Tools.Hdc -s $script:TaskHdcPort list targets)
    $targets = @($rawTargets | ForEach-Object { $_.Trim() } | Where-Object {
        $_ -match '^[A-Za-z0-9][A-Za-z0-9._:-]{5,}$' -and $_ -notmatch '^(127\.|localhost|COM\d+)'
    })
    if ($RequestedId) {
        if ($RequestedId -notin $targets) { throw 'Requested phone is not connected/authorized in HDC.' }
        return $RequestedId
    }
    if ($targets.Count -eq 0) { throw 'No phone detected. Enable USB/HDC debugging and accept authorization on the unlocked phone.' }
    if ($targets.Count -gt 1) { throw 'Multiple phones connected. Pass -DeviceId with the phone serial shown by hdc list targets.' }
    return $targets[0]
}
