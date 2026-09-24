# ShadePatcher Başlat menüsü

`custom-start-menu` modu, Windows'un Başlat menüsünün yerine ShadePatcher'ın kendi menüsünü açar. Menü
explorer.exe içinde, motorun bir modu olarak çalışır; ayrı bir süreç, servis ya da tarayıcı motoru yoktur.

Tasarım `design/startmenu/` altındaki teslim paketindendir: `README.md` belirtimdir (ölçüler, renkler, animasyon
eğrileri, klavye davranışı), `Start Menu.dc.html` tarayıcıda açılabilen etkileşimli referanstır,
`StartMenuTokens.h` tokenlardır. Koddaki yorumlar bu README'nin bölüm numaralarını anar (ör. "5.3").

## Katmanlar

| Dosya | İş |
|-------|-----|
| `src/core/mods/custom_start_menu.cpp` | Mod: ayarlar, yaşam döngüsü, `ShellRegisterHotKey` kancası |
| `src/core/startmenu/host.*` | Menü iş parçacığı; Başlat düğmesi ve Windows tuşu kancaları |
| `src/core/startmenu/view.*` | Pencereler, DirectComposition görsel ağacı, Direct2D çizimi, animasyon, klavye ve fare |
| `src/core/startmenu/catalog.*` | Uygulama listesi, ikonlar, sabitlenmişler, son dosyalar, kullanıcı, başlatma, güç |
| `src/core/startmenu/text.*` | Figtree yazı tipi, metin biçimleri, menünün kendi metinleri (Türkçe/İngilizce) |
| `src/core/startmenu/tokens.h` | Tasarım tokenları (renk, ölçü, tipografi, hareket) |

## Başlat'ı devralmak (Open-Shell yöntemi)

Windows 11'de Open-Shell'in kullandığı yol, bu motorun API'siyle yeniden yazıldı:

- **Başlat düğmesi.** Görev çubuğunun iş parçacığına bir `WH_MOUSE` kancası kurulur. Görev çubuğu, XAML Başlat
  düğmesinin yerini gösteren gizli bir `Start` penceresi tutar (`Shell_TrayWnd` çocuğu); bu alana düşen sol tuş
  mesajları XAML'a ulaşmadan yutulur ve menüye "aç/kapat" gönderilir. Dokunma ve kalem `WM_POINTER*` olarak
  geldiği için aynı iş `WH_GETMESSAGE` kancasında da yapılır. Shift+tıklama (ayar açıksa) yutulmaz, Windows'un
  menüsü açılır.
- **Windows tuşu ve Ctrl+Esc.** twinui.dll bu tuşları user32'nin belgelenmemiş `ShellRegisterHotKey`'i (sıra
  no. 2671) ile kabuk kısayolu olarak kaydeder. Mod bu işlevi kancalar ve Win ile Ctrl+Esc kayıtlarını reddeder;
  motor kabuktan sonra yüklendiyse kayıtlar, kaydı yapan iş parçacığında bir APC ile geri alınır (kimlik 1 ve 2).
  Kısayol olmayınca Windows klasik davranışa döner ve kabuk penceresine (Progman) `WM_SYSCOMMAND` / `SC_TASKLIST`
  gönderir. Progman'ın iş parçacığındaki `WH_GETMESSAGE` kancası bu mesajı menüye çevirir.
- Kancalar menünün kendi iş parçacığından kurulur ve o iş parçacığı bitmeden sökülür (Windows kancayı kuran iş
  parçacığına bağlar). Görev çubuğu yeniden kurulursa (`TaskbarCreated`) kancalar yenilenir.
- Mod kapatılınca: reddedilen kayıtlar kabuğa geri verilir. APC ile geri alınanlar geri verilemez (twinui'nin
  bağımsız değişkenleri bilinmez) ama gerek de yoktur: `SC_TASKLIST` artık engellenmediği için Windows tuşu
  Windows'un menüsünü açar. Kabuk bir sonraki başlangıçta kayıtları zaten kendisi yapar.

