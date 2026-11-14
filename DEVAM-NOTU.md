# Devam notu: kablolu (USB) yansıtma — 14 Kasım 2026

Bu not, kablolu yansıtma üzerinde çalışan bir sonraki oturum içindir (insan ya da Claude). Ayrıntılı geçmiş
`GELISTIRME.txt`'nin "bilinen sorunlar" bölümünde, 1.3.0 → 1.3.6 maddelerinde. Kod: `viewer/usbqt.c` (USB tarafı),
`viewer/qtproto.c` (protokol), `viewer/usbdriver.c` (sürücü kurucusu).

## Kısa durum

- **Wi-Fi / ağ üzerinden yansıtma (AirPlay) çalışıyor.** Kablolu yansıtma gerçek cihazda hâlâ çalışmıyor.
- Son sürüm **1.3.6** (main dalı; `usb-akis-duzeltme` dalı main'in aynısı). Her push'ta GitHub Actions Windows
  exe'sini derler ve `viewer/version.h`'deki sürümün taslak (Draft) release'ini yeniler.
- Deneme ortamı: Windows 10 22H2 (19045), bir iPad (USB ürün kimliği 12ab; bazı durumlarda 12a8 görünüyor),
  Apple Aygıtları / Apple Mobile Device Service kurulu. Kullanıcının bilgisayar saati log'da yerel saatten 1 saat
  ileri görünüyor (önemli değil, karşılaştırırken dikkat).
- Kullanıcı Türkçe yazışıyor; denemeleri o yapıyor (exe'yi Releases'tan indirir, `%APPDATA%\ReliosMirror\log.txt`
  gönderir).

## Ne öğrendik (sırayla)

1. **1.3.0:** libusb0 cihazın *kendi* sürücüsüyken (Zadig benzeri bir araçla kurulmuştu; Apple'ın sürücüsünün yerini
   almıştı) görüntü ve ses geldi, 5 kareden sonra "the stream from the device could not be followed" ile koptu.
   Sebep: `usb_bulk_read` süresi dolunca istek iptal edilir ve gelmiş baytlar atılır. **1.3.1+**: okuma boyu mesajın
   eksik kısmı kadar (`qt_session_missing`, paket boyutuna yuvarlı), mesaj arasında tek paket/250 ms, mesaj içinde
   3 sn. Bu düzeltme duruyor; `tests/qtproto_test.c` onu da sınıyor.
2. **1.3.1–1.3.2**'deki zaman uyumsuz okuma (`usb_reap_async_nocancel`) bırakıldı: ilk mesajdan sonra akış durdu,
   iptal edilen istek bitmedi.
3. **1.3.2:** libusb0 kendi sürücüyken geçişten sonra kimse yapılandırma seçmiyor ("active configuration 0");
   program 5'i kendisi seçer. (Yalnızca libusb0 kendi sürücüyken.)
4. **1.3.3:** okumadan önce `usb_clear_halt` (cihaza giden pipe) → cihaz hiç PING göndermedi. **Kaldırıldı, geri
   koymayın.** Cihaz PING'i yapılandırması seçildiği anda bir kez gönderiyor.
5. libusb0 kendi sürücüyken Apple'ın hizmeti cihaza ulaşamıyor: **"Bu Bilgisayara Güven" çıkmıyor**, Apple Aygıtları
   iPad'i görmüyor. Kullanıcı Aygıt Yöneticisi'nden libusb0'ı kaldırıp Apple'ın sürücüsüne döndü (USBCCGP +
   AppleLowerFilter), sonra programın kurucusuyla libusb0'ı **üst filtre** olarak kurdu (başarılı).
6. **1.3.4 → MAVİ EKRAN.** Apple'ın sürücüsü + libusb0 filtresi düzeninde geçişten sonra `claim_interface`
   "invalid configuration 0" ile reddedildi (filtre, Windows'un bileşik sürücüsünün seçtiği yapılandırmayı görmez).
   `open_capture`'daki yedek adım (iPhoneMirror'dan) yapılandırmayı libusb0 üzerinden yeniden seçti → Windows çöktü.
   **1.3.5**: libusb0 cihazın kendi sürücüsü değilse hiçbir yapılandırma seçilmez (`libusb0_runs_device`); arayüz
   alınamazsa log'a yazılır, cihaz yeniden takılana / "Yeniden Dene"ye kadar tekrar geçirilmez. Gerçek cihazda
   denendi: çökme yok, cihaz düzgün geri verildi.

**Sonuç:** Bugünkü tasarımla iki düzen de kullanılamaz:
- Apple'ın sürücüsü + libusb0 filtresi: arayüz alınamaz; zorlamak Windows'u çökertir.
- Yalnızca libusb0: görüntü gelir ama Apple'ın hizmeti (güven/eşleşme) çalışmaz.

## Sıradaki adım (kullanıcının seçtiği yol: "2")

Fikir: Apple'ın sürücüsü cihazı ekran kaydı yapılandırmasına (5) aldığında Windows'un bileşik sürücüsü (usbccgp) her
arayüz için bir alt cihaz açar: ekran kaydı arayüzü `USB\VID_05AC&PID_12AB&MI_02\...` olmalı (sınıf 0xFF, alt sınıf
0x2A). Sürücümüz (libusb0 ya da WinUSB) yalnızca **o alt cihaza işlev sürücüsü** olarak bağlanırsa yapılandırmayı
usbccgp yönetir, libusb0'ın hiçbir yapılandırma seçmesi gerekmez, Apple'ın kanalı (eşleşme) çalışmaya devam eder.

**1.3.6 yalnızca bunu ölçer (hiçbir şey kurmaz, değiştirmez):** arayüz alınamadığında (1.3.5'teki durum) iPad'in
alt cihazlarını log'a yazar (`log_interfaces`): `interface device: <kimlik>; driver <hizmet>; "<ad>"; compatible
<ilk uyumlu kimlik>; started/not started, problem <kod>`. Bu satırların çıkması için libusb0 filtresinin iPad'de
**kurulu** olması gerekir (Ayarlar > USB > USB Sürücüsü > Kur); filtre tek başına sorun çıkarmadı (1.3.5 denemesi
filtre kuruluyken yapıldı).

Kullanıcıdan istenecek: 1.3.6'yı indir → filtre kurulu değilse Kur → iPad'i tak, kilidini aç → 30 sn bekle →
`log.txt`'yi gönder.

Log'a göre karar:
- `...&MI_02...; driver none; ... problem 28` (sürücüsüz) → yol açık: o alt cihaza sürücü bağlanabilir.
  Yapılacaklar: (a) kurucuya "o alt cihaza işlev sürücüsü" kurulumu (INF + imzalı katalog gerekir; libwdi/Zadig
  bunu kendi imzaladığı sertifikayı güvenilenlere ekleyerek yapar — güvenlik açısından kullanıcıya açıkça
  sorulmalı); (b) `usbqt.c`'de cihaz arama ve `open_capture` alt cihazı (tek arayüzlü ayrı bir libusb0 cihazı)
  bulacak şekilde değişmeli; `set_configuration` hiç çağrılmamalı.
- MI_02 bir Apple sürücüsüne bağlıysa ya da hiç görünmüyorsa → bu yol kapalı; kablolu yansıtmayı kapatıp ağ
  üzerinden yansıtmayı önermek daha doğru.

## 1.3.6 ölçümü ve 1.3.7

- Ölçüm: MI_02 (ekran kaydı, sınıf ff/2a) var ama sürücüsü **Netaapl** (iPad'in USB Ethernet'i; normal
  yapılandırmada 2 numaralı arayüz o), sorun 10 ile çalışmıyor. MI_01 (Apple'ın eşleşme kanalı) zaten **WinUSB**.
- Kullanıcı yolu onayladı. **1.3.7**: MI_02'ye Windows'un kendi winusb.inf'inden WinUSB (Aygıt Yöneticisi'nin
  "listeden seç"i gibi; sertifika/paket yok), akış WinUSB ile okunur. Ayrıntı: GELISTIRME.txt 1.3.7 maddesi.
- Gerçek cihazda denenecek: Ayarlar > USB'de "Ekran kaydı sürücüsü gerekli" > Kur. Kurucu kodu 171-175 ise
  hangi adımda kaldığı usbqt.h'deki USBQT_DRIVER_WINUSB_STEP açıklamasında.

## Yapılmaması gerekenler

- Apple'ın sürücüsü cihazı çalıştırırken libusb0 üzerinden `usb_set_configuration` (mavi ekran, 1.3.4).
- `usb_clear_halt` akış başında (cihaz susuyor, 1.3.3).
- libusb0'ı iPad'in tamamına işlev sürücüsü yapmak (Apple'ın eşleşmesi bozuluyor).
- Kullanıcının bilgisayarında çekirdek sürücüsü değiştiren bir şeyi, ne olacağını ve geri dönüşünü söylemeden
  denetmek: her deneme mavi ekran riski taşıyor.

## Çözülmemiş küçük soru

- Kurucu bir kez "result 7" (hizmet) döndü; mevcut `libusb0` hizmeti beklenen biçimdeydi (`sc qc`: çekirdek
  sürücü, `System32\drivers\libusb0.sys`, isteğe bağlı başlatma) ve `sc config` değişikliğe izin verdi. Sebep
  bulunamadı; sonraki denemede kurulum başarılı oldu.
