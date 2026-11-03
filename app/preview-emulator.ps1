param([string]$Target = '127.0.0.1:5555', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-windows.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'App build failed' }
}
$hdc = 'F:\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$hap = Join-Path $PSScriptRoot 'entry\build\default\outputs\default\entry-default-unsigned.hap'
$installed = & $hdc -t $Target install -r $hap
if (($installed -join "`n") -notmatch 'install bundle successfully') { throw 'Emulator install failed' }
& $hdc -t $Target shell aa force-stop com.zhian.eldercare
& $hdc -t $Target shell aa start -a EntryAbility -b com.zhian.eldercare
Start-Sleep -Milliseconds 1500
& $hdc -t $Target shell snapshot_display -f /data/local/tmp/yihongan-blue-home.jpeg
& $hdc -t $Target file recv /data/local/tmp/yihongan-blue-home.jpeg (Join-Path $PSScriptRoot 'docs\yihongan-blue-home.jpeg')
