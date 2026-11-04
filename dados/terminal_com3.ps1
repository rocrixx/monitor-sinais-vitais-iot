$log = Join-Path $PSScriptRoot ("calibracao_" + (Get-Date -Format "yyyy-MM-dd_HHmm") + ".txt")
try {
    $p = New-Object System.IO.Ports.SerialPort "COM3", 115200
    $p.DtrEnable = $false
    $p.RtsEnable = $false
    $p.NewLine = "`n"
    $p.Open()
} catch {
    Write-Host "Nao abriu a COM3. Feche o Serial Monitor da Arduino IDE e rode de novo."
    exit 1
}
Write-Host "Terminal da COM3 aberto. Tudo fica gravado em $log"
Write-Host "Digite o comando e Enter (ex.: t 36.9). Para sair: Ctrl+C."
$buf = ""
try {
    while ($true) {
        $s = $p.ReadExisting()
        if ($s) {
            Write-Host -NoNewline $s
            Add-Content -Path $log -Value $s -NoNewline -Encoding utf8
        }
        while ([Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            if ($k.Key -eq "Enter") {
                Write-Host ""
                $p.WriteLine($buf)
                Add-Content -Path $log -Value ("`n>>> " + $buf + "`n") -NoNewline -Encoding utf8
                $buf = ""
            } elseif ($k.Key -eq "Backspace") {
                if ($buf.Length -gt 0) { $buf = $buf.Substring(0, $buf.Length - 1); Write-Host -NoNewline "`b `b" }
            } else {
                $buf += $k.KeyChar
                Write-Host -NoNewline $k.KeyChar
            }
        }
        Start-Sleep -Milliseconds 20
    }
} finally {
    $p.Close()
}
