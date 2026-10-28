param([string]$ListenAddress = '10.125.126.214', [int]$Port = 8443)
$ErrorActionPreference = 'Stop'
netsh interface portproxy delete v4tov4 listenaddress=$ListenAddress listenport=$Port
Get-NetFirewallRule -Name "ElderCareBackend-$Port" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
