# play_makapix_64.ps1
# -----------------------------------------------------------------------------
# Stress-test helper for p3a.
#
# Pops up a tiny window with two buttons. Each one tells the p3a device
# (default p3a.local) to create and immediately play a 64-channel playset:
#
#   * "makapix_64"    - 64 channels, each a different Makapix Club hashtag.
#   * "giphy_64_test" - 64 channels, each a different Giphy search keyword.
#
# 64 channels is the documented maximum (PS_MAX_CHANNELS), so this exercises the
# device at its design limit.
#
# How it works (see components/http_api):
#   POST /playsets/{name}   { "channels":[...64...], "activate":true }
#       -> creates/overwrites the playset and starts switching to it.
#   GET  /playsets/active   -> polled to confirm the switch landed
#                              (switching==false, last_switch_error=="").
#
# No auth: the device's HTTP API is plain HTTP on the LAN.
#
# Run via the play_makapix_64.cmd launcher (handles -STA), or:
#   powershell -ExecutionPolicy Bypass -STA -File play_makapix_64.ps1
# -----------------------------------------------------------------------------

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# --- 64 Makapix hashtags (one per channel). Tags only, no '#', no spaces. -----
$Hashtags = @(
    'pixelart','landscape','character','nature','space','city','fantasy','retro',
    'game','animal','cat','dog','forest','ocean','mountain','sunset',
    'night','robot','cyberpunk','dungeon','rpg','sprite','portrait','food',
    'flower','tree','sky','star','moon','sun','fire','water',
    'ice','dragon','knight','wizard','ghost','skull','heart','magic',
    'winter','summer','autumn','spring','rain','snow','cloud','island',
    'desert','cave','castle','village','spaceship','planet','galaxy','neon',
    'vaporwave','samurai','ninja','mecha','monster','bird','fish','butterfly'
)

# --- 64 Giphy search keywords (one per channel). ------------------------------
$GiphyKeywords = @(
    'cats','dogs','reaction','dance','happy','sad','lol','wow',
    'fail','win','party','love','fire','money','food','coffee',
    'pizza','space','robot','dinosaur','unicorn','rainbow','explosion','facepalm',
    'thumbsup','applause','confetti','fireworks','ocean','rain','snow','sun',
    'moon','star','flower','nature','city','neon','retro','anime',
    'cartoon','meme','sports','soccer','basketball','gaming','music','guitar',
    'drums','magic','ghost','skull','zombie','alien','ninja','samurai',
    'dragon','cute','baby','puppy','kitten','panda','fox','owl'
)

# ----------------------------- channel builders ------------------------------

function Build-MakapixChannels {
    foreach ($tag in $Hashtags) {
        [ordered]@{
            type = 'hashtag'; name = 'hashtag'; identifier = $tag
            display_name = "#$tag"; weight = 100; offset = 0
        }
    }
}

function Build-GiphyChannels {
    foreach ($kw in $GiphyKeywords) {
        [ordered]@{
            type = 'giphy'; name = 'search'; identifier = $kw
            display_name = $kw; weight = 100; offset = 0
        }
    }
}

# ----------------------------- networking ------------------------------------