Kancaların günlük satırları (`Logging` açıkken): `Start button hooked on the taskbar thread`,
`Windows key routed through the shell window's thread`, `ShellRegisterHotKey(id ...)`.

## İki pencere

Panelin gölgesi yanlara 80, alta 110 DIP taşar. Gölgeyi taşıyan tek bir pencere, gölgenin altındaki her tıklamayı
(menünün hemen altındaki görev çubuğu dahil) yutardı. Bu yüzden menü iki penceredir:

- **Görsel pencere:** panel + gölge payı, `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`. Sistem onu
  isabet testinde atlar; görünen her şey DirectComposition ile ona çizilir.
- **Giriş penceresi:** tam panel boyutunda, `WS_EX_NOREDIRECTIONBITMAP` ve içeriksiz. Görünmez ama tıklamaları,
  klavye odağını ve etkinleşmeyi alır.

## Çizim ve animasyon

Görsel ağaç tasarım README'sinin 4.3 bölümündeki gibidir (`view.h` başındaki şema). Önemli kararlar:

- **WARP aygıtı.** Direct3D aygıtı yazılım (WARP) sürücüsüyle oluşturulur. Menü seyrek ve az çizer; animasyonları
  hangi aygıt çizmiş olursa olsun DWM GPU'da oynatır. Bu makinede (NVIDIA) donanım aygıtı kabukta 20-35 MB özel
  bellek tutarken WARP yaklaşık 1,5 MB tutar; GPU sıfırlanması da onu etkilemez. Donanım aygıtı yalnızca yedektir.
- Vurgu (hover/seçim) tek bir görseldir, taşınır ve 80 ms'de belirir; içerik yeniden çizilmez. Kaydırma yalnızca
  bir görselin konumunu değiştirir. İçerik bir sanal yüzeydedir ve görünen alanın bir ekran üstü/altı kadarı
  çizilir; menü kapanınca bu yüzeyin belleği bırakılır.
- Açılış/kapanış eğrileri (CSS cubic-bezier) DirectComposition'a sekiz Hermite parçası olarak verilir. Yarıda
  kesilen bir kapanış, menü yeniden açılırsa o anki değerden geri döner.
- Sistemde animasyonlar kapalıysa (`SPI_GETCLIENTAREAANIMATION`) süreler sıfırdır.

Ölçüm (bu makine, 150 %): ilk açılış 26,5 ms (gölge bir kez hesaplanır), sonraki açılışlar 5,8 ms.

## Veri

Uygulama listesi, ikonlar, son dosyalar ve kullanıcı bilgisi ayrı bir yükleyici iş parçacığında hazırlanır;
menü açılırken G/Ç yapılmaz. Liste 60 saniyeden eskiyse menü kapanırken arka planda yenilenir; daha önce çözülmüş
ikonlar yeniden okunmaz.

- Uygulamalar: `shell:AppsFolder` (Win32 ve UWP birlikte), kullanıcının diline göre sıralanır; harf başlıkları
  dilin kurallarına uyar (Türkçede Ç ayrı harftir, İngilizcede É, E altına girer).
- Arama: büyük/küçük harf duyarsız "içerir"; önce baştan eşleşenler, sonra kelime başı, sonra içerenler; aksansız
  yazım da eşleşir ("cizim" → "Çizim").
- Sabitlenmişler: `%LOCALAPPDATA%\ShadePatcher\pinned.json` (uygulama kimliklerinin JSON dizisi). Dosya yoksa
  kurulu olan bilinen uygulamalardan 15 tanesiyle başlar. Sağ tık menüsünde "Başlat'a sabitle", "Başlat'tan
  kaldır", "Başa taşı" ve kabuğun kendi öğeleri (yönetici olarak çalıştır, dosya konumu, kaldır...) vardır;
  kabuğun kendi "Başlat'a sabitle" öğesi Windows'un menüsüne sabitlediği için çıkarılır.
- Son dosyalar: `Recent` klasöründeki kısayolların en yenileri. Windows'ta "Son açılan öğeleri göster" kapalıysa
  şerit hiç görünmez. Ağ yolları yoklanmaz (bağlantısı kopmuş bir paylaşım yükleyiciyi bekletmesin diye).
