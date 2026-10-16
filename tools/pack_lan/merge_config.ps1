# merge_config.ps1 - add the release's config.ini keys the player's file is missing.
#
# update_jericho never REPLACES config.ini: it holds the player's window size, fullscreen,
# binds and options. But a release may also carry a key theirs has never seen (a new render
# or game option), so this appends just those, under their own section, and leaves every
# value the player already has exactly as it is.
#
# Their value always wins: the game's ini reader (utils/ini.c) returns the FIRST match for a
# section+key, so even a key that got added twice could not override one of theirs.
#
# Called as:  set JERICHO_CFG_REL=<release config.ini>
#             set JERICHO_CFG_USR=<install config.ini>
#             powershell -NoProfile -ExecutionPolicy Bypass -File merge_config.ps1

$rel = $env:JERICHO_CFG_REL
$usr = $env:JERICHO_CFG_USR

if (-not (Test-Path -LiteralPath $rel)) { exit 0 }

if (-not (Test-Path -LiteralPath $usr)) {
	Copy-Item -Force -LiteralPath $rel -Destination $usr
	exit 0
}

# Which section+key the player already has.
$have = @{}
$s = ''
foreach ($line in [System.IO.File]::ReadAllLines($usr)) {
	if ($line -match '^\s*\[(.+?)\]') { $s = $matches[1].ToLower(); continue }
	if ($line -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=') {
		$have["$s|" + $matches[1].ToLower()] = $true
	}
}

# Anything the release has and they do not, section by section.
$add = New-Object System.Collections.Generic.List[string]
$s = ''
$opened = ''
foreach ($line in [System.IO.File]::ReadAllLines($rel)) {
	if ($line -match '^\s*\[(.+?)\]') { $s = $matches[1].ToLower(); continue }
	if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=') { continue }

	$k = "$s|" + $matches[1].ToLower()
	if ($have.ContainsKey($k)) { continue }

	$have[$k] = $true
	if ($s -ne $opened) {
		$opened = $s
		$add.Add('')
		$add.Add("[$s]")
	}
	$add.Add($line)
}

if ($add.Count -gt 0) {
	Add-Content -LiteralPath $usr -Value $add
}
