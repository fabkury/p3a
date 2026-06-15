# check_memory.ps1
# -----------------------------------------------------------------------------
# Memory-usage inspector for p3a.
#
# Pops up a tiny window with a "Check memory" button. Each press asks the p3a
# device (default p3a.local) for its current memory-usage breakdown and shows
# it, with particular attention to *internal* RAM (the scarce pool) and the
# INTERNAL|DMA|8BIT "SDIO RX" pool whose exhaustion panics the chip.
#
# How it works (see components/http_api):
#   GET /api/memory           -> full breakdown as JSON; ALSO logs the same
#                                breakdown to the device console.
#   GET /api/memory?silent=1  -> same JSON, but does NOT log on the device.
#
# The device also auto-logs this same breakdown to its console every 2 minutes
# on its own (no HTTP needed) -- see memory_report_task in main/p3a_main.c.
#
# No auth: the device's HTTP API is plain HTTP on the LAN.
#
# GUI usage (double-click the .cmd launcher, or):
#   powershell -ExecutionPolicy Bypass -STA -File check_memory.ps1
#
# Headless usage (for scripted/CI checks -- prints to stdout, sets exit code):
#   powershell -ExecutionPolicy Bypass -File check_memory.ps1 -Headless
#   powershell -ExecutionPolicy Bypass -File check_memory.ps1 -Headless -HostName 192.168.1.50 -Silent
# -----------------------------------------------------------------------------

param(
    [string]$HostName = "p3a.local",
    [switch]$Silent,
    [switch]$Headless
)

# ----------------------------- formatting ------------------------------------

function Format-KB {
    param([double]$Bytes)
    return ("{0,10:N0} B  ({1,9:N1} KB)" -f [int64]$Bytes, ($Bytes / 1024.0))
}

# Render one capability block ({total,free,used,used_pct,largest_free_block,...})
function Format-Cap {
    param([string]$Label, $Cap)
    if (-not $Cap) { return "" }
    $lines = @()
    $lines += $Label
    $lines += ("  total              " + (Format-KB $Cap.total))
    $lines += ("  free               " + (Format-KB $Cap.free) + ("   ({0:N1}% used)" -f [double]$Cap.used_pct))
    $lines += ("  used               " + (Format-KB $Cap.used))
    if ($Cap.PSObject.Properties['min_free']) {
        $lines += ("  min free (boot lo) " + (Format-KB $Cap.min_free))
    }
    $lines += ("  largest free block " + (Format-KB $Cap.largest_free_block))
    return ($lines -join "`r`n")
}

function Format-MemReport {
    param($Data, [string]$HostLabel)
    if (-not $Data) { return "(no data in response)" }

    $out = @()
    $out += "p3a memory report   --   host: $HostLabel"
    $out += ("uptime: {0} s     FreeRTOS tasks: {1}" -f [int64]$Data.uptime_sec, [int]$Data.tasks)
    $out += ""

    # Internal RAM first: it's the pool that actually runs out on the P4.
    $out += (Format-Cap "INTERNAL RAM   (scarce pool -- watch this)" $Data.internal)
    $out += ""

    if ($Data.PSObject.Properties['sdio_rx']) {
        $out += (Format-Cap "SDIO RX pool   ($($Data.sdio_rx.caps)) -- exhaustion panics esp_hosted" $Data.sdio_rx)
        $out += ""
    }

    if ($Data.PSObject.Properties['spiram'] -and $Data.spiram) {
        $out += (Format-Cap "SPIRAM   (external PSRAM -- plentiful)" $Data.spiram)
        $out += ""
    }
    if ($Data.PSObject.Properties['dma'] -and $Data.dma) {
        $out += (Format-Cap "DMA-capable" $Data.dma)
        $out += ""
    }
    if ($Data.PSObject.Properties['all_8bit'] -and $Data.all_8bit) {
        $out += (Format-Cap "8-bit accessible (all byte-addressable RAM)" $Data.all_8bit)
        $out += ""
    }

    if ($Data.PSObject.Properties['heap']) {
        $out += "Overall default heap"
        $out += ("  free               " + (Format-KB $Data.heap.free))
        $out += ("  min free (boot lo) " + (Format-KB $Data.heap.min_free))
        $out += ("  largest free block " + (Format-KB $Data.heap.largest_free_block))
    }

    return ($out -join "`r`n")
}

