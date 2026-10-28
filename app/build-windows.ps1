$ErrorActionPreference = 'Stop'
$env:DEVECO_SDK_HOME = 'F:\DevEco Studio\sdk'
$env:JAVA_HOME = 'F:\DevEco Studio\jbr'
$env:NODE_HOME = 'F:\DevEco Studio\tools\node'
$env:Path = "$env:NODE_HOME;$env:JAVA_HOME\bin;" + $env:Path
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force 'node_modules\@ohos' | Out-Null
if (-not (Test-Path 'node_modules\@ohos\hvigor-ohos-plugin')) {
 New-Item -ItemType Junction -Path 'node_modules\@ohos\hvigor-ohos-plugin' -Target 'F:\DevEco Studio\tools\hvigor\hvigor-ohos-plugin' | Out-Null
}
if (-not (Test-Path 'node_modules\@ohos\hvigor')) {
 New-Item -ItemType Junction -Path 'node_modules\@ohos\hvigor' -Target 'F:\DevEco Studio\tools\hvigor\hvigor' | Out-Null
}
$env:NODE_PATH = "$PSScriptRoot\node_modules"
$ErrorActionPreference = 'Continue'
& "$env:NODE_HOME\node.exe" 'F:\DevEco Studio\tools\hvigor\hvigor\bin\hvigor.js' --mode module -p product=default -p module=entry@default assembleHap --no-daemon
exit $LASTEXITCODE
