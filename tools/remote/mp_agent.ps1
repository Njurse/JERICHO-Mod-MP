# mp_agent.ps1 -- the "leave it running" half of a two-machine test rig.
#
# START IT ONCE on the other PC (START_AGENT.bat does it for you) and leave the
# window open. From then on that machine is hands-free: this script stays resident,
# takes a small fixed set of commands over the LAN, and -- the point of it --
# UPDATES ITSELF AND RELAUNCHES when a new build arrives:
#
#   sync  -> apply a build (a zip of the files that changed), and if a game was
#            running, stop it, update, and start it again with the SAME arguments.
#   start -> launch the game (host or join) on the current build
#   stop  -> close the game
#   log   -> send its log back (and its crash dump, if there is one)
#   status-> build stamp, whether the game is up, log/dump presence
#   ping  -> are you there
#   quit  -> stop being resident
#
# Nothing here is installed: PowerShell is part of Windows, and the script only
# writes inside the game folder it is pointed at.
#
# The token is NOT a security boundary -- it stops a stray program on the same LAN
# from driving this machine by accident. Treat it as a "do not touch" label, not
# as a password.

[CmdletBinding()]
param(
    [int]    $Port  = 1401,
    [string] $Token = 'jericho-mp',
    [string] $Root  = '',              # defaults to the folder this script sits in
    [int]    $LogTailBytes = 400000    # how much of JERICHO.log to send back
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent $MyInvocation.MyCommand.Path
}

