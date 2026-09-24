# Mod motoru

Motor `src/core/engine` altındadır ve explorer.exe içinde çalışır. Görevi, modların Windows'un iç işleyişine
güvenli biçimde dokunmasını sağlamak: kanca kurmak, belgelenmemiş fonksiyonları bulmak, ayarları okumak ve
birden çok modun aynı hareketi istediği durumları çözmek.

Tasarım ExplorerPatcher ve Windhawk'ın ikisinden de yararlanır. Hangi fikrin nereden geldiği aşağıda yazılıdır.

## Katmanlar

| Dosya | İş |
|-------|-----|
| `manager.c` | Mod yaşam döngüsü, hangi modun bu süreçte çalışacağına karar verme, ayar izleme |
| `hooks.c` | İşlem içi kanca (inline hook), işlem tabanlı, mod sahipliğiyle |
| `symbols.cpp` | PDB indirme, sembol çözümleme, kayıt defteri önbelleği |
| `settings.c` | Mod ayarlarının okunması ve değişiklik bildirimi |
| `input.c` | Paylaşımlı hareketlerin hakemliği |
| `surfaces.c` | Paylaşımlı yüzeylerin (masaüstü, görev çubuğu) izlenmesi |
| `log.c` | Hata ayıklayıcıya ve dosyaya günlük |
| `modapi.h` | Modların çağırabileceği her şey |

## explorer.exe'ye giriş

Windows'un "kodumu kabuğun içinde çalıştır" diye desteklenen bir yolu yok. ExplorerPatcher'ın çözümü kullanılıyor
(`src/core/dxgi_proxy.c`): kurulum DLL'i `C:\Windows\dxgi.dll` olarak kopyalar. explorer.exe `C:\Windows` içinde
olduğu için yükleyici System32'den önce kendi klasörüne bakar ve bu dosyayı yükler. `DllMain` motoru bir iş
parçacığında başlatır, DLL'in tüm dışa aktarımları gerçek `System32\dxgi.dll`'e iletilir.

Uzak iş parçacığı, enjeksiyon veya sürücü gerekmez. Bedeli, DXGI kullanan her sürecin bu DLL'i yüklemesidir; bu
yüzden `DllMain` süreç adını kontrol eder ve yalnızca explorer.exe için iş parçacığı açar.

## Kanca katmanı

SlimDetours üzerine kuruludur. Windhawk da MinHook uyumlu kabuğunun altında aynı kütüphaneyi kullanır; Detours
tabanlıdır ve x64'te güvenilirdir.

Her kanca bir **işlem** içinde uygulanır ve işlem diğer iş parçacıklarını askıya alır. Bir iş parçacığı, bir
fonksiyonun ilk baytları yeniden yazılırken o fonksiyonun ortasında çalışıyor olamaz. Bir grup kanca ya tamamen
uygulanır ya hiç uygulanmaz.

Her kanca, isteyen modun kimliğiyle kaydedilir. Bir mod kapatıldığında yalnızca onun kancaları sökülür.
ExplorerPatcher'da kancalar süreç ömrü boyunca kalır; burada mod açılıp kapanabilir.

## Sembol katmanı

Kabuğun çoğu dışa aktarılmaz. `winrt::SystemTray::implementation::VolumeSystemTrayIconDataModel::OnIconClicked`
gibi bir fonksiyonun adı yalnızca Microsoft'un yayımladığı PDB dosyasında vardır.

Üç aşama, ucuzdan pahalıya:

1. **Kayıt defteri önbelleği.** `HKCU\Software\ShadePatcher\SymbolCache\<modül adı>` altında, anahtar modülün PDB
   kimliğidir (GUID + age). Windows güncellemesi kimliği değiştirir, dolayısıyla eski bir konum yeni bir yapıya
   karşı asla kullanılamaz.
2. **Yerel PDB.** `%LOCALAPPDATA%\ShadePatcher\symbols` altında.
3. **İndirme.** `msdl.microsoft.com` üzerinden WinHTTP ile.

Windhawk önbelleği mod başına tutar; burada **modül başına** tutulur ve tüm modlar paylaşır. Aynı DLL'den sembol
isteyen ikinci mod hiçbir bedel ödemez.

Ölçüm (bu makinede, user32.dll): ilk arama 2,3 saniye (indirme dahil), önbellekten arama 0 ms.

