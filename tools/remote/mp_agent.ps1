# mp_agent.ps1 -- the "leave it running" half of a two-machine test rig.
#
# START IT ONCE on the other PC (START_AGENT.bat does it for you) and leave the
# window open. From then on that machine is hands-free: this script stays resident,
# takes a small fixed set of commands, and -- the point of it -- INSTALLS A NEW
# BUILD AND RELAUNCHES THE GAME when asked to:
#
#   update [tag] -> install a build PUBLISHED AS A GITHUB RELEASE: the rolling
#            'alpha' pre-release by default, or the release tag given. The agent
#            downloads it itself over HTTPS, checks its SHA256 against the digest
#            GitHub publishes for that asset, installs it atomically, and if a game
#            was running, starts it again with the SAME arguments.
#   rollback -> put back the build the last update replaced
#   start -> launch the game (host or join) on the current build
#   stop  -> close the game
#   log   -> send its log back (and say whether a crash dump exists)
#   dump  -> send its crash dump back, if there is one
#   status-> build stamp, installed release, whether the game is up, log/dump presence
#   ping  -> are you there
#   quit  -> stop being resident
#
# There is deliberately NO way to push files to this machine. The old `sync` took a
# zip over the socket and installed it, checked against a "manifest" that travelled
# INSIDE the same zip -- so anyone who could reach the port and knew the (published)
# default token could replace the exe. A command now carries at most a release TAG;
# the bytes always come from GitHub and are checked against a digest that does not
# travel with them.
#
# One-shot use, no listener (run it on the machine itself):
#     powershell -ExecutionPolicy Bypass -File mp_agent.ps1 -InstallRelease [-Tag v0.9.0]
#     powershell -ExecutionPolicy Bypass -File mp_agent.ps1 -Rollback
#
# What protects this machine:
#   * The listener binds to 127.0.0.1 unless -Bind names one of this PC's LAN
#     addresses. It refuses to listen on every interface.
#   * The token is random: generated on the first run and kept in
#     mp_agent.config.json next to the game (never committed). The old published
#     default 'jericho-mp' is refused. The token is never written to mp_agent.log.
#   * Only the Release_dev Windows build (JERICHO_Release_dev_win64.zip, which
#     carries JERICHO_dev.exe) is ever installed. The plain Release assets that CI
#     publishes beside it are not considered real releases yet and are refused, by
#     name and again by what the archive contains.
#   * A build is only ever taken from the releases of -Repo. The trust root is
#     GitHub's TLS and that repository's release permissions: whoever can publish a
#     release there can ship code here. (Signing the archives with a pinned key
#     would remove even that dependency; it is not done yet.)
#
# Nothing here is installed: PowerShell is part of Windows, and the script only
# writes inside the game folder it is pointed at.

[CmdletBinding()]
param(
    [int]    $Port  = 1401,
    [string] $Token = '',              # '' = the token in mp_agent.config.json (made on first run)
    [string] $Bind  = '127.0.0.1',     # listen address; pass this PC's LAN address to accept a peer
    [string] $Root  = '',              # defaults to the folder this script sits in
    [string] $Repo  = 'Njurse/JERICHO',                  # owner/name the releases come from
    [string] $Tag   = 'alpha',                           # the release `update` installs by default
    [string] $Asset = 'JERICHO_Release_dev_win64.zip',   # the ONLY asset accepted: the Release_dev build
    [switch] $InstallRelease,          # one-shot: install -Tag, then exit (no listener)
    [switch] $Rollback,                # one-shot: restore the previous build, then exit
    [switch] $Force,                   # reinstall even when that exact archive is already installed
    [int]    $LogTailBytes = 400000    # how much of JERICHO.log to send back
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Invoke-WebRequest's progress bar makes a download in Windows PowerShell 5.1 many
# times slower; nobody watches it anyway.
$ProgressPreference = 'SilentlyContinue'

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent $MyInvocation.MyCommand.Path
}

