@echo off
rem iPad Ekran Yansitma listesinde "Relios Mirror" gorunmuyorsa bunu bir kez calistirin:
rem Windows Guvenlik Duvari'nda alici programa yerel ag izni verir (yonetici onayi ister).
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
netsh advfirewall firewall delete rule name="Relios Mirror" >nul 2>&1
netsh advfirewall firewall delete rule name=all program="%~dp0relios-airplay-receiver.exe" >nul 2>&1
netsh advfirewall firewall add rule name="Relios Mirror" dir=in action=allow protocol=TCP localport=7000,7100
netsh advfirewall firewall add rule name="Relios Mirror" dir=in action=allow protocol=UDP localport=5353,6000,6001,7011
echo.
echo Izin eklendi.
pause