Sembol adları DIA'nın ürettiği biçimle eşleşsin diye çözümleme `UnDecorateSymbolNameW` ile `0x20800` bayraklarıyla
yapılır. Bu sayede Windhawk modlarındaki ad dizeleri değiştirilmeden kullanılabilir.

**Güvenlik.** Hem önbellek anahtarı hem sembol klasörü yazılabilir. Modül görüntüsünün dışına düşen bir konum
reddedilir. Bu kontrol olmadan yerleştirilmiş bir kayıt, kancayı saldırganın seçtiği bir adrese yönlendirebilirdi.
Aynı korumayı Windhawk da uygular.

## Çakışma hakemi

İki mod aynı hareketi isteyebilir. Biri masaüstüne çift tıklayınca simgeleri gizler, diğeri Görev Yöneticisi'ni
açar. Windhawk'ta her mod masaüstünü kendisi subclass eder, ikisi de tıklamayı görür ve ikisi de çalışır.

Burada yüzey motora aittir. Kaç mod ilgilenirse ilgilensin yüzey başına tek bir subclass kurulur ve hareket
abonelere öncelik sırasıyla verilir. `TRUE` döndüren ilk abone hareketi tüketir, arkasındakiler onu hiç görmez.

Öncelik modun `InputPriority` ayarından gelir (küçük olan önce çalışır, varsayılan 100). Yani kazananı yükleme
sırası değil, kullanıcı belirler.

Yüzeyler:

| Yüzey | Durum |
|-------|-------|
| `SP_SURFACE_DESKTOP` | Çalışıyor. Çift tıklama, orta tıklama, tekerlek. İkon üzerindeki tıklamalar kabuğa bırakılır. |
| `SP_SURFACE_TASKBAR_EMPTY` | Henüz yok; ilk görev çubuğu moduyla gelecek. |

## Yaşam döngüsü

```
Init()             Mod kancalarını kurar. FALSE dönerse mod yüklenmez.
AfterInit()        Tüm modlar hazır, tüm kancalar canlı. Mevcut pencerelere dokunmak için doğru yer.
SettingsChanged()  Ayar değişti. Yeniden başlatmadan uygula.
BeforeUninit()     Hâlâ kancalı. AfterInit'in yaptıklarını geri al.
Uninit()           Kancalar zaten söküldü. Kalan kaynakları bırak.
```

Motor önce bütün modların `Init`'ini çağırır, sonra hepsinin `AfterInit`'ini. Bu sayede `AfterInit` içinde mevcut
pencereleri tarayan bir mod, o sırada diğer modların kancalarının yeni pencereleri zaten yakaladığından emin
olabilir. Windhawk'ın sırası da budur.

**İş parçacığı kuralı.** Beş geri çağırma da motor iş parçacığında, tek seferde tek mod olacak şekilde çalışır;
bir mod kendi durumunu bunlara karşı korumak zorunda değildir. Kanca gövdesi ise kancalanan fonksiyonu çağıran
hangi iş parçacığıysa onda çalışır. Kancayla paylaşılan durum bu yüzden `std::atomic` ya da kilit ister.

## Ayarlar

```
HKCU\Software\ShadePatcher
    Logging, LogToFile                  motor geneli
HKCU\Software\ShadePatcher\Mods\<mod id>
    Enabled        dword                0 ise mod yüklenmez
    InputPriority  dword                paylaşımlı hareket sırası
    <ayar adı>     dword/sz             modun kendi ayarları
```

Ayar penceresi aynı değerleri yazar, yani ürünün iki yarısı tek bir depoyu paylaşır. Motor
`RegNotifyChangeKeyValue` ile anahtarı izler; bir değer değişince ilgili mod anında yüklenir, kaldırılır ya da
`SettingsChanged` alır. Kabuğu yeniden başlatmak gerekmez.

## Doğrulama

`tools/selftest` motor kaynaklarını bir konsol programına bağlar ve katmanları explorer.exe'ye dokunmadan
sınar: kanca trampolinleri, sembol çözümleme ve önbelleği, hakem sıralaması ve tüketimi, ayar okuma.

```
build\bin\Release\sp_selftest.exe
```

Kabuğa herhangi bir şey kurmadan önce bunun geçmesi beklenir.