- Ctrl+Shift+Enter ya da Ctrl+Shift+tıklama uygulamayı yönetici olarak başlatır.

## Arama, en çok kullanılanlar, kısayollar

Arama kutusu uygulamaların yanında şunları da bulur (`startmenu/search.cpp`); her biri ayrı ayrı kapatılabilir:

- **Hesap makinesi.** `12x7`, `(12+3)*7/2`, `2^10`, `=15%4`; ondalık için virgül ya da nokta. Sonuç en üstte,
  Enter panoya kopyalar.
- **Çalıştır.** Çalıştır penceresinin anladığı her şey: `%appdata%`, `%temp%\klasör`, `~\Downloads`,
  `C:\Windows`, `shell:startup`, `cmd`, `regedit`, `devmgmt.msc`, `notepad C:\a.txt`, `https://...`, `www...`.
  Ortam değişkenleri açılır, program PATH'te ve App Paths kayıtlarında aranır. Açıkça bir yol ya da komut
  olduğunda (değişken, yol, bağımsız değişken, uzantı) satır en üste çıkar; yalnızca bir program adıysa
  uygulamaların altında "Çalıştır" bölümünde durur. Ctrl+Shift+Enter yönetici olarak çalıştırır. Ağ yolları
  yoklanmaz.
- **Ayarlar.** 43 Ayarlar sayfası, Türkçe ve İngilizce adları ve anahtar kelimeleriyle ("ekran", "bluetooth",
  "koyu mod", "güncelleme"). Tablo `tools/gen_startmenu_settings.py` ile üretilir (`settings_table.inc`);
  Türkçe karakterler derleme kod sayfası yüzünden kaçışlı yazılır, tabloyu betikten düzenleyin.

**En çok kullanılanlar.** Menüden açılan her uygulama `%LOCALAPPDATA%\ShadePatcher\usage.txt` içinde sayılır;
sabitlenmemiş olanların en çok açılanları ızgaranın altında liste olarak görünür.

**Sürükle-bırak.** Sabitlenmiş bir uygulama tutulup sürüklenince hücre imleci izler, bırakılacağı yer vurgulanır;
bırakınca sıra `pinned.json`'a yazılır. Esc sürüklemeyi iptal eder.

**Dosyalar.** Aramada, Windows Search dizininden kullanıcı klasöründeki dosya ve klasörler (`startmenu/filesearch.cpp`):
dizinin OLE DB sağlayıcısına (`Search.CollatorDSO`) SQL ile, kendi iş parçacığında ve yazma durduktan 150 ms sonra
sorulur; yalnızca son sorgunun cevabı gösterilir. AppData hariç tutulur.

**Harf dizini.** "Tüm uygulamalar"da bir harf başlığına tıklayınca harf karoları açılır, seçilen harfe atlanır.

**Yeni uygulamalar.** `known.txt` ilk görülen listeyi taban kabul eder; sonradan gelenler bir hafta ya da ilk
açılışlarına kadar "Yeni" işaretlidir ve "Tüm uygulamalar"ın başında listelenir.

**Atlama listesi.** Sağ tık menüsünde uygulamanın son belgeleri (`IApplicationDocumentLists`, uygulama kimliğiyle).

