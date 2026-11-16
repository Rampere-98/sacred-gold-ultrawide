# Kills an agent copy (SacredA1..A8) whose window has not responded for 3 checks in a row (3 min); agentes.sh restarts it.
# Never touches the owner's own "Sacred Gold" folder.
$strikes = @{}
$reviving = @{}
$staleMin = 20

# The time of the last line in a copy's log that shows the agent getting on (the same patterns the mosaic's «PARADO»
# badge uses); $null when the log cannot be read. Only the last 1.5 MB is looked at.
$progressRx = [regex]'^(\d\d):(\d\d):(\d\d)\.\d+ \[\s*\d+\] (CombatLearn: fight over|AutoAttack: collect over|.*journal moved on|Gear: (sold|bought|equips|puts on)|HeroBuild: |AutoAttack: target )'
function Get-LastProgress([string]$log) {
    try {
        $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite, Delete')
        try {
            $len = [math]::Min($fs.Length, 1500000)
            $fs.Seek(-$len, 'End') | Out-Null
            $buf = New-Object byte[] $len
            $n = $fs.Read($buf, 0, $len)
        } finally { $fs.Dispose() }
        $lines = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n) -split "`n"
        for ($i = $lines.Length - 1; $i -ge 0; $i--) {
            $m = $progressRx.Match($lines[$i])
            if ($m.Success) {
                $at = [datetime]::Today.AddHours([int]$m.Groups[1].Value).AddMinutes([int]$m.Groups[2].Value).AddSeconds([int]$m.Groups[3].Value)
                if ($at -gt (Get-Date).AddMinutes(5)) { $at = $at.AddDays(-1) }
                return $at
            }
        }
        # Nothing in the last 1.5 MB: as old as the oldest line read.
        return (Get-Date).AddHours(-1)
    } catch { return $null }
}

while ($true) {
    $procs = Get-Process Sacred -ErrorAction SilentlyContinue | Where-Object { $_.Path -match '\\SacredA[1-8]\\Sacred\.exe$' }
    $seen = @{}
    foreach ($p in $procs) {
        $seen[$p.Id] = $true
        # Hung for real: the window not responding AND the mod's log silent for 3 min (the analyst, 2026-11-11: A8
        # flagged «not responding» while its log went on, killed every 3.7 min).
        $log = Join-Path (Split-Path $p.Path) 'SacredBild.log'
        $silent = (Test-Path $log) -and ((Get-Date) - (Get-Item $log).LastWriteTime).TotalSeconds -gt 180
        if (-not $p.Responding -and $silent) {
            $strikes[$p.Id] = 1 + [int]$strikes[$p.Id]
            if ($strikes[$p.Id] -ge 3) {
                $copy = ($p.Path -replace '.*\\(SacredA[1-8])\\.*', '$1')
                "{0:HH:mm:ss} {1} (PID {2}) not responding for 3 min: killed" -f (Get-Date), $copy, $p.Id
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                $strikes.Remove($p.Id)
            }
        } else {
            $strikes.Remove($p.Id)
        }
    }
    foreach ($id in @($strikes.Keys)) { if (-not $seen[$id]) { $strikes.Remove($id) } }
    # Revive: a copy whose agent has made no progress for $staleMin minutes (the window answers, the log goes on, but
    # no fight, loot, quest, sale or level for that long: a dialog the Enter does not close, the game menu, a hang of
    # the agent's own; the coordinator restarted these by hand three times on 2026-11-15) is asked to save, then closed
    # a cycle later; agentes.sh starts it again. Off while auto-revivir.off is beside this script; never a copy with
    # its agent paused on purpose (SacredBild-stop) nor one up for less than $staleMin minutes.
    if (-not (Test-Path (Join-Path $PSScriptRoot 'auto-revivir.off'))) {
        $now = Get-Date
        foreach ($p in $procs) {
            $dir = Split-Path $p.Path
            $copy = Split-Path $dir -Leaf
            if ($reviving[$copy]) {
                if (($now - $reviving[$copy]).TotalSeconds -ge 30) {
                    Remove-Item (Join-Path $dir 'SacredBild-save-now') -ErrorAction SilentlyContinue
                    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
                    $line = "{0:HH:mm:ss} {1}: no progress for {2} min: saved and closed (agentes.sh starts it again)" -f $now, $copy, $staleMin
                    Add-Content -Path (Join-Path $PSScriptRoot 'vigilante.log') -Value $line -Encoding utf8
                    $line
                    $reviving.Remove($copy)
                }
                continue
            }
            if (($now - $p.StartTime).TotalMinutes -lt $staleMin -or (Test-Path (Join-Path $dir 'SacredBild-stop'))) { continue }
            $last = Get-LastProgress (Join-Path $dir 'SacredBild.log')
            if ($last -and ($now - $last).TotalMinutes -ge $staleMin) {
                New-Item -ItemType File -Path (Join-Path $dir 'SacredBild-save-now') -Force | Out-Null
                $reviving[$copy] = $now
            }
        }
    }
    # The owner's game coming up on its own (Steam, seen 12:21, 13:07, 13:55 and 17:36 on 2026-11-11), within 60 s of a
    # copy's start: closed (the owner, 2026-11-11: «yo no estoy jugando, estoy apenas mirando»). One started by hand,
    # away from the copies' starts, is left alone. Looked at every 10 s; what it does goes to vigilante.log.
    for ($i = 0; $i -lt 6; $i++) {
        $copies = Get-Process Sacred -ErrorAction SilentlyContinue | Where-Object { $_.Path -match '\\SacredA[1-8]\\Sacred\.exe$' }
        $main = Get-Process Sacred -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq 'D:\SteamLibrary\steamapps\common\Sacred Gold\Sacred.exe' }
        foreach ($m in $main) {
            $near = $copies | Where-Object { [math]::Abs(($_.StartTime - $m.StartTime).TotalSeconds) -lt 60 }
            if ($near) {
                $line = "{0:HH:mm:ss} the owner's game (PID {1}, up {2:HH:mm:ss}) came up with a copy's start: closed" -f (Get-Date), $m.Id, $m.StartTime
                try { Stop-Process -Id $m.Id -Force -ErrorAction Stop } catch { $line += " - FAILED: $($_.Exception.Message)" }
                Add-Content -Path (Join-Path $PSScriptRoot 'vigilante.log') -Value $line -Encoding utf8
                $line
            }
        }
        Start-Sleep -Seconds 10
    }
}