# ----------------------------- networking ------------------------------------

function Invoke-MemoryCheck {
    param(
        [string]$HostName,
        [bool]$Silent,
        [scriptblock]$Log
    )

    $url = "http://$HostName/api/memory"
    if ($Silent) { $url += "?silent=1" }

    & $Log "GET $url"
    try {
        $resp = Invoke-RestMethod -Uri $url -Method Get -TimeoutSec 15
    } catch {
        & $Log "ERROR: request failed: $($_.Exception.Message)"
        if ($_.Exception.Response) {
            try {
                $sr = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
                & $Log ("  body: " + $sr.ReadToEnd())
            } catch {}
        }
        return $null
    }

    if (-not $resp.ok) {
        & $Log ("ERROR: device returned ok=false: {0} ({1})" -f $resp.error, $resp.code)
        return $null
    }

    & $Log ("(device console logging was {0})" -f ($(if ($resp.silent) { "SUPPRESSED (silent)" } else { "performed" })))
    & $Log ""
    & $Log (Format-MemReport -Data $resp.data -HostLabel $HostName)
    return $resp
}

# ------------------------------- headless ------------------------------------

if ($Headless) {
    # Write-Host (not Write-Output): keep log lines on the host stream so they
    # print, instead of being captured as Invoke-MemoryCheck's return value.
    $log = { param($m) Write-Host $m }
    $r = Invoke-MemoryCheck -HostName $HostName -Silent:$Silent.IsPresent -Log $log
    if ($null -eq $r) { exit 1 }
    exit 0
}

# ------------------------------- GUI -----------------------------------------

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = "p3a memory inspector"
$form.Size = New-Object System.Drawing.Size(620, 560)
$form.StartPosition = "CenterScreen"

$lblHost = New-Object System.Windows.Forms.Label
$lblHost.Text = "Device host:"
$lblHost.Location = New-Object System.Drawing.Point(12, 15)
$lblHost.AutoSize = $true
$form.Controls.Add($lblHost)

$txtHost = New-Object System.Windows.Forms.TextBox
$txtHost.Text = $HostName
$txtHost.Location = New-Object System.Drawing.Point(90, 12)
$txtHost.Size = New-Object System.Drawing.Size(200, 24)
$form.Controls.Add($txtHost)

$chkSilent = New-Object System.Windows.Forms.CheckBox
$chkSilent.Text = "Silent (don't log on device console)"
$chkSilent.Location = New-Object System.Drawing.Point(300, 13)
$chkSilent.AutoSize = $true
$form.Controls.Add($chkSilent)

$btnCheck = New-Object System.Windows.Forms.Button
$btnCheck.Text = "Check memory"
$btnCheck.Location = New-Object System.Drawing.Point(90, 44)
$btnCheck.Size = New-Object System.Drawing.Size(380, 38)
$form.Controls.Add($btnCheck)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = "Vertical"
$logBox.Location = New-Object System.Drawing.Point(12, 96)
$logBox.Size = New-Object System.Drawing.Size(580, 412)
$logBox.Font = New-Object System.Drawing.Font("Consolas", 9)
$logBox.Anchor = "Top,Bottom,Left,Right"
$form.Controls.Add($logBox)

$appendLog = {
    param($msg)
    $logBox.AppendText($msg + "`r`n")
    [System.Windows.Forms.Application]::DoEvents()
}

$btnCheck.Add_Click({
    $btnCheck.Enabled = $false
    $logBox.Clear()
    try {
        $h = $txtHost.Text.Trim()
        if (-not $h) { & $appendLog "Enter a device host first."; return }
        Invoke-MemoryCheck -HostName $h -Silent:$chkSilent.Checked -Log $appendLog | Out-Null
    } catch {
        & $appendLog "UNEXPECTED ERROR: $($_.Exception.Message)"
    } finally {
        & $appendLog ""
        & $appendLog "Done."
        $btnCheck.Enabled = $true
    }
})

[void]$form.ShowDialog()
