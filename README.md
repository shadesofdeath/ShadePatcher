# ShadePatcher

Windows kabuğunu (explorer.exe) iyileştiren bir araç. İki parçadan oluşur:

- **Ayar penceresi**, ExplorerPatcher'ın penceresiyle birebir aynı motoru kullanır. Pencere hiçbir diyalog
  denetimi içermez; sayfalar ve seçenekler `settings.reg` içindeki yorum satırlarıyla tanımlanır, pencere kendini
  çizer.
- **Mod motoru**, explorer.exe içinde çalışır ve kabuğa dokunan değişiklikleri (modları) barındırır.

## Durum

| Parça | Durum |
|-------|-------|
| Ayar penceresi | Çalışıyor. Türkçe ve İngilizce. Genel, Modlar, iki menü sayfası, Ayarlar, Hakkında. Genel sayfası motorun kabukta olup olmadığını gösterir. |
| Mod motoru | Çalışıyor. Kanca, sembol, ayar, çakışma hakemi. 39 testin hepsi geçiyor. Masaüstü yüzeyi kabuğun kendi iş parçacığından bağlanır ve masaüstü yeniden kurulunca kendini yeniden bağlar. |
| Kabuğu yeniden başlatma | Pencere explorer.exe içinden açılmış olsa da çalışır: iş `rundll32 ShadePatcher.dll,ZZRestartExplorer` yardımcısına verilir, yeni kabuğa motor (enjekte edilmişse) daha ilk komutu çalışmadan geri yüklenir ve pencere yeniden açılır. Kabuğa temiz çıkması için 20 s tanınır; çıkmazsa sonlandırılır ve Winlogon'un kendi getirdiği kabuk beklenip motor ona geç yüklenir, ikinci bir explorer başlatılıp görev çubuğu yarışına girilmez. Aynı anda tek yeniden başlatma (adlandırılmış mutex). Pencerenin alt satırı motorun o anki durumunu gösterir: aktif / yüklü değil / güvenli mod / yeniden başlatılıyor. `ZZStartup` ile oturum açılışında da yüklenebilir. |
| Kurulum | `sp_setup.exe` tek dosya: ShadePatcher.dll ve sp_gui.dll içine gömülüdür (yanında yeni derlenmiş kopyalar varsa onları tercih eder). Kurar, kaldırır, Ayarlar listesinde görünür ve Başlat menüsüne "ShadePatcher Ayarları" kısayolu koyar; kurulumsuz kullanımda aynı kısayolu motor kullanıcı için kendisi yazar. |
| Modlar | 5 tane hazır, 9 tanesi sırada (aşağıda). |
| Başlat menüsü | Kendi Başlat menüsü (`custom-start-menu`): Win32 + DirectComposition + Direct2D + DirectWrite, Open-Shell yöntemiyle Başlat düğmesini ve Windows tuşunu devralır. Ayar penceresinde "Başlat menüsü" sayfası. Ayrıntı: [docs/startmenu.md](docs/startmenu.md). |

## Klasör düzeni

