param([string]$DeviceId = '', [string]$Hap = '')
. (Join-Path $PSScriptRoot 'Common.ps1')
$tools = Get-DemoTools
Start-DemoHdc $tools
$phoneId = Get-PhoneTarget $tools $DeviceId
if (-not $Hap) {
    $signedCandidates = @(
        (Join-Path $script:DemoRoot 'install\zhian-care-debug-signed.hap'),
        (Join-Path $script:DemoRoot 'app\entry\build\default\outputs\default\entry-default-signed.hap')
    )
    $Hap = $signedCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Hap -or -not (Test-Path $Hap)) {
    throw 'Signed phone HAP is missing. Open app in DevEco, configure HarmonyOS automatic signing for this phone, and build the signed HAP first. See README.'
}
if ($Hap -match 'unsigned') { throw 'An unsigned HAP cannot be used as the phone installation package.' }
& (Join-Path $PSScriptRoot 'Prepare-UsbDemo.ps1') -DeviceId $phoneId
$result = (& $tools.Hdc -s $script:TaskHdcPort -t $phoneId install -r $Hap) -join "`n"
Write-Host $result
if ($result -notmatch 'install bundle successfully') { throw 'Phone install failed. Check signing profile, device authorization and bundle name.' }
$result = (& $tools.Hdc -s $script:TaskHdcPort -t $phoneId shell aa start -a EntryAbility -b com.zhian.eldercare) -join "`n"
Write-Host $result
if ($result -notmatch 'start ability successfully') { throw 'App launch failed.' }
Write-Host 'Open Device > Connect backend. User: guardian. Use the password configured on this computer.'
