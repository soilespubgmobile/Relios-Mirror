@echo off
rem iPad Ekran Yansitma listesinde "Relios Mirror" gorunmuyorsa bunu bir kez calistirin:
rem Windows Guvenlik Duvari'nda alici programa yerel ag izni verir (yonetici onayi ister).
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
netsh advfirewall firewall delete rule name="Relios Mirror" >nul 2>&1
netsh advfirewall firewall add rule name="Relios Mirror" dir=in action=allow program="%~dp0relios-airplay-receiver.exe" enable=yes profile=private,domain
echo.
echo Izin eklendi. Wi-Fi aginiz Windows'ta "Ozel ag" olarak ayarli olmali.
pause