```
ShadePatcher/
├── ShadePatcher.sln            Visual Studio çözümü (yalnızca x64)
├── build.cmd                   Komut satırından derleme: build.cmd [Debug|Release]
├── build/
│   ├── props/                  Tüm projelerin ortak MSBuild ayarları
│   ├── bin/<Config>/           Üretilen ikililer (git dışı)
│   └── obj/<Config>/           Ara dosyalar (git dışı)
├── design/
│   ├── icon/                   Logo tasarımının kaynağı (SVG + 1024px PNG, iki palet)
│   └── startmenu/              Başlat menüsünün tasarım teslim paketi (README belirtimdir)
├── libs/
│   └── SlimDetours/            Kanca kütüphanesi (KNSoft.SlimDetours, MIT)
├── docs/
│   ├── engine.md               Mod motorunun mimarisi ve kararların gerekçeleri
│   ├── installing.md           Kurulumun ne yaptığı, çökme koruması, kurtarma
│   ├── writing-a-mod.md        Mod yazma ve Windhawk modu uyarlama rehberi
│   ├── startmenu.md            Kendi Başlat menüsünün mimarisi, Başlat'ı devralma yöntemi, önizleme
│   └── settings-format.md      settings.reg direktiflerinin tam listesi
├── tools/
│   ├── make_icon.py            design/icon kaynağından src/common/app.ico üretir
│   ├── check_proxy.py          dxgi proxy'sinin gerçek dxgi.dll'e ilettiğini doğrular
│   └── selftest/               Motoru explorer.exe'ye dokunmadan sınayan konsol programı
└── src/
    ├── common/                 İki DLL'in paylaştığı statik kitaplık
    │   ├── app.ico             Uygulama ikonu (her iki ikiliye de gömülür)
    │   ├── config.h            Ürün adı, kayıt yolu, CLSID (tek yerden yeniden adlandırma)
    │   ├── osversion.*         Windows derleme tespiti, Mica
    │   ├── localization.*      Dil seçimi
    │   ├── explorer.*          explorer.exe'yi yeniden başlatma
    │   ├── inputbox.*          Metin girişi diyaloğu
    │   └── utils.*, getline.c, fmemopen.c
    ├── gui/                    sp_gui.dll: ayar penceresi
    │   ├── GUI.c / GUI.h       Çizim ve tıklama motoru
    │   ├── registry.*          Kayıt sarmalayıcıları, dışa/içe aktarma
    │   ├── conditions.*        ";s" satırlarının koşulları
    │   └── resources/          settings.reg, strings.h, lang/ (en-US, tr-TR)
    ├── core/                   ShadePatcher.dll: explorer.exe'ye yüklenen modül
    │   ├── dllmain.c           Giriş noktası, ZZGUI
    │   ├── dxgi_proxy.c        explorer.exe'ye yükleme yolu
    │   ├── engine/             Mod motoru (docs/engine.md)
    │   ├── mods/               Modlar ve mod_table.c
    │   └── startmenu/          Kendi Başlat menüsü (docs/startmenu.md), Figtree yazı tipi (OFL)
    └── setup/                   sp_setup.exe: kurar ve kaldırır (docs/installing.md)
```

## Derleme ve çalıştırma

Visual Studio 2022 veya üstü, "Desktop development with C++" iş yükü.

```
build.cmd Release
```

