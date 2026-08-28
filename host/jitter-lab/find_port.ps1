# find_port.ps1 — probe COM ports for ESP-IDF log traffic (read-only).
# WARNING: opening a port with System.IO.Ports.SerialPort toggles DTR/RTS and
# RESETS the ESP32 behind it. Use only when the port is unknown.
#
# Usage: pwsh host/jitter-lab/find_port.ps1 [COM5,COM12]
param([string[]]$Ports)

if (-not $Ports -or $Ports.Count -eq 0) {
    $Ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object { [int]($_ -replace 'COM','') } -Descending
}

foreach ($p in $Ports) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort $p, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        $sp.ReadTimeout = 2500
        $sp.Open()
        Start-Sleep -Milliseconds 2500
        $n = $sp.BytesToRead
        $s = if ($n -gt 0) { $sp.ReadExisting() } else { '' }
        $sp.Close()
        $clean = ($s -replace '[^\x20-\x7E]', '.')
        $sample = $clean.Substring(0, [Math]::Min(160, $clean.Length))
        $tag = if ($clean -match 'ESP-ROM|esp32p4|I \(\d+\)') { 'ESP' } else { '   ' }
        Write-Output ("{0,-6} {1} bytes={2,-6} {3}" -f $p, $tag, $n, $sample)
    } catch {
        Write-Output ("{0,-6} ERR {1}" -f $p, $_.Exception.Message.Split("`n")[0])
    }
}