# Tolerate a trailing separator or a stray quote. %~dp0 (what the launcher passes)
# ends in a backslash, and a trailing backslash before the closing quote makes
# PowerShell treat the quote as escaped -- so the path arrives as
# C:\...\Release_dev" and Resolve-Path fails with "Illegal characters in path".
$Root = $Root.TrimEnd('\', '/', '"')
$Root = (Resolve-Path -LiteralPath $Root).Path

$Exe     = Join-Path $Root 'REDRIVER2_dev.exe'
$LogFile = Join-Path $Root 'JERICHO.log'
$DmpFile = Join-Path $Root 'JERICHO.dmp'
$OwnLog  = Join-Path $Root 'mp_agent.log'
$Staging = Join-Path $Root '_mp_staging'

$script:Game      = $null   # the running game process, or $null
$script:LastArgs  = $null   # its arguments, so a sync can put it back exactly
# A plain flag, NOT the command's return value: every helper here emits output of
# its own ("OK stopped" and friends) and a `$quit = Invoke-Command ...` reads that
# as "yes, quit" -- which silently shut the agent down on the first sync.
$script:WantQuit  = $false

function Write-Own {
    param([string] $Message)
    $line = '{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    try { Add-Content -LiteralPath $OwnLog -Value $line -Encoding UTF8 } catch { }
    Write-Host $line
}

function Get-BuildStamp {
    $v = Join-Path $Root 'VERSION.txt'
    if (Test-Path -LiteralPath $v) { return (Get-Content -LiteralPath $v -TotalCount 1).Trim() }
    return 'unknown'
}

function Test-GameRunning {
    if ($null -ne $script:Game) {
        try { if (-not $script:Game.HasExited) { return $true } } catch { }
        $script:Game = $null
    }
    # also notice a game the user started by hand (double-clicking PLAY_*.bat)
    $p = Get-Process -Name 'REDRIVER2_dev' -ErrorAction SilentlyContinue
    if ($null -ne $p) { return $true }
    return $false
}

function Get-Hashes {
    # SHA256 of every file the rig may replace, so the other end can send ONLY what
    # changed. The game data is deliberately NOT in this list: it is 1.6 GB and
    # does not change between builds.
    $out = @{}
    $sets = @('REDRIVER2_dev.exe', 'VERSION.txt', 'JERICHO')
    foreach ($s in $sets) {
        $p = Join-Path $Root $s
        if (Test-Path -LiteralPath $p -PathType Leaf) {
            $out[$s] = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
        } elseif (Test-Path -LiteralPath $p -PathType Container) {
            Get-ChildItem -LiteralPath $p -Recurse -File | ForEach-Object {
                # Forward slashes: the comparing end (mp_remote.py) speaks UNIX-style
                # relative paths, and a mismatch here silently turns a delta sync into
                # a full 311-file resend.
                $rel = $_.FullName.Substring($Root.Length).TrimStart('\').Replace('\', '/')
                $out[$rel] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        }
    }
    return $out
}

function Send-Line {
    # Raw stream, NOT a StreamWriter: a StreamReader/Writer buffers, and a buffered
    # reader will happily swallow the binary payload that follows a `sync` command
    # line. Everything here reads and writes bytes.
    param([System.IO.Stream] $S, [string] $Text)
    $b = [System.Text.Encoding]::ASCII.GetBytes($Text + "`n")
    $S.Write($b, 0, $b.Length); $S.Flush()
}

function Send-Bytes {
    param([System.IO.Stream] $S, [byte[]] $Data)
    $S.Write($Data, 0, $Data.Length); $S.Flush()
}

function Read-Exact {
    param([System.IO.Stream] $S, [int] $Count)
    $buf = New-Object byte[] $Count
    $got = 0
    while ($got -lt $Count) {
        $n = $S.Read($buf, $got, $Count - $got)
        if ($n -le 0) { throw "peer closed while $got of $Count bytes were expected" }
        $got += $n
    }
    return $buf
}

function Invoke-Sync {
    # The peer sends, right after the command line:
    #     <name>\n<length>\n<that many bytes: a zip>
    # We verify every file's SHA256 against the manifest inside it, and only then
    # extract. If a game was running we stop it, apply, and start it again -- which
    # is what makes the other PC hands-free.
    param([System.IO.Stream] $S)

    $name   = (Read-Line $S)
    Write-Own ("sync: taking '{0}'" -f $name)
    $length = [int](Read-Line $S)
    if ($length -le 0 -or $length -gt 400MB) { Send-Line $S "ERR bad length $length"; return }

    Write-Own ("sync: receiving {0:N1} MB" -f ($length / 1MB))
    $zip = Read-Exact $S $length
    Write-Own ("sync: got {0:N0} bytes" -f $zip.Length)

    if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
    $null = New-Item -ItemType Directory -Path $Staging -Force
    $zipPath = Join-Path $Staging 'build.zip'
    [System.IO.File]::WriteAllBytes($zipPath, $zip)

    # Unpack into staging FIRST, and refuse the package outright if it will not
    # unpack: nothing goes near the game folder until every file has been checked,
    # so a bad package can never leave a half-applied build.
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
        [System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $Staging)
    } catch {
        Send-Line $S ("ERR could not unpack the package: " + $_.Exception.Message); return
    }

    # verify before touching anything
    $manifest = Join-Path $Staging 'manifest.sha256'
    if (-not (Test-Path -LiteralPath $manifest)) { Send-Line $S "ERR no manifest in the package"; return }
    $bad = 0
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line -split '\s+', 2
        $want = $parts[0]; $rel = $parts[1]
        $f = Join-Path $Staging $rel
        if (-not (Test-Path -LiteralPath $f)) { Write-Own "missing in package: $rel"; $bad++; continue }
        $got = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash
        if ($got -ne $want) { Write-Own "hash mismatch: $rel"; $bad++ }
    }
    if ($bad -gt 0) { Send-Line $S "ERR $bad file(s) failed verification -- nothing applied"; return }

    # stop the game so the exe is not locked
    $wasRunning = Test-GameRunning
    if ($wasRunning) { Invoke-Stop | Out-Null }

    $count = 0
    Get-ChildItem -LiteralPath $Staging -Recurse -File | ForEach-Object {
        if ($_.FullName -eq $zipPath -or $_.Name -eq 'manifest.sha256') { return }
        $rel  = $_.FullName.Substring($Staging.Length).TrimStart('\')
        $dest = Join-Path $Root $rel
        $dir  = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $dir)) { $null = New-Item -ItemType Directory -Path $dir -Force }
        Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
        $count++
    }
    Remove-Item -LiteralPath $Staging -Recurse -Force -ErrorAction SilentlyContinue

    # moving to a new build invalidates the old log -- keep it, but start a fresh one
    if (Test-Path -LiteralPath $LogFile) {
        Move-Item -LiteralPath $LogFile -Destination ($LogFile + '.prev') -Force
    }

    $stamp = Get-BuildStamp
    Write-Own ("sync: applied {0} file(s); build is now {1}" -f $count, $stamp)

    if ($wasRunning -and $null -ne $script:LastArgs) {
        Write-Own ("sync: it was running -- restarting it on the new build ({0})" -f $script:LastArgs)
        Invoke-Start $script:LastArgs | Out-Null
        Send-Line $S ("OK applied {0} file(s); build {1}; RESTARTED it as: {2}" -f $count, $stamp, $script:LastArgs)
    } else {
        Send-Line $S ("OK applied {0} file(s); build {1}" -f $count, $stamp)
    }
}

function Read-Line {
    param([System.IO.Stream] $S)
    $sb = New-Object System.Text.StringBuilder
    while ($true) {
        $b = $S.ReadByte()
        if ($b -lt 0) { throw 'peer closed' }
        if ($b -eq 10) { break }
        if ($b -ne 13) { [void]$sb.Append([char]$b) }
    }
    return $sb.ToString()
}

function Invoke-Start {
    param([string] $ArgsLine)
    if (-not (Test-Path -LiteralPath $Exe)) { return "ERR no game exe in $Root" }
    if (Test-GameRunning) { Invoke-Stop | Out-Null }

    $argv = @()
    if (-not [string]::IsNullOrWhiteSpace($ArgsLine)) { $argv = $ArgsLine -split '\s+' }

    Write-Own ("start: {0} {1}" -f $Exe, ($argv -join ' '))
    $script:LastArgs = $ArgsLine
    $script:Game = Start-Process -FilePath $Exe -ArgumentList $argv -WorkingDirectory $Root -PassThru
    return ("OK started (pid {0}) on build {1}" -f $script:Game.Id, (Get-BuildStamp))
}