**Arka plan resmi**, **zamanlanmış tema**, **güç menüsü ekleri** (güncelleme bekliyorsa "Güncelle ve yeniden
başlat/kapat", "Gelişmiş başlangıç", "BIOS/UEFI'ye yeniden başlat"; son ikisi yönetici izni ister) ve **Başlat'a
orta tık** (Tüm uygulamalar, Dosya Gezgini, Görev Yöneticisi, Ayarlar) ayar sayfalarından seçilir. Değer adları:
`SearchFiles`, `FileCount`, `ShowNewApps`, `BackgroundImage`, `ImageBlur`, `ImageTint`, `Theme`=6 ile
`DarkFrom`/`LightFrom`, `PowerUpdates`, `PowerAdvanced`, `PowerFirmware`, `MiddleClick`.

**Hızlı ayarlar.** Son dosyaların üstünde Wi-Fi, Bluetooth, ses ve koyu mod düğmeleri (`startmenu/quick.cpp`):
radyolar `Windows.Devices.Radios`, ses varsayılan çıkışın `IAudioEndpointVolume` arayüzüyle, kendi iş parçacığında.
Tıklama açar/kapar; ses düğmesinin üzerinde tekerlek ses düzeyini değiştirir. Donanımı olmayan düğme görünmez.

**Açık pencereler ve arama geçmişi.** Aramada Alt+Tab'ın gösterdiği pencerelerin başlıkları da aranır, Enter o
pencereye geçer. Bir sonuca götüren aramalar `history.txt` içinde tutulur; boş arama kutusuna tıklayınca son
aramalar listelenir.

**Listeden gizleme.** Sağ tıktaki "Listeden gizle" bir uygulamayı tüm listelerden ve aramadan çıkarır
(`hidden.txt`); ayar sayfasındaki bağlantı hepsini geri getirir.

**Oyun koruması.** Tam ekran bir oyun, video ya da sunum öndeyken (`SHQueryUserNotificationState`) Windows tuşu
yok sayılır; hiçbir Başlat menüsü açılmaz. Başlat düğmesi her zaman çalışır.

**Alt çubuk kısayolları.** Dosya Gezgini, Belgeler, İndirilenler, Resimler, Müzik, Videolar, kişisel klasör,
Ayarlar; güç düğmesinin solunda, üzerine gelince adı ipucu olarak çıkar.

**Ok tuşları** artık düzenden bağımsızdır: her yönde o yöndeki en yakın öğeye gidilir, bu yüzden ızgaradan
listeye (ör. en çok kullanılanlar, arama bölümleri) geçiş doğaldır.

## Ayarlar

`HKCU\Software\ShadePatcher\Mods\custom-start-menu`. Ayar penceresinde beş sayfa: **Başlat menüsü** (genel ve
açılış), **Başlat › Görünüm**, **Başlat › Düzen**, **Başlat › Davranış**, **Başlat › Arama ve kısayollar**. Okuma tek yerde yapılır
(`startmenu/host.cpp`, `ReadHostSettings`); tanınmayan bir değer varsayılana döner. Değişiklikler menünün bir
sonraki açılışında görünür; yeniden başlatma gerekmez.

| Değer | Varsayılan | Anlamı |
|-------|------------|--------|
| `Enabled` | 0 | Menü açık |
| `Position` | 0 | 0 görev çubuğu hizasını izle, 1 ortada, 2 solda |
| `EdgeGap` | 12 | Görev çubuğuna/ekran kenarına uzaklık (DIP): 0, 4, 8, 12, 16, 24, 32 |
| `DefaultView` | 0 | Açılışta 0 sabitlenmişler, 1 tüm uygulamalar |
| `ShowRecent` | 1 | Son dosyalar şeridi |
| `StartButton`, `WinKey`, `ShiftClickWindows` | 1 | Başlat düğmesi / Win ve Ctrl+Esc bu menüyü açar; Shift+tık Windows'unkini |
| `Theme` | 0 | 0 Windows'u izle, 1 açık, 2 koyu, 3 gece mavisi, 4 grafit, 5 kum |
| `Background` | 0 | 0 opak, 1 akrilik (bulanık), 2 yarı saydam cam (bulanıksız) |
| `Opacity` | 80 | Akrilik/cam tonunun yoğunluğu (%) |
| `Accent` | 0 | 0 temanın rengi, 1 Windows vurgu rengi, 2-7 mavi, mor, mercan, pembe, kehribar, camgöbeği |
| `CornerRadius` | 16 | Köşe yarıçapı (DIP) |
| `Shadow` | 1 | Gölge |
| `Font` | 0 | 0 Figtree, 1 Segoe UI Variable, 2 Segoe UI |
| `FontSize` | 100 | Yazı boyutu (%) |
| `Scale` | 100 | Menünün tamamının boyutu (%) |
| `Columns` | 5 | Izgara sütunu; panel genişliği buna göre değişir |
| `IconSize` | 44 | Izgara simgesi (DIP); hücre simgeyle büyür |
| `ShowLabels` | 1 | Izgarada adlar |
| `ListIconSize` | 28 | Tüm uygulamalar listesinde simge (DIP) |
| `MaxHeight` | 600 | En fazla yükseklik (DIP) |
| `ShowSearch`, `ShowTitle`, `ShowFooter`, `ShowAccount`, `ShowUserName`, `ShowPower` | 1 | Görünen öğeler. Arama kutusu gizliyken yazınca belirir |
| `Animation` | 0 | 0 Windows'u izle, 1 açık, 2 hızlı, 3 kapalı |
| `OpenStyle` | 0 | 0 kayarak ve büyüyerek (tasarım), 1 yalnızca belirerek |
| `PowerLock`, `PowerSleep`, `PowerRestart`, `PowerShutDown` | 1 | Güç menüsü öğeleri |
| `PowerSignOut`, `PowerHibernate` | 0 | Güç menüsü öğeleri |
| `ShowMostUsed`, `MostUsedCount` | 1, 4 | En çok kullanılanlar listesi ve uzunluğu |
| `SearchSettings`, `SearchCalculator`, `SearchCommands` | 1 | Aramada Ayarlar sayfaları, hesap makinesi, Çalıştır |
| `ShortcutExplorer`, `ShortcutDocuments`, `ShortcutDownloads`, `ShortcutSettings` | 1 | Alt çubuk kısayolları |
| `ShortcutPictures`, `ShortcutMusic`, `ShortcutVideos`, `ShortcutUserFolder` | 0 | Alt çubuk kısayolları |
| `ResetPins` | 0 | Ayar penceresindeki bağlantı 1 yazar; menü varsayılan sabitlenmişleri geri koyup 0'a çeker |

**Akrilik.** DirectComposition pencerenin arkasını örnekleyemez; bu yüzden akrilik, aynı pencereye bağlanan ayrı
bir Windows.UI.Composition ağacıdır (`startmenu/backdrop.cpp`): `HostBackdropBrush` ve pencerede
`DWMWA_USE_HOSTBACKDROPBRUSH`. Bu ağaç DirectComposition ağacının altında durur, panelin yarı saydam tonu onun
üstüne çizilir, açılış animasyonu iki ağaca aynı eğrilerle verilir. Windows'ta "Saydamlık efektleri" kapalıysa
opak arka plana dönülür.

## Geliştirme: önizleme

Menü kabuğu yeniden başlatmadan, kancasız olarak herhangi bir süreçte açılabilir:

```
rundll32 build\bin\Release\ShadePatcher.dll,ZZStartMenuPreview [all] [power] [left] [recent] [search=metin] [bench]
```

`all` tüm uygulamalar listesini, `power` güç menüsünü açar; `left` sola yerleştirir; `recent` Windows'un izleme
ayarına bakmadan son dosyaları gösterir; `search=` aramaya yazar; `bench` sıcak açılışı da ölçer. Kayıttaki ayarlar okunur; `dark`, `light`, `acrylic`, `opaque`, `theme=N`, `bg=N`, `cols=N`, `icon=N`, `fontsize=N`, `accent=N`, `radius=N`, `opacity=N`, `nolabels`, `nosearch`, `notitle`, `nofooter`, `noname` bunları geçersiz kılar. Açılış süreleri
`%TEMP%\ShadePatcher-startmenu-preview.txt` dosyasına yazılır. Menü kapanınca süreç biter.

## Sınırlar

- Tasarım paketi yalnızca açık temayı tanımlar; koyu ve diğer temalar aynı yapının (yüzey üstünde aynı mürekkep
  saydamlıkları) başka renklerle kurulmuş hâlidir (`tokens.h`).
- İkincil monitörlerdeki görev çubuklarında `Start` penceresi yoksa oradaki Başlat düğmesi Windows'un menüsünü
  açar; Windows tuşu her zaman çalışır.
- Ekran okuyucu desteği (UI Automation) henüz yok.
