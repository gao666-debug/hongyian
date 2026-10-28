param([string]$ListenAddress = '10.125.126.214', [string]$WslAddress = '172.28.220.208', [int]$Port = 8443)
$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script in Administrator PowerShell.' }
[void][System.Net.IPAddress]::Parse($ListenAddress)
[void][System.Net.IPAddress]::Parse($WslAddress)
if ($Port -lt 1024 -or $Port -gt 65535) { throw 'Invalid port' }
Write-Host "Forward $ListenAddress`:$Port to $WslAddress`:$Port; allow LocalSubnet only."
netsh interface portproxy show v4tov4
netsh interface portproxy add v4tov4 listenaddress=$ListenAddress listenport=$Port connectaddress=$WslAddress connectport=$Port
if ($LASTEXITCODE -ne 0) { throw 'Port forwarding failed' }
$ruleName = "ElderCareBackend-$Port"
if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) { Remove-NetFirewallRule -Name $ruleName }
New-NetFirewallRule -Name $ruleName -DisplayName $ruleName -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -LocalAddress $ListenAddress -RemoteAddress LocalSubnet -Profile Any
Write-Host "Backend address: https://$ListenAddress`:$Port"
