# play_makapix_64.ps1
# -----------------------------------------------------------------------------
# Stress-test helper for p3a.
#
# Pops up a tiny window with a single button. Pressing it tells the p3a device
# (default p3a.local) to create and immediately play a 64-channel playset named
# "makapix_64", where every channel is a different Makapix Club hashtag.
#
# 64 channels is the documented maximum (PS_MAX_CHANNELS), so this exercises the
# device at its design limit.
#
# How it works (see components/http_api):
#   POST /playsets/makapix_64   { "channels":[...64...], "activate":true }
#       -> creates/overwrites the playset and starts switching to it.
#   GET  /playsets/active       -> polled to confirm the switch landed
#                                  (switching==false, last_switch_error=="").
#
# No auth: the device's HTTP API is plain HTTP on the LAN.
#
# Run via the play_makapix_64.cmd launcher (handles -STA), or:
#   powershell -ExecutionPolicy Bypass -STA -File play_makapix_64.ps1
# -----------------------------------------------------------------------------

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# --- The 64 hashtags (one per channel). Tags only, no leading '#', no spaces. --
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

$PlaysetName = 'makapix_64'

# ----------------------------- networking ------------------------------------

function Build-PlaysetJson {
    $channels = foreach ($tag in $Hashtags) {
        [ordered]@{
            type         = 'hashtag'
            name         = 'hashtag'
            identifier   = $tag
            display_name = "#$tag"
            weight       = 100
            offset       = 0
        }
    }
    $payload = [ordered]@{
        channels = @($channels)
        activate = $true
    }
    return ($payload | ConvertTo-Json -Depth 5)
}

function Invoke-PlayMakapix64 {
    param([string]$HostName, [scriptblock]$Log)

    $base = "http://$HostName"
    & $Log "Target: $base"
    & $Log ("Building playset '{0}' with {1} hashtag channels..." -f $PlaysetName, $Hashtags.Count)

    $json  = Build-PlaysetJson
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
            & $Log "           Hashtag channels won't refresh until the device is registered with Makapix Club."
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
$form.Text = "p3a stress test - makapix_64"
$form.Size = New-Object System.Drawing.Size(560, 460)
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

$btn = New-Object System.Windows.Forms.Button
$btn.Text = "Play makapix_64  (64 channels)"
$btn.Location = New-Object System.Drawing.Point(90, 44)
$btn.Size = New-Object System.Drawing.Size(380, 40)
$form.Controls.Add($btn)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = "Vertical"
$logBox.Location = New-Object System.Drawing.Point(12, 96)
$logBox.Size = New-Object System.Drawing.Size(520, 312)
$logBox.Font = New-Object System.Drawing.Font("Consolas", 9)
$logBox.Anchor = "Top,Bottom,Left,Right"
$form.Controls.Add($logBox)

$appendLog = {
    param($msg)
    $logBox.AppendText($msg + "`r`n")
    [System.Windows.Forms.Application]::DoEvents()
}

$btn.Add_Click({
    $btn.Enabled = $false
    $logBox.Clear()
    $h = $txtHost.Text.Trim()
    if (-not $h) { & $appendLog "Enter a device host first."; $btn.Enabled = $true; return }
    try {
        Invoke-PlayMakapix64 -HostName $h -Log $appendLog | Out-Null
    } catch {
        & $appendLog "UNEXPECTED ERROR: $($_.Exception.Message)"
    } finally {
        & $appendLog "Done."
        $btn.Enabled = $true
    }
})

[void]$form.ShowDialog()