# Tolerate a trailing separator or a stray quote. %~dp0 (what the launcher passes)
# ends in a backslash, and a trailing backslash before the closing quote makes
# PowerShell treat the quote as escaped -- so the path arrives as
# C:\...\Release_dev" and Resolve-Path fails with "Illegal characters in path".
$Root = $Root.TrimEnd('\', '/', '"')
$Root = (Resolve-Path -LiteralPath $Root).Path

$Exe        = Join-Path $Root 'JERICHO_dev.exe'
$LogFile    = Join-Path $Root 'JERICHO.log'
$DmpFile    = Join-Path $Root 'JERICHO.dmp'
$OwnLog     = Join-Path $Root 'mp_agent.log'
$ConfigFile = Join-Path $Root 'mp_agent.config.json'      # the token (local only, gitignored)
$StateFile  = 'mp_agent.installed.json'                   # which release is installed (rides with the build)
$Staging    = Join-Path $Root '_mp_staging'               # downloads + unpacking, never the live tree
$Previous   = Join-Path $Root '_mp_previous'              # what the last install replaced (rollback)

# What an install may replace, relative to $Root: the exe and JERICHO/ (the set the
# old push sync used), the runtime DLLs, and the exe's .pdb/.map, which crash triage
# needs BESIDE the exe (tools/dmp_fault.py + map_lookup.py). Deliberately NOT the
# DRIVER2/ game data and NOT config.ini (the player's settings). VERSION.txt and the
# state file are written by the install itself.
$InstallSet = @('JERICHO_dev.exe', 'JERICHO_dev.pdb', 'JERICHO_dev.map', 'SDL2.dll', 'OpenAL32.dll', 'JERICHO')
$SwapSet    = $InstallSet + @('VERSION.txt', $StateFile)

# Names the wire and the command line may carry. A tag is a NAME, never a path or a
# URL, and the repository is owner/name -- so neither can point a download elsewhere.
$TagPattern   = '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$'
$RepoPattern  = '^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$'

# The one asset this agent installs. build.yml publishes Release AND Release_dev
# archives for each platform; only the Release_dev Windows build (JERICHO_dev.exe)
# counts for now -- the plain Release ones are not treated as real releases yet,
# and their exe (JERICHO.exe) is not what this agent starts or stops anyway.
$DevAsset   = 'JERICHO_Release_dev_win64.zip'
$DevExe     = 'JERICHO_dev.exe'
$NonDevExe  = 'JERICHO.exe'     # the plain Release exe: an archive carrying it is refused

$script:Game      = $null   # the running game process, or $null
$script:LastArgs  = $null   # its arguments, so an update can put it back exactly
# A plain flag, NOT the command's return value: every helper here emits output of
# its own ("OK stopped" and friends) and a `$quit = Invoke-Command ...` reads that
# as "yes, quit" -- which silently shut the agent down on the first sync.
$script:WantQuit  = $false
$script:AgentToken = $null   # the live token (resolved at start; NOT the -Token parameter, which shares the script scope)

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

function Get-InstalledRelease {
    $p = Join-Path $Root $StateFile
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { return (Get-Content -LiteralPath $p -Raw | ConvertFrom-Json) } catch { return $null }
}

function Get-OurGames {
    # Games running THIS agent's exe -- not every process on the machine that happens to
    # be called JERICHO_dev. Matching the name alone reaches a session started from
    # somewhere else: a second install, another agent's seat, or a hand-started copy in
    # a different folder. On a machine somebody is also working on, that is their
    # session, and a remote command must not be able to close it. A game the user
    # double-clicked from THIS folder still matches, which is the case the name test
    # was there for.
    $ours = @()

    foreach ($p in @(Get-Process -Name 'JERICHO_dev' -ErrorAction SilentlyContinue)) {
        try {
            if ($p.Path -and ($p.Path -ieq $Exe)) { $ours += $p }
        } catch {
            # .Path is denied for a process we cannot open: leave it alone, always
        }
    }

    return $ours
}

function Test-GameRunning {
    if ($null -ne $script:Game) {
        try { if (-not $script:Game.HasExited) { return $true } } catch { }
        $script:Game = $null
    }
    # also notice a game the user started by hand (double-clicking PLAY_*.bat) from
    # THIS folder -- see Get-OurGames for why the exe path, not the process name.
    # @(...) because a function that returns an empty array hands back $null, and
    # $null.Count is an error under Set-StrictMode.
    if (@(Get-OurGames).Count -gt 0) { return $true }
    return $false
}

# ------------------------------------------------------------------ token + config

function Get-AgentConfig {
    if (-not (Test-Path -LiteralPath $ConfigFile)) { return $null }
    try {
        return (Get-Content -LiteralPath $ConfigFile -Raw | ConvertFrom-Json)
    } catch {
        throw "$ConfigFile is not valid JSON -- fix it, or delete it to get a new token"
    }
}

function Get-ConfigValue {
    param($Config, [string] $Name)
    if ($null -eq $Config) { return $null }
    $p = $Config.PSObject.Properties[$Name]
    if ($null -eq $p) { return $null }
    return [string]$p.Value
}

function New-AgentToken {
    $bytes = New-Object byte[] 24
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Resolve-AgentToken {
    # -Token wins, then the stored one; with neither, make one and store it. The
    # token is printed to THIS console once (the person at this PC copies it to the
    # machine running mp_remote.py) and is never written to mp_agent.log.
    $cfg = Get-AgentConfig
    $t = $Token
    $made = $false

    if ([string]::IsNullOrWhiteSpace($t)) { $t = Get-ConfigValue $cfg 'token' }

    if ([string]::IsNullOrWhiteSpace($t)) {
        $t = New-AgentToken
        $made = $true
        $obj = [ordered]@{ token = $t }
        $repoValue = Get-ConfigValue $cfg 'repo'
        if (-not [string]::IsNullOrWhiteSpace($repoValue)) { $obj['repo'] = $repoValue }
        Set-Content -LiteralPath $ConfigFile -Value ($obj | ConvertTo-Json) -Encoding ASCII
    }

    if ($t -eq 'jericho-mp') {
        throw "the token 'jericho-mp' is the old published default and is refused. Drop it (from -Token or mp_agent.config.json) to have a random one generated, or pass your own."
    }
    if ($t.Length -lt 16 -or $t -match '\s') {
        throw 'the token must be at least 16 characters with no whitespace'
    }

    if ($made) {
        Write-Host ''
        Write-Host '  A new agent token was generated and saved in mp_agent.config.json:' -ForegroundColor Yellow
        Write-Host "      $t" -ForegroundColor Yellow
        Write-Host '  Pass it to mp_remote.py (--token, or the MP_AGENT_TOKEN environment variable).'
        Write-Host ''
    }

    return $t
}

function Test-Token {
    # Fixed-time compare: how long a rejection takes says nothing about how much of
    # the guess was right.
    param([string] $Given)
    $a = [System.Text.Encoding]::UTF8.GetBytes([string]$Given)
    $b = [System.Text.Encoding]::UTF8.GetBytes($script:AgentToken)
    $diff = $a.Length -bxor $b.Length
    for ($i = 0; $i -lt $b.Length; $i++) {
        $x = 0
        if ($i -lt $a.Length) { $x = $a[$i] }
        $diff = $diff -bor ($x -bxor $b[$i])
    }
    return ($diff -eq 0)
}

function Get-RedactedLine {
    # What the log may say about a command line: everything but the token.
    param([string] $Line)
    # The token belongs in the second word, but a client that sends it anywhere
    # else must not get it written down either.
    if ($script:AgentToken) { $Line = $Line.Replace($script:AgentToken, '***') }
    $parts = $Line -split '\s+', 3
    if ($parts.Count -lt 2) { return $parts[0] }
    $parts[1] = '***'
    return ($parts -join ' ')
}

# ------------------------------------------------------------------ release install

function Enable-Tls12 {
    # Windows PowerShell 5.1 on an older .NET can still default to TLS 1.0, which
    # GitHub refuses. (PowerShell 7 ignores this setting; it is harmless there.)
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch { }
}

function Invoke-GitHubApi {
    param([string] $Path)
    $headers = @{
        'Accept'               = 'application/vnd.github+json'
        'User-Agent'           = 'jericho-mp-agent'
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    return Invoke-RestMethod -Uri ('https://api.github.com/' + $Path) -Headers $headers -UseBasicParsing -TimeoutSec 60
}

function Save-Download {
    param([string] $Url, [string] $OutFile)
    # Only from this repository's release downloads, only over HTTPS. GitHub then
    # redirects to its own HTTPS asset host.
    $prefix = 'https://github.com/{0}/releases/download/' -f $Repo
    if (-not $Url.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing a download that is not a release asset of ${Repo}: $Url"
    }
    $null = Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing -TimeoutSec 900 -Headers @{ 'User-Agent' = 'jericho-mp-agent' }
}

function Get-ExpectedSha256 {
    # The digest to check the archive against -- obtained SEPARATELY from it:
    #   1. GitHub's own `digest` for the asset (computed by GitHub at upload);
    #   2. otherwise a SHA256SUMS asset published beside the archives (if any).
    # Never a manifest inside the archive: whoever made the archive made that too.
    # Returns @{ Hash; Source }, or $null when there is nothing to check against.
    param($Release, $AssetObj)

    $d = $AssetObj.PSObject.Properties['digest']
    if ($null -ne $d -and $d.Value -is [string] -and $d.Value -match '^sha256:([0-9a-fA-F]{64})$') {
        return @{ Hash = $Matches[1].ToUpperInvariant(); Source = 'GitHub asset digest' }
    }

    $sums = @($Release.assets | Where-Object { $_.name -eq 'SHA256SUMS' })
    if ($sums.Count -ne 1) { return $null }

    $sumsPath = Join-Path $Staging 'SHA256SUMS'
    Save-Download $sums[0].browser_download_url $sumsPath
    foreach ($line in Get-Content -LiteralPath $sumsPath) {
        if ($line -match '^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$' -and $Matches[2] -eq $AssetObj.name) {
            return @{ Hash = $Matches[1].ToUpperInvariant(); Source = 'SHA256SUMS release asset' }
        }
    }
    return $null
}

function Expand-ZipSafely {
    # ExtractToDirectory, but every entry is checked to land INSIDE $Dest first, so a
    # crafted entry name ("..\..\x") cannot write outside the staging folder.
    param([string] $ZipPath, [string] $Dest)

    Add-Type -AssemblyName System.IO.Compression -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue

    $null = New-Item -ItemType Directory -Path $Dest -Force
    $destFull = [System.IO.Path]::GetFullPath($Dest).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar

    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($e in $zip.Entries) {
            $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destFull, $e.FullName))
            if (-not $target.StartsWith($destFull, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "archive entry escapes the staging folder: $($e.FullName)"
            }
            if ($e.FullName.EndsWith('/') -or $e.FullName.EndsWith('\')) {
                $null = New-Item -ItemType Directory -Path $target -Force
                continue
            }
            $dir = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $dir)) { $null = New-Item -ItemType Directory -Path $dir -Force }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
        }
    } finally {
        $zip.Dispose()
    }
}