Çıktı `build\bin\Release\`: `ShadePatcher.dll`, `sp_gui.dll`, `sp_setup.exe`, `sp_selftest.exe`.

Ayar penceresini açmak:

```
rundll32 "build\bin\Release\ShadePatcher.dll",ZZGUI
```

Kurmadan önce çalıştırılacak iki denetim. İkisi de kabuğa dokunmaz:

```
build\bin\Release\sp_selftest.exe     motoru ve modları sınar
python tools\check_proxy.py           proxy'nin gerçek dxgi.dll'e ilettiğini doğrular
```

## Kurmak

Kurulum yönetici hakkı ister ve Dosya Gezgini'ni yeniden başlatır:

```
build\bin\Release\sp_setup.exe
```

Kaldırmak için Ayarlar > Uygulamalar listesinden, ya da:

```
"%ProgramFiles%\ShadePatcher\sp_setup.exe" /uninstall
```

Kurulumun tam olarak ne yaptığı, etki alanı ve kurtarma adımları için
[docs/installing.md](docs/installing.md).

Bir mod kabuğu çökertirse motor kendini korur: kabuk 60 saniye içinde üç kez başlarsa o oturumda hiçbir mod
yüklenmez, masaüstü geri gelir ve suçlu mod kapatılabilir.

## Mod motoru

Ayrıntı ve gerekçeler [docs/engine.md](docs/engine.md) içinde. Özet:

- **Kanca.** SlimDetours üzerine kurulu, işlem tabanlı. Bir grup kanca ya tamamen uygulanır ya hiç uygulanmaz;
  uygularken diğer iş parçacıkları askıya alınır. Her kanca isteyen modun kimliğiyle kaydedilir, böylece bir mod
  kapatıldığında yalnızca onun kancaları sökülür.
- **Sembol.** Kabuğun çoğu dışa aktarılmaz. Motor modülün PDB kimliğini okur, gerekirse Microsoft sembol
  sunucusundan PDB indirir, adresleri çıkarır ve kayıt defterinde önbelleğe alır. Önbellek **modül başına** tutulur
  ve tüm modlar paylaşır. Ölçüm: ilk arama 2,3 sn (indirme dahil), önbellekten 0 ms.
- **Çakışma hakemi.** İki mod aynı hareketi isterse (örneğin masaüstüne çift tıklama) yüzeyi motor sahiplenir ve
  hareketi önceliğe göre dağıtır; ilk işleyen zinciri durdurur. Sıralamayı kullanıcı `InputPriority` ayarıyla
  belirler. Windhawk'ta her iki mod da çalışır, bu farkı kapatıyoruz.
- **Canlı ayar.** Ayar değişince mod yeniden başlatma olmadan yüklenir, kaldırılır ya da ayarını yeniden okur.

Tasarım ExplorerPatcher ve Windhawk'ın ikisinden de yararlanır; hangi fikrin nereden geldiği docs/engine.md'de
yazılıdır.

## Modlar

Modlar Windhawk topluluğunun yayımladığı fikirlerden uyarlanır. **Kod kopyalanmaz:** özgün modun ne yaptığı
okunur, aynı davranış bu motorun API'sine karşı yeniden yazılır, özgün mod ve yazarı hem kaynakta hem burada
anılır.

### Hazır

| Mod | Ne yapar | Uyarlandığı Windhawk modu | Özgün yazar |
|-----|----------|---------------------------|-------------|
| `tray-show-all-icons` | Bildirim alanındaki tüm simgeleri her zaman gösterir | [taskbar-notification-icons-show-all](https://windhawk.net/mods/taskbar-notification-icons-show-all) | m417z |
| `tray-icons-rebroadcast` | Kabuk açılırken kaybolan tepsi simgelerini geri getirir | [taskbar-disappearing-tray-icons-fix](https://windhawk.net/mods/taskbar-disappearing-tray-icons-fix) | Alchemy |
| `desktop-toggle-icons` | Masaüstüne çift tıklayınca simgeleri gizler veya gösterir | [zen-desktop-toggle-icons](https://windhawk.net/mods/zen-desktop-toggle-icons) | Lanbo |
| `context-menu-preloader` | Bağlam menüsü işleyicilerini önceden yükler, ilk sağ tık hızlanır | [context-menu-preloader](https://windhawk.net/mods/context-menu-preloader) | Lockframe |
| `explorer-auto-file-sizes` | Boyut sütununu yalnızca KB yerine KB/MB/GB gösterir | [explorer-details-better-file-sizes](https://windhawk.net/mods/explorer-details-better-file-sizes) | m417z |
| `explorer-double-click-up` | Klasörün boş alanına çift tıklayınca üst klasöre çıkar | [explorer-double-click-up](https://windhawk.net/mods/explorer-double-click-up) | wrldspawn |
| `taskbar-empty-space-clicks` | Görev çubuğunun boş alanına çift/orta tık ile eylem (masaüstü, Başlat, Görev Yöneticisi, komut, sessiz, otomatik gizle) | [taskbar-empty-space-clicks](https://windhawk.net/mods/taskbar-empty-space-clicks) | m1lhaus |
| `taskbar-volume-control` | Görev çubuğu üzerinde tekerlekle ses; ses simgesine orta tık sessiz | [taskbar-volume-control](https://windhawk.net/mods/taskbar-volume-control) | m417z |
| `taskbar-wheel-cycle` | Görev çubuğu düğmesi üzerinde tekerlekle pencereler arasında geçiş | [taskbar-wheel-cycle](https://windhawk.net/mods/taskbar-wheel-cycle) | m417z |
| `taskbar-button-click` | Düğmeye orta tık pencereyi kapatır; Ctrl ile görevi sonlandırır | [taskbar-button-click](https://windhawk.net/mods/taskbar-button-click) | m417z |
| `taskbar-clock-customization` | Saat/tarih biçimi, satır şablonları, yazı boyutu ve rengi | [taskbar-clock-customization](https://windhawk.net/mods/taskbar-clock-customization) | m417z |
| `taskbar-icon-size` | Görev çubuğu yüksekliği, simge boyutu, düğme genişliği | [taskbar-icon-size](https://windhawk.net/mods/taskbar-icon-size) | m417z |
| `taskbar-labels` | Düğme etiketleri ve genişlik sınırları (bu yapıda HasLabel sembolleri bulunamıyor, bkz. bilinen sorunlar) | [taskbar-labels](https://windhawk.net/mods/taskbar-labels) | m417z |
| `taskbar-start-button-position` | Simgeler ortadayken Başlat (ve isteğe bağlı arama/görev görünümü) solda | [taskbar-start-button-position](https://windhawk.net/mods/taskbar-start-button-position) | m417z |
| `explorer-single-window-tabs` | Yeni klasörler açık pencerede sekme olarak açılır | [explorer-single-window-tabs](https://windhawk.net/mods/explorer-single-window-tabs) | ALMAS CP |
| `file-explorer-reopen-closed-tab` | Ctrl+Shift+T son kapatılan sekmeyi geri açar | [file-explorer-reopen-closed-tab](https://windhawk.net/mods/file-explorer-reopen-closed-tab) | Armaninyow |
| `extension-change-no-warning` | Uzantı değiştirirken onay sormaz | [extension-change-no-warning](https://windhawk.net/mods/extension-change-no-warning) | m417z |
| `hide-desktop-icon-text` | Kısayol oklarını kaldırır; simge yazısını gizler (yazı gizleme bu yapıda etkisiz, bkz. bilinen sorunlar) | [hide-desktop-icon-text](https://windhawk.net/mods/hide-desktop-icon-text) | kivsak |
| `desktop-icons-view` | Masaüstü simgeleri liste/ayrıntı/küçük/kutucuk görünümünde | [desktop-icons-view](https://windhawk.net/mods/desktop-icons-view) | m417z |
| `start-menu-all-apps` | Başlat açılınca tüm uygulamalar (yalnızca Başlat explorer.exe içindeyken) | [start-menu-all-apps](https://windhawk.net/mods/start-menu-all-apps) | m417z |
| `win11-power-buttons` | Başlat'a tek tık güç düğmeleri (yalnızca Başlat explorer.exe içindeyken) | [win11-power-buttons](https://windhawk.net/mods/win11-power-buttons) | Hakuuyosei |
| `taskbar-tray-system-icon-tweaks` | Ses, ağ, pil, mikrofon, konum, dil çubuğu, zil ve Masaüstünü göster düğmesini gizle | [taskbar-tray-system-icon-tweaks](https://windhawk.net/mods/taskbar-tray-system-icon-tweaks) | m417z |
| `taskbar-notification-icon-spacing` | Tepsi simge genişliği, satır sayısı, taşma penceresi düzeni | [taskbar-notification-icon-spacing](https://windhawk.net/mods/taskbar-notification-icon-spacing) | m417z |
| `taskbar-count-badges` | Düğmelerde pencere sayısı rozeti veya noktalar | [taskbar-count-badges](https://windhawk.net/mods/taskbar-count-badges) | digART |
| `taskbar-thumbnail-size` | Görev çubuğu önizleme boyutu | [taskbar-thumbnail-size](https://windhawk.net/mods/taskbar-thumbnail-size) | m417z |
| `remove-context-menu-items` | Sağ tık menüsünden seçilen öğeleri kaldır (WinUI ve klasik menü) | [remove-context-menu-items](https://windhawk.net/mods/remove-context-menu-items) | Armaninyow |
| `hide-home-gallery-explorer` | Gezinti bölmesinden Giriş, Galeri, OneDrive ve özel öğeleri gizle | [hide-home-gallery-explorer](https://windhawk.net/mods/hide-home-gallery-explorer) | rinosaur681 |
| `desktop-icon-selection-style` | Seçili masaüstü simgesi vurgusu: köşe, dolgu, kenarlık, parıltı, renk | [desktop-icon-selection-style](https://windhawk.net/mods/desktop-icon-selection-style) | RiteshK |
| `transparent-desktop-icons-spotlight` | Boştayken soluk masaüstü simgeleri, fare gelince belirir | [transparent-desktop-icons-spotlight](https://windhawk.net/mods/transparent-desktop-icons-spotlight) | drgutman |
| `taskbar-styler` | Görev çubuğunu XAML stil kurallarıyla biçimlendirir; gömülü temalar: TranslucentTaskbar, DockLike, SimplyTransparent, Squircle, Matter, Surface, Luminosity (Dock/Classic/Compact); temalar görev çubuğunun gerçek yüksekliğine uyarlanır | [windows-11-taskbar-styler](https://windhawk.net/mods/windows-11-taskbar-styler) | m417z; temalar [windows-11-taskbar-styling-guide](https://github.com/ramensoftware/windows-11-taskbar-styling-guide) yazarları |

`taskbar-styler` XAML diagnostics bağlantısı kullanır: `ShadePatcher.dll` bu amaçla `DllGetClassObject` dışa aktarır ve
`InitializeXamlDiagnosticsEx` ile kabuğun görsel ağacına abone olur. Kural dili özgün modunkiyle aynıdır (hedef
seçiciler, `@GörselDurum`, `Özellik=değer`, `Özellik:=<XAML>`, `WindhawkBlur` fırçası), ek XAML kuralı girme alanı
kaldırılmıştır; yalnızca gömülü temalar kullanılır. Bazı temalar `taskbar-icon-size`, `taskbar-labels` ve
`taskbar-clock-customization` ile birlikte tasarlanmıştır; önerilen ayarlar `docs/ports/taskbar-styler.notes.md` içindedir.

### Özgün

| Mod | Ne yapar |
|-----|----------|
| `custom-start-menu` | ShadePatcher'ın kendi Başlat menüsü: sabitlenmiş ızgara, A-Z tüm uygulamalar, arama, son dosyalar, güç menüsü. Başlat düğmesini ve Windows tuşunu Open-Shell'in Windows 11 yöntemiyle devralır. [docs/startmenu.md](docs/startmenu.md) |

`desktop-toggle-icons` çakışma hakemini kullanır: masaüstü çift tıklamasını doğrudan yakalamaz, paylaşımlı
yüzeye abone olur. Her modun uyarlama notları (neyin bırakıldığı, nasıl doğrulanacağı) `docs/ports/` altındadır.

### Bilinen sorunlar (build 26200)

- `taskbar-labels`: `ITaskbarAppItemViewModel::HasLabel` sarmalayıcısının bu yapıdaki adı bulunamıyor; genişlik
  sınırları çalışır, "her zaman etiket" kipi çalışmaz. `sp_symbolprobe` ile yeni ad bulunup listeye eklenmeli.
- `hide-desktop-icon-text`: kısayol okları kalkar, ancak masaüstü yazıları bu yapıda DrawText/DrawThemeTextEx
  üzerinden çizilmiyor gibi görünüyor; yazı gizleme etkisiz.
- `start-menu-all-apps`, `win11-power-buttons`: Başlat menüsü bu yapıda StartMenuExperienceHost.exe içinde
  çalışıyor. Motorun artık bu süreç için bir hedefi var (`SP_TARGET_STARTMENU`, `engine/hostinject.c`), ancak bu
  iki mod henüz ona taşınmadı; şimdilik etkisiz. Ayar sayfasında not düşülmüştür.
- Başlat menüsünü sol kenarda açma (`taskbar-start-button-position` / `StartMenuOnLeft`) StartMenuExperienceHost.exe
  içinde çalışan ayrı bir mod girişidir (`start_menu_on_left.cpp`): mod açıkken kabuktaki motor çekirdek DLL'i o
  sürece yükler. Başlat süreci Windows tarafından dondurulmuşsa DLL menü bir sonraki açılışında devreye girer.
- `sp_inject /eject` ile çıkarma geliştirme içindir; motor iş parçacığı 15 sn içinde durmazsa kabuk çökebilir.
- Tepsi simgelerini ve görev çubuğu düğmelerini **oluşturulurken** biçimlendiren modlar (tepsi simge gizleme/aralığı,
  etiketler, rozetler, Başlat düğmesi konumu) motorun kabuktan önce yüklenmesini ister. Kurulu proxy bunu sağlar;
  kurulumsuz kullanımda ayar penceresindeki "Dosya Gezgini'ni yeniden başlat" yardımcısı explorer'ı askıda başlatıp
  motoru ilk komuttan önce yükler (erken enjeksiyon). Sonradan `sp_inject` ile yüklemek bu modları bir sonraki
  yeniden başlatmaya kadar etkisiz bırakır; bu yapıda taskbar.dll'deki `GetTaskbarHost` yolu olmadığı için var olan
  öğelere sonradan ulaşılamaz.

Yeni mod eklemek için [docs/writing-a-mod.md](docs/writing-a-mod.md).

## Mimari kararı

- `core` (explorer.exe içinde çalışır) C ile yazılır: loader lock, belgelenmemiş yapılar ve belirli ABI için en az
  sürpriz veren dil. Modlar kısıtlı C++ kullanır, bu da Windhawk modlarını uyarlamayı kolaylaştırır.
- `gui` ExplorerPatcher'dan taşınan C motorudur.
- Kurulum ve güncelleme gibi ayrı süreçte çalışan parçalar eklendiğinde C++ ile yazılabilir.

## Lisans ve kaynak

Ayar penceresi motoru [ExplorerPatcher](https://github.com/valinet/ExplorerPatcher) (GPL-2.0) projesinden
uyarlanmıştır; bu proje de aynı lisans altındadır. Kanca kütüphanesi
[KNSoft.SlimDetours](https://github.com/KNSoft/KNSoft.SlimDetours) (MIT). Başlat menüsünün yazı tipi
[Figtree](https://github.com/erikdkennedy/figtree) (SIL Open Font License 1.1, `src/core/startmenu/fonts/OFL.txt`);
DLL'e gömülüdür, sisteme kurulmaz. Başlat düğmesini ve Windows tuşunu devralma yöntemi
[Open-Shell](https://github.com/Open-Shell/Open-Shell-Menu)'den (MIT) öğrenilip yeniden yazılmıştır.

## İkon

Uygulama ikonu `design/icon` altındaki tasarımdan üretilir. Kullanılan varyant **3A koyu**: kendi arka planını
taşıdığı için hem açık hem koyu başlık çubuğunda okunur, adaçayı ve kum renkli kutucukları da 16 piksele
indiğinde birbirinden ayırt edilebilir kalır.

`tools/make_icon.py` tasarımdan `src/common/app.ico` üretir. Yaptığı tek biçim değişikliği köşeleri
yuvarlamaktır; kompozisyona dokunulmaz. On boyut üretilir (16'dan 256'ya), 128'e kadar olanlar 32 bit bitmap,
256 PNG olarak yazılır. İkon hem `ShadePatcher.dll` hem `sp_gui.dll` içine 1 numaralı kaynak olarak gömülür, bu
yüzden Dosya Gezgini de dosya ikonu olarak onu gösterir.

```
python tools/make_icon.py
```

Gereksinim: Pillow (`pip install pillow`).