function Invoke-PlayPlayset {
    param(
        [string]$HostName,
        [string]$PlaysetName,
        [array] $Channels,
        [scriptblock]$Log
    )

    $base = "http://$HostName"
    & $Log "Target: $base"
    & $Log ("Building playset '{0}' with {1} channels..." -f $PlaysetName, $Channels.Count)

    $json  = ([ordered]@{ channels = @($Channels); activate = $true } | ConvertTo-Json -Depth 5)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $url   = "$base/playsets/$PlaysetName"

    & $Log "POST $url"
    try {
        $resp = Invoke-RestMethod -Uri $url -Method Post -Body $bytes `
                    -ContentType 'application/json' -TimeoutSec 20
    } catch {
        & $Log "ERROR: create/activate request failed: $($_.Exception.Message)"
        if ($_.Exception.Response) {
            try {
                $sr = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
                & $Log ("  body: " + $sr.ReadToEnd())
            } catch {}
        }
        return $false
    }

    if (-not $resp.ok) {
        & $Log ("ERROR: device rejected playset: {0} ({1})" -f $resp.error, $resp.code)
        return $false
    }
    & $Log ("OK: saved={0} activated={1}" -f $resp.data.saved, $resp.data.activated)

    # Activation is async; poll /playsets/active to confirm the switch landed.
    & $Log "Confirming switch (polling /playsets/active)..."
    $activeUrl = "$base/playsets/active"
    for ($i = 1; $i -le 15; $i++) {
        Start-Sleep -Milliseconds 1000
        try {
            $a = Invoke-RestMethod -Uri $activeUrl -Method Get -TimeoutSec 10
        } catch {
            & $Log ("  poll {0}: request failed: {1}" -f $i, $_.Exception.Message)
            continue
        }
        $d = $a.data
        if ($d.makapix_registration_required) {
            & $Log "  WARNING: device reports makapix_registration_required=true."
            & $Log "           Makapix channels won't refresh until the device is registered."
        }
        if ($d.last_switch_error) {
            & $Log ("  ERROR: last_switch_error = {0}" -f $d.last_switch_error)
            return $false
        }
        if (-not $d.switching) {
            & $Log ("SUCCESS: active playset is now '{0}' ({1} channels)." -f $d.name, $d.channel_count)
            return $true
        }
        & $Log ("  poll {0}: still switching..." -f $i)
    }
    & $Log "WARNING: switch did not confirm within timeout (device may still be working)."
    return $false
}

# ------------------------------- GUI -----------------------------------------

$form = New-Object System.Windows.Forms.Form
$form.Text = "p3a stress test - 64-channel playsets"
$form.Size = New-Object System.Drawing.Size(560, 500)
$form.StartPosition = "CenterScreen"

$lblHost = New-Object System.Windows.Forms.Label
$lblHost.Text = "Device host:"
$lblHost.Location = New-Object System.Drawing.Point(12, 15)
$lblHost.AutoSize = $true
$form.Controls.Add($lblHost)

$txtHost = New-Object System.Windows.Forms.TextBox
$txtHost.Text = "p3a.local"
$txtHost.Location = New-Object System.Drawing.Point(90, 12)
$txtHost.Size = New-Object System.Drawing.Size(200, 24)
$form.Controls.Add($txtHost)

$btnMakapix = New-Object System.Windows.Forms.Button
$btnMakapix.Text = "Play makapix_64  (64 hashtag channels)"
$btnMakapix.Location = New-Object System.Drawing.Point(90, 44)
$btnMakapix.Size = New-Object System.Drawing.Size(380, 38)
$form.Controls.Add($btnMakapix)

$btnGiphy = New-Object System.Windows.Forms.Button
$btnGiphy.Text = "Play giphy_64_test  (64 Giphy search channels)"
$btnGiphy.Location = New-Object System.Drawing.Point(90, 88)
$btnGiphy.Size = New-Object System.Drawing.Size(380, 38)
$form.Controls.Add($btnGiphy)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = "Vertical"
$logBox.Location = New-Object System.Drawing.Point(12, 138)
$logBox.Size = New-Object System.Drawing.Size(520, 312)
$logBox.Font = New-Object System.Drawing.Font("Consolas", 9)
$logBox.Anchor = "Top,Bottom,Left,Right"
$form.Controls.Add($logBox)

$appendLog = {
    param($msg)
    $logBox.AppendText($msg + "`r`n")
    [System.Windows.Forms.Application]::DoEvents()
}

# Shared click logic: validate host, run the playset, keep the UI responsive.
$runPlayset = {
    param($PlaysetName, $Channels)
    $btnMakapix.Enabled = $false
    $btnGiphy.Enabled = $false
    $logBox.Clear()
    try {
        $h = $txtHost.Text.Trim()
        if (-not $h) { & $appendLog "Enter a device host first."; return }
        Invoke-PlayPlayset -HostName $h -PlaysetName $PlaysetName -Channels $Channels -Log $appendLog | Out-Null
    } catch {
        & $appendLog "UNEXPECTED ERROR: $($_.Exception.Message)"
    } finally {
        & $appendLog "Done."
        $btnMakapix.Enabled = $true
        $btnGiphy.Enabled = $true
    }
}

$btnMakapix.Add_Click({ & $runPlayset 'makapix_64'    (Build-MakapixChannels) })
$btnGiphy.Add_Click(  { & $runPlayset 'giphy_64_test' (Build-GiphyChannels) })

[void]$form.ShowDialog()