function Find-BuildFolder {
    # The archive keeps the build at its root today; tolerate one wrapping folder.
    param([string] $Unpacked)
    if (Test-Path -LiteralPath (Join-Path $Unpacked $DevExe)) { return $Unpacked }
    $dirs = @(Get-ChildItem -LiteralPath $Unpacked -Directory)
    if ($dirs.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $dirs[0].FullName $DevExe))) {
        return $dirs[0].FullName
    }
    return $null
}

function Switch-Build {
    # Swap the items in $NewDir into $Root. Each Move-Item is a rename on the same
    # volume; the replaced items are collected in a fresh folder that becomes the
    # rollback point ONLY once every move succeeded. If any move fails, everything
    # moved so far is put back, so a failed install never leaves a half-applied
    # build. Only items $NewDir provides are touched, plus $TakeOut: items to move
    # out WITHOUT a replacement (a rollback taking away what the install added).
    # Items that arrive with nothing to replace are listed in '.added' in the
    # rollback point, so the rollback knows to take them away again.
    param([string] $NewDir, [string[]] $TakeOut = @())

    $prevNext = Join-Path $Root '_mp_previous.next'
    if (Test-Path -LiteralPath $prevNext) { Remove-Item -LiteralPath $prevNext -Recurse -Force }
    $null = New-Item -ItemType Directory -Path $prevNext -Force

    $items    = @($SwapSet | Where-Object { Test-Path -LiteralPath (Join-Path $NewDir $_) })
    $out      = @($SwapSet | Where-Object { ($items -contains $_) -or ($TakeOut -contains $_) })
    $movedOut = New-Object System.Collections.Generic.List[string]
    $movedIn  = New-Object System.Collections.Generic.List[string]

    try {
        foreach ($item in $out) {
            $cur = Join-Path $Root $item
            if (Test-Path -LiteralPath $cur) {
                Move-Item -LiteralPath $cur -Destination (Join-Path $prevNext $item)
                $movedOut.Add($item)
            }
        }
        foreach ($item in $items) {
            Move-Item -LiteralPath (Join-Path $NewDir $item) -Destination (Join-Path $Root $item)
            $movedIn.Add($item)
        }
    } catch {
        $err = $_.Exception.Message
        foreach ($item in $movedIn) {
            try { Move-Item -LiteralPath (Join-Path $Root $item) -Destination (Join-Path $NewDir $item) } catch { }
        }
        foreach ($item in $movedOut) {
            try { Move-Item -LiteralPath (Join-Path $prevNext $item) -Destination (Join-Path $Root $item) } catch { }
        }
        # an empty leftover is noise; one that still holds files is the only copy of
        # something that could not be put back, so it stays for a human to see
        if (@(Get-ChildItem -LiteralPath $prevNext -Force).Count -eq 0) {
            Remove-Item -LiteralPath $prevNext -Recurse -Force -ErrorAction SilentlyContinue
        }
        throw "the swap failed and was undone: $err"
    }

    $added = @($items | Where-Object { -not $movedOut.Contains($_) })
    if ($added.Count -gt 0) { Set-Content -LiteralPath (Join-Path $prevNext '.added') -Value $added -Encoding ASCII }

    # Done. What it replaced is the new rollback point. ($NewDir may BE $Previous
    # when rolling back; it is now just the empty shells of what moved out.)
    if (Test-Path -LiteralPath $NewDir) { Remove-Item -LiteralPath $NewDir -Recurse -Force }
    if (Test-Path -LiteralPath $Previous) { Remove-Item -LiteralPath $Previous -Recurse -Force }
    Move-Item -LiteralPath $prevNext -Destination $Previous
    return $items.Count
}

