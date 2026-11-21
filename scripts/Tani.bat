@echo off
rem Robz Play - tani. Uygulama acilmiyorsa bu dosyaya cift tiklayin; masaustunde olusan
rem robzplay-tani.txt dosyasini sorun bildirirken gonderin.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0robzplay-tani.ps1" %*
echo.
echo Masaustunde "robzplay-tani.txt" adli bir dosya olustu. Sorunu bildirirken bu dosyayi gonderin.
echo.
pause
