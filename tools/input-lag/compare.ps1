# Compares the button-to-screen time of our window and Supermodel with
# input-lag.exe (README.md beside this script), each in the game's test menu
# INPUT TEST screen, the same button (A = Beat), the same window size.
#
#   tools\input-lag\compare.ps1 -Ours build\spikeout.exe -Rom build\roms\spikeofe `
#       -State Z:\Emu\tmp\lag\inputtest.state -Supermodel Z:\Emu\tmp\lag\sm [-PresentMon PresentMon.exe]
#
# -State: a save state on the INPUT TEST screen made by the same build's gamerun:
#   gamerun ROMDIR --blocks 800000000 --press test@600 --press service@800 --press service@830
#     --press service@860 --press service@890 --press test@950 --save-state FILE@1100
# -Supermodel: a COPY of a Supermodel folder (supermodel.exe, Assets, Config, NVRAM,
#   ROMs\spikeofe.zip) whose Config\Supermodel.ini reads the pad through XInput:
#   InputSystem = xinput, InputBeat = JOY_BUTTON1, InputServiceA = JOY_BUTTON7,
#   InputTestA = JOY_BUTTON8 (Back moves in the test menu, Start picks).
# -PresentMon: PresentMon's console exe (github.com/GameTechDev/PresentMon); it
#   needs an elevated PowerShell (or the "Performance Log Users" group). It adds
#   each program's present mode and its present-to-display time.
# The windows go on top and take the focus: leave the PC alone while it runs
# (~3 minutes a program at 200 presses).
param(
    [Parameter(Mandatory)] [string]$Ours,
    [Parameter(Mandatory)] [string]$Rom,
    [Parameter(Mandatory)] [string]$State,
    [Parameter(Mandatory)] [string]$Supermodel,
    [string]$PresentMon = '',
    [int]$Presses = 200,
    [int]$Runs = 1,
    [string]$Out = 'Z:\Emu\tmp\lag\compare',
    [string]$Tool = "$PSScriptRoot\..\..\build-lag\input-lag.exe"
)
$Ours = (Resolve-Path $Ours -ErrorAction Stop).Path; $Rom = (Resolve-Path $Rom -ErrorAction Stop).Path
$State = (Resolve-Path $State -ErrorAction Stop).Path; $Supermodel = (Resolve-Path $Supermodel -ErrorAction Stop).Path
$Tool = (Resolve-Path $Tool -ErrorAction Stop).Path
New-Item -ItemType Directory -Force $Out | Out-Null

# Our window from a folder of its own (portable: settings there, the window
# 992 x 768 as Supermodel's, no update check).
$win = Join-Path $Out 'ours-folder'
New-Item -ItemType Directory -Force $win | Out-Null
Copy-Item $Ours (Join-Path $win 'spikeout.exe') -Force
Set-Content -Encoding ascii (Join-Path $win 'portable.txt') ''
Set-Content -Encoding ascii (Join-Path $win 'settings.ini') "window_w = 992`nwindow_h = 768`nwindow_x = 200`nwindow_y = 150`ncheck_updates = 0`n"

$programs = @(
    @{ Name = 'ours'; Exe = 'spikeout.exe'
       Args = @('--cwd', $win, '--settle', '6000')
       Program = @("$win\spikeout.exe", $Rom, '--load-state', $State) },
    @{ Name = 'supermodel'; Exe = 'supermodel.exe'
       Args = @('--cwd', $Supermodel, '--settle', '15000',
                '--tap', 'start,back,back,back,back,start', '--tap-gap', '1500', '--after-tap', '3000')
       Program = @("$Supermodel\supermodel.exe", 'ROMs\spikeofe.zip') }
)

& $Tool --self-test --presses 60 --label floor | Tee-Object (Join-Path $Out 'floor.txt')
foreach ($run in 1..$Runs) {
    foreach ($p in $programs) {
        $label = "$($p.Name)-$run"
        $pm = $null
        if ($PresentMon) {
            $pmCsv = Join-Path $Out "$label-presentmon.csv"
            $pm = Start-Process -FilePath $PresentMon -PassThru -WindowStyle Hidden -ArgumentList @(
                '--process_name', $p.Exe, '--v1_metrics', '--no_console_stats', '--output_file', "`"$pmCsv`"",
                '--timed', '900', '--terminate_after_timed', '--stop_existing_session')
        }
        & $Tool @($p.Args) --presses $Presses --label $label --out (Join-Path $Out "$label.csv") --shots (Join-Path $Out $label) `
            -- @($p.Program) 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match '^(input-lag|  )' } |
            Tee-Object (Join-Path $Out "$label.txt")
        if ($pm) {
            Stop-Process -Id $pm.Id -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
            # Present mode and present-to-display time (msUntilDisplayed) of the program's presents.
            if (Test-Path $pmCsv) {
                $rows = Import-Csv $pmCsv | Where-Object { $_.Application -ieq $p.Exe }
                $shown = $rows | Where-Object { $_.msUntilDisplayed -and [double]$_.msUntilDisplayed -gt 0 }
                $modes = ($rows | Group-Object PresentMode | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', '
                $sorted = @($shown | ForEach-Object { [double]$_.msUntilDisplayed } | Sort-Object)
                $median = if ($sorted.Count) { $sorted[[int][math]::Floor($sorted.Count / 2)] } else { 0 }
                $line = "  presentmon $label : $($rows.Count) presents ($modes); present to display median {0:N2} ms over $($sorted.Count)" -f $median
                $line | Tee-Object -Append (Join-Path $Out "$label.txt")
            } else {
                "  presentmon $label : no CSV (not elevated?)" | Tee-Object -Append (Join-Path $Out "$label.txt")
            }
        }
    }
}
"Results in $Out"