function Invoke-Stop {
    $p = Get-Process -Name 'REDRIVER2_dev' -ErrorAction SilentlyContinue
    if ($null -eq $p) { $script:Game = $null; return 'OK nothing was running' }
    foreach ($proc in $p) { try { $proc.CloseMainWindow() | Out-Null } catch { } }
    Start-Sleep -Milliseconds 800
    $p = Get-Process -Name 'REDRIVER2_dev' -ErrorAction SilentlyContinue
    foreach ($proc in $p) { try { Stop-Process -Id $proc.Id -Force } catch { } }
    $script:Game = $null
    Write-Own 'stop: closed the game'
    return 'OK stopped'
}

function Invoke-Log {
    param([System.IO.Stream] $S)
    if (-not (Test-Path -LiteralPath $LogFile)) { Send-Line $S 'ERR no log yet'; return }

    # Share the file: the game holds JERICHO.log open for append WHILE it runs, and
    # pulling a live log is the whole point -- ReadAllBytes would just fail with
    # "being used by another process" exactly when the log matters most.
    $bytes = New-Object byte[] 0
    $fs = [System.IO.File]::Open($LogFile, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read,
                                 [System.IO.FileShare]::ReadWrite)
    try {
        $total = $fs.Length
        $start = [Math]::Max(0, $total - $LogTailBytes)
        $want  = [int]($total - $start)
        $bytes = New-Object byte[] $want
        $fs.Position = $start
        $got = 0
        while ($got -lt $want) {
            $n = $fs.Read($bytes, $got, $want - $got)
            if ($n -le 0) { break }
            $got += $n
        }
        if ($got -lt $want) { $bytes = $bytes[0..([Math]::Max(0, $got - 1))] }
    } finally {
        $fs.Dispose()
    }

    $dump = ''
    if (Test-Path -LiteralPath $DmpFile) { $dump = ' + JERICHO.dmp present' }
    Send-Line $S ("OK {0}{1}" -f $bytes.Length, $dump)
    Send-Bytes $S $bytes
    Write-Own ("log: sent {0:N0} bytes (of {1:N0})" -f $bytes.Length, $total)
}

function Invoke-Status {
    $h = Get-Hashes
    $obj = [ordered]@{
        build    = Get-BuildStamp
        running  = (Test-GameRunning)
        args     = $script:LastArgs
        logBytes = $(if (Test-Path -LiteralPath $LogFile) { (Get-Item -LiteralPath $LogFile).Length } else { 0 })
        dump     = (Test-Path -LiteralPath $DmpFile)
        files    = $h
    }
    return 'OK ' + ($obj | ConvertTo-Json -Compress -Depth 4)
}

function Invoke-Command {
    # Wire format:  <command> <token> [rest]\n   (the token is the SECOND field)
    param([System.IO.Stream] $S, [string] $Line)
    $parts = $Line -split '\s+', 3
    if ($parts.Count -lt 2) { Send-Line $S 'ERR malformed'; return $false }
    if ($parts[1] -ne $Token) { Write-Own 'rejected: bad token'; Send-Line $S 'ERR bad token'; return $false }

    $cmd  = $parts[0]
    $rest = if ($parts.Count -ge 3) { $parts[2] } else { '' }

    switch ($cmd) {
        'ping'   { Send-Line $S ("OK pong build {0}" -f (Get-BuildStamp)) }
        'status' { Send-Line $S (Invoke-Status) }
        'sync'   { Invoke-Sync $S }
        'start'  { Send-Line $S (Invoke-Start $rest) }
        'stop'   { Send-Line $S (Invoke-Stop) }
        'log'    { Invoke-Log $S }
        'quit'   { Send-Line $S 'OK bye'; $script:WantQuit = $true }
        default  { Send-Line $S ("ERR unknown command '$cmd'") }
    }
}

# ------------------------------------------------------------------ resident loop
Write-Own ("agent up: port {0}, root {1}, build {2}" -f $Port, $Root, (Get-BuildStamp))

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $Port)
$listener.Start()
Write-Host ''
Write-Host "  mp agent listening on port $Port -- leave this window open." -ForegroundColor Green
Write-Host "  It will update and relaunch the game by itself when a new build arrives."
Write-Host ''

$quit = $false
while (-not $script:WantQuit) {
    try {
        $client = $listener.AcceptTcpClient()
    } catch {
        Write-Own ("accept failed: {0}" -f $_.Exception.Message)
        continue
    }

    $stream = $client.GetStream()
    $stream.ReadTimeout = 120000

    try {
        $line = Read-Line $stream
        if ($null -ne $line) {
            Write-Own ("<- {0}" -f $line)
            Invoke-Command $stream $line | Out-Null
        }
    } catch {
        Write-Own ("command failed: {0}" -f $_.Exception.Message)
        try { Send-Line $stream ("ERR " + $_.Exception.Message) } catch { }
    } finally {
        try { $client.Close() } catch { }
    }
}

$listener.Stop()
Write-Own 'agent down'
