Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$root = [System.Windows.Automation.AutomationElement]::RootElement
$condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'发现新镜像')
$dialog = $root.FindFirst([System.Windows.Automation.TreeScope]::Children,$condition)
if (-not $dialog) { throw 'Expected image update dialog not found' }
$yes = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'是')
$button = $dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$yes)
if (-not $button) { throw 'Download button not found' }
$pattern=$button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
$pattern.Invoke()
