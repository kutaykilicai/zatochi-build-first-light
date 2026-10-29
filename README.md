# shadPS4: inFAMOUS First Light (kişisel derleme)

Bu depo, axylpup'un shadPS4 second-son çatalının (sürüm second-son-v1.1.1) üzerine kurulmuş kişisel bir derlemedir. Amaç, inFAMOUS First Light'ı (CUSA00897, sürüm 1.04) Second Son'daki ayarlar ve kontrollerle oynamak.

## Değişiklikler

- Fare ile birebir kamera (SHADPS4_MOUSE_AIM): Second Son'daki kamera kancası First Light'ın kamera koduna uyarlandı. Kanca yalnızca oyunun kodu beklenen baytlarla birebir eşleşirse kurulur, eşleşmezse fare sağ çubuk öykünmesiyle çalışmaya devam eder.
  - Kamera güncelleme fonksiyonu: 0x171c00, açı yazımı 0x17228a (kamera r15).
  - Otomatik takip baytı: 0xf2b380 (0x16cf60 fonksiyonu okuyor, Second Son'daki 0x149360 ile aynı kod).
  - Dikey açı sınırı ve oyunun ters kamera ayarı First Light'ta bulunamadığı için kullanılmıyor. Ters kamera gerekirse SHADPS4_MOUSE_AIM_FLIP=y kullanılabilir.
- Derleme yalnızca Windows için yapılır.

Asıl proje belgeleri README-axylpup.md dosyasındadır.

## Lisans

GPL-2.0. Oyun dosyası, firmware ya da anahtar içermez. Derlenmiş dosyalar dağıtılmaz.
