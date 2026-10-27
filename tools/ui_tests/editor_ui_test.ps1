# Drives the real game_editor window with real mouse and keyboard input and checks what it did through the editor's --log file.
# It needs an unlocked interactive Windows desktop and a GPU. Run it with Windows PowerShell 5 (it uses System.Drawing):
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\ui_tests\editor_ui_test.ps1 [-Editor <game_editor.exe>] [-Backend dx12|vulkan]
#
# It works on a copy of samples/showcase in a temp folder, so Ctrl+S never changes the checked-in sample. Screenshots go to
# artifacts/ui/. The exit code is the number of failed checks.
param(
    [string]$Editor = "$PSScriptRoot\..\..\build\win-dev\tools\game_editor\Release\game_editor.exe",
    [string]$Backend = "dx12"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$shots = Join-Path $root 'artifacts\ui'
New-Item -ItemType Directory -Force $shots | Out-Null

Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,int d,UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte sc,uint f,UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
  [StructLayout(LayoutKind.Sequential)] public struct POINT{public int X,Y;}
  public static void Shot(string path,int x,int y,int w,int h){
    using(var b=new Bitmap(w,h)){ using(var g=Graphics.FromImage(b)){ g.CopyFromScreen(x,y,0,0,new Size(w,h)); } b.Save(path,ImageFormat.Png);} }
}
"@
[W]::SetProcessDPIAware() | Out-Null

$script:failures = 0
function Check($name, $condition) {
    if ($condition) { Write-Host "  ok    $name" } else { Write-Host "  FAIL  $name"; $script:failures++ }
}
function Origin($h) { $p = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$p) | Out-Null; $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null; @($p.X, $p.Y, $r.R, $r.B) }
function Shot($h, $name) { $o = Origin $h; [W]::Shot((Join-Path $shots "$name.png"), $o[0], $o[1], $o[2], $o[3]) }
function Focus($h) { [W]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [W]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [W]::ShowWindow($h, 9) | Out-Null; [W]::SetForegroundWindow($h) | Out-Null }
function MoveTo($h, $x, $y) { $o = Origin $h; [W]::SetCursorPos($o[0] + $x, $o[1] + $y) | Out-Null }
function Click($h, $x, $y) { MoveTo $h $x $y; Start-Sleep -Milliseconds 120; [W]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 250 }
function DoubleClick($h, $x, $y) { MoveTo $h $x $y; Start-Sleep -Milliseconds 150; foreach ($i in 1, 2) { [W]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 30; [W]::mouse_event(4,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60 }; Start-Sleep -Milliseconds 350 }
function Key($vk, $ctrl = $false) {
    # Left Control with its scancode (0x1D), as a real keyboard sends it; the platform layer tells left from right by the scancode.
    if ($ctrl) { [W]::keybd_event(0xA2,0x1D,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
    [W]::keybd_event($vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W]::keybd_event($vk,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60
    if ($ctrl) { [W]::keybd_event(0xA2,0x1D,2,[UIntPtr]::Zero) }
    Start-Sleep -Milliseconds 250
}
function LogText($path) { if (Test-Path $path) { Get-Content $path -Raw } else { '' } }

# A private copy of the sample, so saving never touches the repository.
$work = Join-Path $env:TEMP ("editor_ui_test_" + [guid]::NewGuid().ToString('N'))
Copy-Item (Join-Path $root 'samples\showcase') $work -Recurse
$log = Join-Path $work 'editor.log'
$ws = Join-Path $work 'workspace.json'

$p = Start-Process -FilePath $Editor -ArgumentList @('--backend', $Backend, '--workspace', $ws, '--log', $log) -WorkingDirectory (Split-Path $Editor) -PassThru
Start-Sleep 4
$p.Refresh()
$h = $p.MainWindowHandle
Check 'the editor window exists' ($h -ne [IntPtr]::Zero)
Focus $h; Start-Sleep 1
Check 'the editor window is in front' ([W]::GetForegroundWindow() -eq $h)
Shot $h '01-start'

Write-Host 'typing into a text field'
Click $h 150 172
[System.Windows.Forms.SendKeys]::SendWait($work)
Start-Sleep -Milliseconds 300
Shot $h '02-typed-path'
Click $h 41 213
Start-Sleep -Milliseconds 700
Check 'a typed path opens the project' ((LogText $log) -match 'opened project Showcase')

Write-Host 'opening a scene by double click'
Click $h 192 53
DoubleClick $h 100 521
Check 'a double click opens the scene' ((LogText $log) -match 'opened scene')
Click $h 287 53
Start-Sleep -Milliseconds 500
Shot $h '03-scene-open'

Write-Host 'selecting and editing with the keyboard'
Click $h 100 308
Key 0x44 $true            # Ctrl+D
Check 'Ctrl+D duplicates the selection' ((LogText $log) -match 'duplicated entity')
Key 0x5A $true            # Ctrl+Z
Check 'Ctrl+Z undoes it' ((LogText $log) -match 'undid')
Key 0x59 $true            # Ctrl+Y
Check 'Ctrl+Y redoes it' ((LogText $log) -match 'redid')
Click $h 100 308          # undo and redo clear the selection, so select again
Key 0x2E                  # Delete
Check 'Delete removes the selection' ((LogText $log) -match 'removed entity')
Key 0x5A $true
Key 0x53 $true            # Ctrl+S
Check 'Ctrl+S saves the scene' ((LogText $log) -match 'saved scene to')
Shot $h '04-edited'

Write-Host 'play and stop'
Key 0x74                  # F5
Start-Sleep -Milliseconds 800
Check 'F5 starts Play' ((LogText $log) -match 'play started')
Shot $h '05-playing'
Key 0x74
Check 'F5 stops Play' ((LogText $log) -match 'play stopped')

Write-Host 'the viewport'
MoveTo $h 1000 450
[W]::mouse_event(0x800, 0, 0, 120, [UIntPtr]::Zero)   # wheel up
Start-Sleep -Milliseconds 300
Shot $h '06-viewport'

Check 'no error lines in the log' (-not ((LogText $log) -match '\[Error\]'))
$p | Stop-Process -Force
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "failed checks: $script:failures"
exit $script:failures