function Get-BuildCommit {
    # The commit a release was built from, for the build stamp. Informational only.
    # NOT the tag's commit: build.yml re-uploads the rolling 'alpha' assets on every
    # push to main without moving the 'alpha' tag, so the tag names an old commit.
    # The release text does carry the right one ("... from `main` at <sha>").
    param($Release)
    $body = $Release.PSObject.Properties['body']
    if ($null -ne $body -and $body.Value -is [string] -and $body.Value -match '\b([0-9a-f]{40})\b') {
        return $Matches[1].Substring(0, 7)
    }
    return ''
}

function Format-UtcStamp {
    # ConvertFrom-Json turns ISO dates into DateTime in PowerShell 7 but leaves them
    # strings in 5.1; store one form either way.
    param($Value)
    if ($Value -is [datetime]) { return $Value.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
    return [string]$Value
}

function Install-Release {
    # Returns ONE reply line ("OK ..." / "ERR ..."). Every helper's output is
    # swallowed on purpose -- see $script:WantQuit for why that matters here.
    param([string] $ReleaseTag, [switch] $Reinstall)

    if ($ReleaseTag -notmatch $TagPattern) { return "ERR bad release tag '$ReleaseTag'" }

    $wasRunning = $false
    try {
        Enable-Tls12
        Write-Own ("update: looking up release '{0}' of {1}" -f $ReleaseTag, $Repo)
        $release = Invoke-GitHubApi ('repos/{0}/releases/tags/{1}' -f $Repo, [uri]::EscapeDataString($ReleaseTag))

        $assetObj = @($release.assets | Where-Object { $_.name -eq $Asset })
        if ($assetObj.Count -ne 1) {
            $others = @($release.assets | ForEach-Object { $_.name } | Where-Object { $_ -like '*.zip' })
            $seen = if ($others.Count) { ' (it has: ' + ($others -join ', ') + ')' } else { '' }
            return "ERR release '$ReleaseTag' has no $Asset -- only the Release_dev build is installed, Release assets are refused$seen"
        }
        $assetObj = $assetObj[0]

        if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
        $null = New-Item -ItemType Directory -Path $Staging -Force

        $expected = Get-ExpectedSha256 $release $assetObj
        if ($null -eq $expected) {
            return "ERR release '$ReleaseTag' publishes no digest for $Asset (no asset digest, no SHA256SUMS) -- refusing to install an unverifiable build"
        }

        $installed = Get-InstalledRelease
        if (-not $Reinstall -and $null -ne $installed -and [string]$installed.sha256 -eq $expected.Hash) {
            return ("OK already on {0}; nothing to do" -f (Get-BuildStamp))
        }

        $zipPath = Join-Path $Staging $Asset
        Write-Own ("update: downloading {0} ({1:N1} MB)" -f $Asset, ($assetObj.size / 1MB))
        Save-Download $assetObj.browser_download_url $zipPath

        $got = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
        if ($got -ne $expected.Hash) {
            Write-Own ("update: SHA256 MISMATCH for {0}: got {1}, {2} says {3}" -f $Asset, $got, $expected.Source, $expected.Hash)
            return "ERR SHA256 of the download does not match the $($expected.Source) -- nothing installed"
        }
        Write-Own ("update: SHA256 verified against the {0}" -f $expected.Source)

        $unpacked = Join-Path $Staging 'unpacked'
        Expand-ZipSafely $zipPath $unpacked
        $build = Find-BuildFolder $unpacked
        if ($null -eq $build -or -not (Test-Path -LiteralPath (Join-Path $build 'JERICHO') -PathType Container)) {
            return "ERR $Asset does not look like a game build (no $DevExe + JERICHO\) -- nothing installed"
        }
        if (Test-Path -LiteralPath (Join-Path $build $NonDevExe)) {
            # A Release_dev archive never carries the plain Release exe; one that does
            # is mislabelled, and is refused rather than half-installed.
            return "ERR $Asset carries $NonDevExe (a Release build, not Release_dev) -- nothing installed"
        }

        # Stage ONLY the install set, so nothing else in the archive (DRIVER2\,
        # config.ini, ...) can reach the live folder.
        $new = Join-Path $Staging 'new'
        $null = New-Item -ItemType Directory -Path $new -Force
        foreach ($item in $InstallSet) {
            $src = Join-Path $build $item
            if (Test-Path -LiteralPath $src) { Move-Item -LiteralPath $src -Destination (Join-Path $new $item) }
        }

        # The player's JERICHO\CONFIG files stay theirs: every one that exists here
        # is carried into the new tree over the release's copy. New ones still land.
        $liveConfig = Join-Path (Join-Path $Root 'JERICHO') 'CONFIG'
        if (Test-Path -LiteralPath $liveConfig -PathType Container) {
            $newConfig = Join-Path (Join-Path $new 'JERICHO') 'CONFIG'
            $null = New-Item -ItemType Directory -Path $newConfig -Force
            $base = (Resolve-Path -LiteralPath $liveConfig).Path.TrimEnd('\', '/')
            Get-ChildItem -LiteralPath $liveConfig -Recurse -File | ForEach-Object {
                $rel  = $_.FullName.Substring($base.Length).TrimStart('\', '/')
                $dest = Join-Path $newConfig $rel
                $dir  = Split-Path -Parent $dest
                if (-not (Test-Path -LiteralPath $dir)) { $null = New-Item -ItemType Directory -Path $dir -Force }
                Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
            }
        }

        $commit = Get-BuildCommit $release
        $stamp = $ReleaseTag
        if ($commit -ne '') { $stamp = '{0} ({1})' -f $ReleaseTag, $commit }
        $stamp = '{0} sha256:{1}' -f $stamp, $expected.Hash.Substring(0, 12).ToLowerInvariant()
        Set-Content -LiteralPath (Join-Path $new 'VERSION.txt') -Value $stamp -Encoding ASCII

        $state = [ordered]@{
            repo           = $Repo
            tag            = $ReleaseTag
            commit         = $commit
            asset          = $Asset
            sha256         = $expected.Hash
            verifiedBy     = $expected.Source
            assetUpdatedAt = Format-UtcStamp $assetObj.updated_at
            installedAt    = (Get-Date).ToString('o')
        }
        Set-Content -LiteralPath (Join-Path $new $StateFile) -Value ($state | ConvertTo-Json) -Encoding ASCII

        # stop the game so the exe and the module DLLs are not locked
        $wasRunning = Test-GameRunning
        if ($wasRunning) { Invoke-Stop | Out-Null }

        $count = Switch-Build $new

        # moving to a new build invalidates the old log -- keep it, but start a fresh one
        if (Test-Path -LiteralPath $LogFile) {
            Move-Item -LiteralPath $LogFile -Destination ($LogFile + '.prev') -Force
        }

        Write-Own ("update: installed {0} item(s); build is now {1}" -f $count, (Get-BuildStamp))
        return (Complete-Swap $wasRunning ("installed {0}" -f (Get-BuildStamp)))
    } catch {
        $err = $_.Exception.Message
        Write-Own ("update: failed: {0}" -f $err)
        # the swap undoes itself on failure, so this is still the old build: if the
        # game was stopped for the install, put it back
        if ($wasRunning -and $null -ne $script:LastArgs) { Invoke-Start $script:LastArgs | Out-Null }
        return ("ERR update failed: " + $err)
    } finally {
        # whatever happened, the download and the unpacked tree go
        if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Invoke-Rollback {
    if (-not (Test-Path -LiteralPath $Previous -PathType Container)) { return 'ERR there is no previous build to roll back to' }
    $wasRunning = $false
    try {
        $wasRunning = Test-GameRunning
        if ($wasRunning) { Invoke-Stop | Out-Null }
        # Swapping the rollback point in makes the current build the new rollback
        # point, so a second rollback undoes the first.
        $takeOut = @()
        $marker = Join-Path $Previous '.added'
        if (Test-Path -LiteralPath $marker) {
            $takeOut = @(Get-Content -LiteralPath $marker | Where-Object { $SwapSet -contains $_ })
        }
        $count = Switch-Build $Previous $takeOut
        Write-Own ("rollback: restored {0} item(s); build is now {1}" -f $count, (Get-BuildStamp))
        return (Complete-Swap $wasRunning ("rolled back to {0}" -f (Get-BuildStamp)))
    } catch {
        $err = $_.Exception.Message
        Write-Own ("rollback: failed: {0}" -f $err)
        if ($wasRunning -and $null -ne $script:LastArgs) { Invoke-Start $script:LastArgs | Out-Null }
        return ("ERR rollback failed: " + $err)
    }
}

function Complete-Swap {
    # After a build changed under a running game: start it again exactly as it was.
    param([bool] $WasRunning, [string] $What)
    if ($WasRunning -and $null -ne $script:LastArgs) {
        Write-Own ("it was running -- restarting it on the new build ({0})" -f $script:LastArgs)
        Invoke-Start $script:LastArgs | Out-Null
        return ("OK {0}; RESTARTED it as: {1}" -f $What, $script:LastArgs)
    }
    return ("OK {0}" -f $What)
}

# ------------------------------------------------------------------ the wire

function Send-Line {
    # Raw stream, NOT a StreamWriter: a StreamReader/Writer buffers. Everything here
    # reads and writes bytes.
    param([System.IO.Stream] $S, [string] $Text)
    $b = [System.Text.Encoding]::ASCII.GetBytes($Text + "`n")
    $S.Write($b, 0, $b.Length); $S.Flush()
}

function Send-Bytes {
    param([System.IO.Stream] $S, [byte[]] $Data)
    $S.Write($Data, 0, $Data.Length); $S.Flush()
}

function Read-Line {
    # One command line, capped: a peer that never sends a newline cannot make the
    # agent buffer without end.
    param([System.IO.Stream] $S)
    $sb = New-Object System.Text.StringBuilder
    while ($true) {
        $b = $S.ReadByte()
        if ($b -lt 0) { throw 'peer closed' }
        if ($b -eq 10) { break }
        if ($b -ne 13) { [void]$sb.Append([char]$b) }
        if ($sb.Length -gt 1024) { throw 'command line too long' }
    }
    return $sb.ToString()
}

function Invoke-Start {
    param([string] $ArgsLine)
    if (-not (Test-Path -LiteralPath $Exe)) { return "ERR no game exe in $Root" }
    if (Test-GameRunning) { Invoke-Stop | Out-Null }

    # `+K=V` tokens are ENVIRONMENT for the game, not arguments: a remote seat needs its
    # own MP_BOT (the rig drives the client to chase and the host to flee), and there is
    # deliberately no way to push a config file to this machine. `+` cannot begin one of
    # the engine's own arguments, so the two cannot be confused.
    $argv = @()
    $envPairs = @{}

    if (-not [string]::IsNullOrWhiteSpace($ArgsLine)) {
        foreach ($tok in @($ArgsLine -split '\s+' | Where-Object { $_ })) {
            if ($tok.StartsWith('+') -and $tok.Contains('=')) {
                $kv = $tok.Substring(1).Split('=', 2)
                $envPairs[$kv[0]] = $kv[1]
            } else {
                $argv += $tok
            }
        }
    }

    $envNote = if ($envPairs.Count -gt 0) {
        '  [env: ' + (($envPairs.GetEnumerator() |
                      ForEach-Object { $_.Key + '=' + $_.Value }) -join ' ') + ']'
    } else { '' }

    Write-Own ("start: {0} {1}{2}" -f $Exe, ($argv -join ' '), $envNote)
    $script:LastArgs = $ArgsLine

    # -ArgumentList is OMITTED when there is nothing to pass, by SPLATTING the
    # parameters. Windows PowerShell 5.1 validates the parameter and rejects an empty
    # collection outright:
    #   Cannot validate argument on parameter 'ArgumentList'. The argument is null or
    #   empty. Provide an argument that is not null or empty, and then try the command
    #   again.
    # so `start` with no arguments -- a legitimate request; the game then uses its own
    # config.ini and defaults -- failed with that message instead of launching, and so
    # did any restart-after-update where the game had been started with no arguments.
    # Splatting is the only way to leave a parameter out entirely.
    $sp = @{ FilePath = $Exe; WorkingDirectory = $Root; PassThru = $true }
    if ($argv.Count -gt 0) { $sp['ArgumentList'] = $argv }

    # Start-Process has no -Environment on 5.1, and the child inherits THIS process's
    # environment: set them, launch, then put ours back so the next start is not quietly
    # driven by the last one.
    $saved = @{}
    foreach ($k in $envPairs.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        Set-Item -Path "env:$k" -Value $envPairs[$k]
    }

    try {
        $script:Game = Start-Process @sp
    } finally {
        foreach ($k in $saved.Keys) {
            if ($null -eq $saved[$k]) { Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue }
            else { Set-Item -Path "env:$k" -Value $saved[$k] }
        }
    }

    return ("OK started (pid {0}) on build {1}" -f $script:Game.Id, (Get-BuildStamp))
}

function Invoke-Stop {
    $p = @(Get-OurGames)
    if ($p.Count -eq 0) { $script:Game = $null; return 'OK nothing was running' }
    foreach ($proc in $p) { try { $proc.CloseMainWindow() | Out-Null } catch { } }
    Start-Sleep -Milliseconds 800
    foreach ($proc in @(Get-OurGames)) { try { Stop-Process -Id $proc.Id -Force } catch { } }
    $script:Game = $null
    Write-Own ('stop: closed {0} game process(es) from {1}' -f $p.Count, $Root)
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
    if (Test-Path -LiteralPath $DmpFile) {
        # Reported with the dump's LAST WRITE time, because this folder is reused: a dump
        # left by an earlier session would otherwise read as this run's crash. Epoch
        # seconds so the far end can compare it against when its run started.
        $epoch = [int](((Get-Item -LiteralPath $DmpFile).LastWriteTimeUtc -
                        [datetime]'1970-01-01Z').TotalSeconds)
        $dump = " + JERICHO.dmp present t=$epoch"
    }
    Send-Line $S ("OK {0}{1}" -f $bytes.Length, $dump)
    Send-Bytes $S $bytes
    Write-Own ("log: sent {0:N0} bytes (of {1:N0})" -f $bytes.Length, $total)
}

function Invoke-Dump {
    param([System.IO.Stream] $S)

    # A dump is a plain file read -- the game is not involved -- but it is the only
    # way a crash on THIS machine is attributable without walking over to it. The
    # log command can only say that one exists.
    if (-not (Test-Path -LiteralPath $DmpFile)) { Send-Line $S 'ERR no dump'; return }

    $bytes = [System.IO.File]::ReadAllBytes($DmpFile)
    Send-Line $S ("OK {0}" -f $bytes.Length)
    Send-Bytes $S $bytes
    Write-Own ("dump: sent {0:N0} bytes" -f $bytes.Length)
}

function Invoke-Status {
    $obj = [ordered]@{
        build    = Get-BuildStamp
        release  = Get-InstalledRelease
        previous = (Test-Path -LiteralPath $Previous -PathType Container)
        repo     = $Repo
        running  = (Test-GameRunning)
        args     = $script:LastArgs
        logBytes = $(if (Test-Path -LiteralPath $LogFile) { (Get-Item -LiteralPath $LogFile).Length } else { 0 })
        dump     = (Test-Path -LiteralPath $DmpFile)
    }
    return 'OK ' + ($obj | ConvertTo-Json -Compress -Depth 4)
}

function Invoke-Command {
    # Wire format:  <command> <token> [rest]\n   (the token is the SECOND field)
    param([System.IO.Stream] $S, [string] $Line, [string] $From)
    $parts = $Line -split '\s+', 3
    if ($parts.Count -lt 2) { Send-Line $S 'ERR malformed'; return }
    if (-not (Test-Token $parts[1])) {
        Write-Own "rejected: bad token from $From"
        # a second's pause per wrong guess, so guessing costs time
        Start-Sleep -Seconds 1
        Send-Line $S 'ERR bad token'
        return
    }

    $cmd  = $parts[0]
    $rest = if ($parts.Count -ge 3) { $parts[2].Trim() } else { '' }

    switch ($cmd) {
        'ping'     { Send-Line $S ("OK pong build {0}" -f (Get-BuildStamp)) }
        'status'   { Send-Line $S (Invoke-Status) }
        'update'   {
            # The ONLY thing an update carries is a release tag: a name, checked
            # against $TagPattern. The bytes come from GitHub, never from the peer.
            $t = $Tag
            if ($rest -ne '') { $t = $rest }
            Send-Line $S (Install-Release $t -Reinstall:$Force)
        }
        'rollback' { Send-Line $S (Invoke-Rollback) }
        'sync'     {
            # An older mp_remote.py, about to stream a zip after this line. Refuse it
            # without reading a byte of it.
            Write-Own 'rejected: push sync is no longer supported'
            Send-Line $S 'ERR sync (pushing a build) was removed -- use: update [release tag]'
        }
        'start'    { Send-Line $S (Invoke-Start $rest) }
        'stop'     { Send-Line $S (Invoke-Stop) }
        'log'      { Invoke-Log $S }
        'dump'     { Invoke-Dump $S }
        'quit'     { Send-Line $S 'OK bye'; $script:WantQuit = $true }
        default    { Send-Line $S ("ERR unknown command '$cmd'") }
    }
}

# ------------------------------------------------------------------ entry

if ($Repo -notmatch $RepoPattern) {
    throw "-Repo must be owner/name, got '$Repo'"
}
# mp_agent.config.json may name another repository (a fork); an explicit -Repo wins.
if (-not $PSBoundParameters.ContainsKey('Repo')) {
    $cfgRepo = Get-ConfigValue (Get-AgentConfig) 'repo'
    if (-not [string]::IsNullOrWhiteSpace($cfgRepo)) {
        if ($cfgRepo -notmatch $RepoPattern) { throw "mp_agent.config.json: repo must be owner/name, got '$cfgRepo'" }
        $Repo = $cfgRepo
    }
}
if ($Tag -notmatch $TagPattern) { throw "-Tag is not a valid release tag: '$Tag'" }
if ($Asset -ne $DevAsset) {
    throw "only the Release_dev build is installed: -Asset must be $DevAsset (got '$Asset'). The plain Release assets are refused."
}

if ($InstallRelease -or $Rollback) {
    if ($Rollback) { $reply = Invoke-Rollback } else { $reply = Install-Release $Tag -Reinstall:$Force }
    Write-Host $reply
    if ($reply.StartsWith('OK')) { exit 0 }
    exit 1
}

# Every IPv4 address this PC actually has. -Bind is checked against these BEFORE the
# socket is asked for it, because .NET's answer to a non-local address is a bare
#   Exception calling "Start" with "0" argument(s): "The requested address is not
#   valid in its context"
# (WSAEADDRNOTAVAIL) that says nothing about which addresses WOULD have worked. The
# usual cause is an address copied from the OTHER PC, so the message has to list
# this one's.
function Get-LocalIPv4 {
    $addrs = @()

    try {
        $addrs = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
                   Select-Object -ExpandProperty IPAddress)
    } catch {
        # no Get-NetIPAddress (older PowerShell): ask DNS for this host's addresses
        foreach ($ip in [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName())) {
            if ($ip.AddressFamily -eq 'InterNetwork') { $addrs += $ip.ToString() }
        }
    }

    $addrs += '127.0.0.1'

    return @($addrs | Sort-Object -Unique)
}

# A listener that cannot come up must say why in one readable line and stop. `throw`
# here printed a PowerShell stack trace AND the message three times, which is not
# something to hand to whoever is sitting at the other PC.
function Stop-WithMessage {
    param([string] $Message)

    Write-Host ''
    Write-Host ('  ' + $Message) -ForegroundColor Yellow
    Write-Host ''
    exit 1
}

$script:AgentToken = Resolve-AgentToken

$bindAddr = $null
if (-not [System.Net.IPAddress]::TryParse($Bind, [ref]$bindAddr)) {
    Stop-WithMessage "-Bind must be an IP address of this PC, got '$Bind'"
}
if ($bindAddr.Equals([System.Net.IPAddress]::Any) -or $bindAddr.Equals([System.Net.IPAddress]::IPv6Any)) {
    Stop-WithMessage "refusing to listen on every interface ($Bind). Pass this PC's LAN address, e.g. -Bind 192.168.1.20"
}

# An address this PC does not have can never be bound. Say so -- and say what it DOES
# have -- instead of dying inside TcpListener.Start() with WSAEADDRNOTAVAIL.
$local4 = Get-LocalIPv4

if ($Bind -ne '127.0.0.1' -and ($local4 -notcontains $Bind)) {
    # 169.254.x.x is a link-local address Windows makes up when a NIC has no DHCP
    # lease: valid to bind, useless advice, so it is left out of the list shown.
    $shown = @($local4 | Where-Object { $_ -notlike '169.254.*' })

    Stop-WithMessage ("-Bind {0} is not an address on THIS PC, so nothing here can listen on it.
  This PC's IPv4 address(es): {1}
  Use the LAN one (192.168.x.x or 10.x.x.x) to accept the other PC; 127.0.0.1 only
  works for commands from this machine itself." -f $Bind, ($shown -join ', '))
}

$listener = [System.Net.Sockets.TcpListener]::new($bindAddr, $Port)
$listener.Start()

# Logged only once the listener is actually up: a bind that fails must never have
# claimed "agent up" first, which is what made this look like a crash after success.
Write-Own ("agent up: {0}:{1}, root {2}, build {3}, releases from {4}" -f $Bind, $Port, $Root, (Get-BuildStamp), $Repo)

Write-Host ''
Write-Host "  mp agent listening on ${Bind}:$Port -- leave this window open." -ForegroundColor Green
if ($bindAddr.Equals([System.Net.IPAddress]::Loopback)) {
    Write-Host '  Only THIS PC can reach it. To drive it from another PC, restart with'
    Write-Host '  -Bind <this PC''s LAN address>.'
}
Write-Host "  'update' installs a release of $Repo (default '$Tag') and relaunches the game."
Write-Host ''

while (-not $script:WantQuit) {
    try {
        $client = $listener.AcceptTcpClient()
    } catch {
        Write-Own ("accept failed: {0}" -f $_.Exception.Message)
        continue
    }

    $from = [string]$client.Client.RemoteEndPoint
    $stream = $client.GetStream()
    $stream.ReadTimeout = 120000

    try {
        $line = Read-Line $stream
        if ($null -ne $line) {
            # never the token: the log is a file anyone at this PC can read
            Write-Own ("<- {0}  (from {1})" -f (Get-RedactedLine $line), $from)
            Invoke-Command $stream $line $from | Out-Null
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
