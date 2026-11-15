# What the game has sent to the lobby (tools/problem_report.hpp part 2; lobby/src/index.js, "Reports sent by the game
# itself"), fetched to a folder of the owner's: each network game's numbers (stats.jsonl, a line each), the games' inputs
# (inputs/*.inputs.locked, for tools/guess-study.cpp) and the problem
# bundles (problems/*.zip.locked), unlocked with the owner's report key into problems/*.zip (gamerun --unlock).
#   tools/fetch-reports.ps1 [-Out DIR] [-TokenFile FILE] [-KeyFile FILE] [-Gamerun EXE]
# The admin token (the lobby's ADMIN_TOKEN secret) and the report key stay outside the repo, as files of the owner's.
# Fetches only what is new since the last time (Out\last.txt keeps where it got to).
#   -Only problems,inputs,stats: only those kinds (the games' inputs are only for the guessing study).
# Files come 16 at a time (Windows' curl.exe --parallel): one at a time each waited a round trip to the lobby and R2,
# ~0.3 s, minutes for a day's few hundred (2026-11-14); the same R2 reads either way.
param([string]$Out = "$env:USERPROFILE\spikeout-reports", [string]$TokenFile = "$env:USERPROFILE\spikeout-admin-token.txt",
      [string]$KeyFile = "$env:USERPROFILE\spikeout-report-key.txt", [string]$Gamerun = '',
      [string]$Lobby = 'https://spikeout-lobby.spikeout-lobby.workers.dev',
      [ValidateSet('problems', 'inputs', 'stats')][string[]]$Only = @('problems', 'inputs', 'stats'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # (Windows PowerShell 5's progress bar slows its web calls many times over)
if (-not (Test-Path $TokenFile)) { "no admin token at $TokenFile (the lobby's ADMIN_TOKEN, a file of yours)"; exit 1 }
$headers = @{ Authorization = "Bearer $((Get-Content -Raw $TokenFile).Trim())" }
New-Item -ItemType Directory -Force "$Out\problems" | Out-Null
$last = @{ stats = 0; problems = 0 }
if (Test-Path "$Out\last.txt") { Get-Content "$Out\last.txt" | ForEach-Object { $k, $v = $_ -split '='; $last[$k] = [int]$v } }

$stats = 0
if ($Only -contains 'stats') { do {
    $rows = Invoke-RestMethod -Uri "$Lobby/v1/admin/stats?after=$($last.stats)" -Headers $headers
    foreach ($r in $rows) {
        $line = [ordered]@{ id = $r.id; at = [DateTimeOffset]::FromUnixTimeMilliseconds($r.at).ToString('u'); version = $r.version;
                            country = $r.country; game = ($r.json | ConvertFrom-Json) } | ConvertTo-Json -Compress -Depth 6
        Add-Content -Path "$Out\stats.jsonl" -Value $line -Encoding utf8
        $last.stats = [int]$r.id; $stats++
    }
} while ($rows.Count -ge 500) }

New-Item -ItemType Directory -Force "$Out\inputs" | Out-Null
if (-not $last.ContainsKey('inputs')) { $last.inputs = 0 }
$files = @()
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
foreach ($kind in @('problems', 'inputs') | Where-Object { $Only -contains $_ }) { # (bundles; inputs for tools/guess-study.cpp)
    do {
        # (PowerShell 5 hands the list back as one object: its rows unpacked, none an empty list)
        $rows = @((Invoke-RestMethod -Uri "$Lobby/v1/admin/${kind}?after=$($last[$kind])" -Headers $headers) | ForEach-Object { $_ })
        if (-not $rows.Count) { break }
        # (the list's files at once: a curl config with the token, read only by curl, deleted after)
        $want = @(foreach ($r in $rows) {
            [pscustomobject]@{ id = [int]$r.id; path = "$Out\$kind\$($r.id)-$($r.version)-$($r.country)-$([IO.Path]::GetFileName($r.key))";
                               url = "$Lobby/v1/admin/file/$([Uri]::EscapeDataString($r.key))" }
        })
        $config = [IO.Path]::GetTempFileName()
        try {
            $lines = @("header = `"Authorization: $($headers.Authorization)`"", 'fail', 'silent', 'show-error')
            # (forward slashes: in a quoted curl config string a backslash starts an escape)
            foreach ($w in $want) { $lines += "url = `"$($w.url)`"", "output = `"$($w.path -replace '\\', '/')`"" }
            [IO.File]::WriteAllLines($config, $lines)
            & $curl --parallel --parallel-max 16 --config $config
        } finally { Remove-Item $config -ErrorAction SilentlyContinue }
        # (where it got to: the last of an unbroken run of files that came; a failed one fetched again next time)
        foreach ($w in $want) {
            if (-not (Test-Path $w.path) -or (Get-Item $w.path).Length -eq 0) { "not fetched: $($w.path) (again next time)"; break }
            $files += $w.path
            $last[$kind] = $w.id
        }
    } while ($rows.Count -ge 500 -and $last[$kind] -eq $want[-1].id)
}
"stats=$($last.stats)", "problems=$($last.problems)", "inputs=$($last.inputs)" | Set-Content "$Out\last.txt"

"$stats games' numbers (in $Out\stats.jsonl), $(@($files | Where-Object { $_ -like '*\problems\*' }).Count) problem bundles, " +
    "$(@($files | Where-Object { $_ -like '*\inputs\*' }).Count) games' inputs"
if ($files.Count) {
    $exe = if ($Gamerun) { $Gamerun } else { Join-Path (Split-Path $PSScriptRoot) 'build\gamerun.exe' }
    if ((Test-Path $exe) -and (Test-Path $KeyFile)) { # (20 at a time: all at once can pass Windows' command line limit)
        for ($i = 0; $i -lt $files.Count; $i += 20) { & $exe --unlock @($files[$i..([math]::Min($i + 19, $files.Count - 1))]) --report-key $KeyFile }
    }
    else { "to open them: gamerun --unlock FILE.locked --report-key $KeyFile" }
}
# (this month's use of R2's free operations and the space, as the lobby counts them: it refuses past half of each)
$u = Invoke-RestMethod -Uri "$Lobby/v1/admin/usage" -Headers $headers
"R2 this month ($($u.month)): class A $($u.r2a[0]) of $($u.r2a[1]), class B $($u.r2b[0]) of $($u.r2b[1]); stored " +
    "$([math]::Round($u.stored / 1MB, 1)) MB of $([math]::Round($u.kept / 1GB, 1)) GB"
