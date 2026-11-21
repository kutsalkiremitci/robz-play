# Robz Play - tani betigi.
#
# Robz Play kurulmuyorsa, acilmiyorsa ya da hemen kapaniyorsa bu betigi calistirin;
# masaustune "robzplay-tani.txt" adli bir dosya yazar. Sorun bildirirken o dosyayi ekleyin.
# Dosyada bilgisayarin islemcisi, ekran karti, Windows surumu, kurulum dosyasinin durumu ve
# uygulamanin acilirken yazdiklari bulunur. Sifre, hesap ya da konsol bilgisi icermez;
# Windows kullanici adi da dosyadan cikarilir.
#
# Kurulum klasorundeki Tani.bat bu betigi calistirir.
#   powershell -ExecutionPolicy Bypass -File robzplay-tani.ps1
param([string]$Exe = "", [int]$WaitSeconds = 15, [string]$Out = "", [switch]$Hidden)

# the copy this script sits beside, or else the default place the installer uses
if (-not $Exe) {
    $Exe = "$env:LOCALAPPDATA\Programs\Robz Play\RobzPlay.exe"
    if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot "RobzPlay.exe"))) { $Exe = Join-Path $PSScriptRoot "RobzPlay.exe" }
}
if (-not $Out) { $Out = Join-Path ([Environment]::GetFolderPath('Desktop')) "robzplay-tani.txt" }

$r = @()
$r += "Robz Play tani - " + (Get-Date).ToString("yyyy-MM-dd HH:mm")
$os = Get-CimInstance Win32_OperatingSystem
$r += "Windows: " + $os.Caption + " " + $os.Version
$r += "Islemci: " + (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
Get-CimInstance Win32_VideoController | ForEach-Object { $r += "Ekran karti: " + $_.Name + " | surucu " + $_.DriverVersion }
$r += "vulkan-1.dll (System32): " + (Test-Path "$env:WINDIR\System32\vulkan-1.dll")
$r += "Oturum: " + $env:SESSIONNAME + "   (RDP-Tcp ile basliyorsa Uzak Masaustu oturumu)"
$sac = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
$r += "Akilli Uygulama Denetimi (0 kapali, 1 acik, 2 degerlendirme): " + $sac

$r += "--- kurulum dosyasi (Indirilenler ve Masaustu)"
$setups = @(Get-ChildItem "$env:USERPROFILE\Downloads", ([Environment]::GetFolderPath('Desktop')) -Filter "RobzPlay-Setup*.exe" -ErrorAction SilentlyContinue)
foreach ($s in $setups) {
    $motw = [bool](Get-Item $s.FullName -Stream Zone.Identifier -ErrorAction SilentlyContinue)
    $r += $s.Name + " | " + $s.Length + " bayt | SHA256 " + (Get-FileHash $s.FullName -Algorithm SHA256).Hash.ToLower() + " | internetten indirildi isareti: " + $motw
}
if ($setups.Count -eq 0) { $r += "(bulunamadi)" }

$r += "--- kurulu uygulama"
$r += "RobzPlay.exe kurulu mu: " + (Test-Path $Exe)
$r += "Kurulum yolunda ASCII disi harf var mi: " + [bool]($Exe -match '[^\x00-\x7F]') + "   (0.2.2 ve oncesi boyle yollarda acilmiyordu)"
$r += "Kaldirma kaydi var mi: " + (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RobzPlay')
if (Test-Path $Exe) {
    $r += "surum: " + (Get-Item $Exe).VersionInfo.ProductVersion + " | klasordeki dosya sayisi: " + @(Get-ChildItem (Split-Path $Exe) -Recurse -File).Count
    $err = Join-Path $env:TEMP "robzplay-tani-err.txt"
    $std = Join-Path $env:TEMP "robzplay-tani-out.txt"
    Remove-Item $err, $std -ErrorAction SilentlyContinue
    $env:QT_FORCE_STDERR_LOGGING = "1"
    $start = @{ FilePath = $Exe; RedirectStandardError = $err; RedirectStandardOutput = $std; PassThru = $true }
    if ($Hidden) { $start.WindowStyle = 'Hidden' }
    $p = Start-Process @start
    $null = $p.Handle
    if ($p.WaitForExit($WaitSeconds * 1000)) {
        $r += "SONUC: uygulama kapandi. Cikis kodu: " + $p.ExitCode + " (0x" + ('{0:X8}' -f $p.ExitCode) + ")"
    } else {
        $p.Refresh()
        if ($p.MainWindowTitle) {
            $r += "SONUC: uygulama $WaitSeconds saniyedir acik, penceresi var ('" + $p.MainWindowTitle + "'); acilis sorunsuz gorunuyor."
        } else {
            $r += "SONUC: surec $WaitSeconds saniyedir duruyor ama penceresi yok. Ekranda bir hata kutusu varsa icindeki yaziyi da bildirin."
        }
        if ($Hidden) { Stop-Process -Id $p.Id -Force }
    }
    $r += "--- uygulamanin yazdiklari (son 80 satir)"
    $r += @(Get-Content $err, $std -ErrorAction SilentlyContinue | Select-Object -Last 80)
}

$r += "--- Windows olay gunlugu (son 2 gun, RobzPlay gecenler)"
$events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = (Get-Date).AddDays(-2) } -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'RobzPlay' } | Select-Object -First 5)
$events += @(Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 300 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'RobzPlay' } | Select-Object -First 3)
foreach ($e in $events) { $r += $e.TimeCreated.ToString("yyyy-MM-dd HH:mm:ss") + " | " + $e.ProviderName + " | " + $e.Id; $r += $e.Message; $r += "" }
if ($events.Count -eq 0) { $r += "(kayit yok)" }

$r += "--- Windows Guvenligi"
try {
    $threats = @(Get-MpThreatDetection -ErrorAction Stop | Where-Object { ($_.Resources -join ' ') -match 'RobzPlay' } | Select-Object -First 3)
    foreach ($t in $threats) { $r += "Engellendi: " + $t.InitialDetectionTime + " | " + ($t.Resources -join ' ') }
    if ($threats.Count -eq 0) { $r += "RobzPlay ile ilgili engelleme kaydi yok" }
} catch { $r += "Engelleme kayitlari okunamadi (baska bir antivirus olabilir)" }

$logDir = Join-Path $env:APPDATA "RobzPlay\RobzPlay\log"
$r += "--- yayin gunlukleri"
if (Test-Path $logDir) { Get-ChildItem $logDir -Filter *.log | Sort-Object LastWriteTime -Descending | Select-Object -First 3 | ForEach-Object { $r += $_.Name + " (" + $_.Length + " bayt)" } } else { $r += "(yok: hic yayin baslatilmamis)" }

foreach ($f in @(@("robzplay-acilis.log", "acilis gunlugu (en son acilis)", 150), @("robzplay-acilis-onceki.log", "bir onceki acilis", 80))) {
    $r += "--- " + $f[1]
    $path = Join-Path $logDir $f[0]
    if (Test-Path $path) { $r += @(Get-Content $path -Encoding UTF8 -Tail $f[2]) } else { $r += "(yok)" }
}

(($r -join "`r`n").Replace($env:USERNAME, '<kullanici>')) | Out-File $Out -Encoding utf8
"Yazildi: $Out"
