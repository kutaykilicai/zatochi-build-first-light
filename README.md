# Zatochi Build: inFAMOUS First Light (shadPS4)

Zatochi Build is maintained by Kutay Kılıç (Zatochi), GitHub: kutaykilicai

- [English](#english)
- [Türkçe](#türkçe)

---

## English

Zatochi Build is a Windows build of shadPS4 tuned for inFAMOUS First Light (CUSA00897, version 1.04). It is based on AxylPup's shadPS4 Second Son fork (second-son-v1.1.1) and brings the same controls and settings to First Light.

### What is different

- 1:1 mouse aiming (`SHADPS4_MOUSE_AIM`): the Second Son camera hook was ported to First Light's camera code. The hook is only installed when the game code matches the expected bytes exactly. If it doesn't match, the mouse keeps working through right stick emulation.
  - Camera update function: 0x171c00, angle store at 0x17228a (camera in r15).
  - Auto-follow byte: 0xf2b380 (read by function 0x16cf60, identical to 0x149360 in Second Son).
  - The vertical angle clamp and the in-game invert option were not found in First Light, so they are not used. If you need an inverted camera, set `SHADPS4_MOUSE_AIM_FLIP=y`.
- PC key icons: the PlayStation button glyphs in the game UI are replaced with keyboard and mouse key caps (`SHADPS4_TEX_REPLACE`, `tex_replace` folder). The key cap images are our own drawings.
- Only Windows builds are produced.

### Installation

1. Download `Zatochi-Build-FirstLight-shadPS4.zip` from the Releases page.
2. Extract it into a folder and point shadPS4 to your First Light game folder, or put the files next to your existing shadPS4 setup.
3. `shadps4-flags.txt` and `shadps4-flags-user.txt` are read at startup. The user file sets `SHADPS4_CACHE_NAME=CUSA00897-v104` and overrides the base file.
4. `user\custom_configs\CUSA00897.json` holds the game settings and `user\input_config\default.ini` holds the key bindings.
5. To go back to the old mouse behaviour, add `SHADPS4_MOUSE_AIM=0` to `shadps4-flags-user.txt`. To turn the key icons off, add `SHADPS4_TEX_REPLACE=0`.

You must supply your own legally dumped copy of the game. This repository and its releases contain no game files, firmware, keys or extracted game textures.

The release binary is a plain GitHub Actions build without PGO, so it can be slightly slower than AxylPup's PGO builds.

### Credits

Many thanks to AxylPup for the Second Son fork this build stands on, and to the shadPS4 team for the emulator itself. The original project documentation is in `README-axylpup.md` and `README-axylpup-second-son.md`.

### License

GPL-2.0, same as shadPS4. The full source of every release is in this repository at the release tag.

---

## Türkçe

Zatochi Build, Kutay Kılıç (Zatochi) tarafından geliştiriliyor. GitHub: kutaykilicai

Zatochi Build, inFAMOUS First Light (CUSA00897, sürüm 1.04) için ayarlanmış bir shadPS4 Windows derlemesi. AxylPup'un shadPS4 Second Son çatalını (second-son-v1.1.1) temel alıyor ve Second Son'daki kontrolleri ve ayarları First Light'a taşıyor.

### Farklar

- Fare ile birebir nişan (`SHADPS4_MOUSE_AIM`): Second Son'daki kamera kancası First Light'ın kamera koduna uyarlandı. Kanca yalnızca oyunun kodu beklenen baytlarla birebir eşleşirse kurulur. Eşleşmezse fare sağ çubuk öykünmesiyle çalışmaya devam eder.
  - Kamera güncelleme fonksiyonu: 0x171c00, açı yazımı 0x17228a (kamera r15).
  - Otomatik takip baytı: 0xf2b380 (0x16cf60 fonksiyonu okuyor, Second Son'daki 0x149360 ile aynı kod).
  - Dikey açı sınırı ve oyunun ters kamera ayarı First Light'ta bulunamadığı için kullanılmıyor. Ters kamera gerekirse `SHADPS4_MOUSE_AIM_FLIP=y` kullanılabilir.
- PC tuş ikonları: oyun arayüzündeki PlayStation tuş simgeleri klavye ve fare tuşlarıyla değiştiriliyor (`SHADPS4_TEX_REPLACE`, `tex_replace` klasörü). Tuş görselleri bize ait çizimler.
- Yalnızca Windows için derleniyor.

### Kurulum

1. Releases sayfasından `Zatochi-Build-FirstLight-shadPS4.zip` dosyasını indir.
2. Bir klasöre çıkar ve shadPS4'e First Light oyun klasörünü göster ya da dosyaları mevcut shadPS4 kurulumunun yanına koy.
3. `shadps4-flags.txt` ve `shadps4-flags-user.txt` açılışta okunur. Kullanıcı dosyası `SHADPS4_CACHE_NAME=CUSA00897-v104` ayarını yapar ve temel dosyanın önüne geçer.
4. Oyun ayarları `user\custom_configs\CUSA00897.json`, tuş atamaları `user\input_config\default.ini` içinde.
5. Eski fare davranışına dönmek için `shadps4-flags-user.txt` dosyasına `SHADPS4_MOUSE_AIM=0` satırını ekle. Tuş ikonlarını kapatmak için `SHADPS4_TEX_REPLACE=0` ekle.

Oyunu kendin, yasal yollarla dump etmen gerekiyor. Bu depoda ve sürümlerde oyun dosyası, firmware, anahtar ya da oyundan çıkarılmış doku yok.

Sürümdeki exe, PGO kullanılmadan yapılmış düz bir GitHub Actions derlemesi. Bu yüzden AxylPup'un PGO'lu derlemelerinden biraz yavaş olabilir.

### Teşekkürler

Bu derlemenin dayandığı Second Son çatalı için AxylPup'a, emülatörün kendisi için de shadPS4 ekibine çok teşekkürler. Asıl proje belgeleri `README-axylpup.md` ve `README-axylpup-second-son.md` dosyalarında.

### Lisans

shadPS4 gibi GPL-2.0. Her sürümün kaynak kodunun tamamı bu depoda, sürüm etiketinde duruyor.
