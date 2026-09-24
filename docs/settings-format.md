# settings.reg biçimi

Ayar penceresi `src/gui/resources/settings.reg` dosyasını satır satır okur. Dosya geçerli bir `.reg` dosyasıdır
(regedit ile içe aktarılabilir, "Varsayılan ayarlara dön" tam olarak bunu yapar); arayüzü tanımlayan her şey `;`
ile başlayan yorum satırlarındadır. Dosya CRLF satır sonları kullanmalıdır.

## Genel yapı

```
Windows Registry Editor Version 5.00

;M ShadePatcher            Pencere başlığı satırı (metin ürün adıyla değiştirilir)
;q

;T %R:1001%                1. sayfa (sol kenar çubuğunda görünür)
...sayfa içeriği...

;T %R:2001%                2. sayfa
...

;f                         Alt bilgi: bundan sonrası her sayfada en altta gösterilir
;u %R:2201% (*)
;restart
```

Metinlerde `%R:1234%` yazımı, `strings.h` içindeki 1234 numaralı dizeyle değiştirilir. Böylece dosya dilden
bağımsız kalır.

## Bir seçeneğin anatomisi

```
[HKEY_CURRENT_USER\Software\ShadePatcher]     Değerin yazılacağı anahtar
;b %R:1004%                                   Denetim türü ve etiketi
"ExampleToggle"=dword:00000000                Değer adı ve varsayılanı
```

Anahtar satırı her seçenekten önce tekrar yazılır. Değer satırı `dword:` (sayı) ya da `""` (metin) olabilir.
Etiketin sonundaki ` *`, seçeneğin Dosya Gezgini yeniden başlatılmadan etkili olmayacağını belirtir; ekranda
`(*)` olarak gösterilir.

## Direktifler

| Direktif | Anlamı |
|----------|--------|
| `;M metin` | Pencere başlık satırı. Metin ürün adıyla değiştirilir, önüne simge çizilir. |
| `;T metin` | Yeni sayfa başlatır; metin kenar çubuğunda görünür. |
| `;f` | Alt bilgi başlangıcı. Sonraki satırlar her sayfanın altında gösterilir. |
| `;q` | Erişilebilirlik için biriken başlık metnini sıfırlar. |
| `;a metin` | Alt başlık (önünde ➕ işareti). |
| `;e metin` | Düz metin satırı. `;e ` (boş) boş satır bırakır. |
| `;t metin` | Düz metin satırı (başlık birikimine katılmaz). |
| `;b metin` | Açma/kapama anahtarı. Sonraki satır `"Ad"=dword:0` ya da `1`. |
| `;i metin` | Ters anahtar: değer 0 iken açık görünür. |
| `;d metin` | Anahtarın varlığını açıp kapatır: açıkken önceki `[...]` anahtarı oluşturulur, kapalıyken silinir. |
| `;c N metin` | Açılır liste, N seçenek. Ardından N adet `;x değer etiket` satırı, sonra `"Ad"=dword:...`. |
| `;z N metin` | `;c` ile aynı, liste etiketin sonuna değil başına çizilir. `;z 10001 ...` + `"Language"` dil seçicidir. |
| `;w metin` | Metin girişi. Sonraki iki satır `;istem` ve `;varsayılan`, sonra `"Ad"=""`. İstem satırı `;@image` ise tıklayınca giriş kutusu yerine Windows dosya seçicisi (resim dosyaları) açılır ve yalnızca dosya adı gösterilir. |
| `;v metin` | Değer gösterimi (salt okunur). |
| `;u metin` | Eylem bağlantısı. Sonraki satır eylem adı: `;restart`, `;import`, `;export`, `;reset`, `;uninstall`, `;resetpins` (Başlat menüsünün sabitlenmişlerini varsayılana döndürür), `;resethidden`, `;clearhistory`, `;clearimage` (Başlat menüsünün gizlenen uygulamaları, arama geçmişi, arka plan resmi). |
| `;m metin` | Çoklu seçim listesi. Ardından `;x değer etiket` (işaretsiz) ya da `;X değer etiket` (işaretli) satırları, sonra `;eylemöneki` satırı. Bir öğe seçilince `eylemöneki` + değer adıyla `SP_HandleCustomMenuAction` çağrılır ve sayfa yeniden kurulur; menüde işaretlerin ne anlama geldiğine işleyici karar verir. Hazır menü öğeleri bununla eklenip kaldırılır. |
| `;y metin` | Köprü. Sonraki satır `;https://...` (ShellExecute ile açılır). Metne `🡕` eklemek dışa açıldığını gösterir. |
| `;l metin` | `;y` gibi ama altı çizilmez. |
| `;s Ad Koşul` | Koşullu bölüm başlangıcı. Koşul yanlışsa `;g Ad` satırına kadar atlanır. |
| `;g Ad` | Koşullu bölüm sonu. |
| `;p N` | Sonraki N satırı atlar (`USE_PRIVATE_INTERFACES` tanımlı değilse). |

### Koşullar

`src/gui/conditions.c` içindeki tabloda tanımlıdır; başına `!` konarak tersi alınır:

- `IsWindows10`, `IsWindows11`
- `IsWindows11Version22H2OrHigher`, `IsWindows11Version23H2OrHigher`

Bilinmeyen bir koşul bölümü gizlemez, hata ayıklama konsoluna yazar.

### Yer tutucular

`;e` / `;t` satırlarında kullanılabilir:

- `%VERSIONINFORMATIONSTRING%` sürüm satırı
- `%AUTHORSSTRING%` geliştirici satırı
- `%OSVERSIONSTRING%` çalışılan Windows sürümü
- `%ENGINESTATUSSTRING%` motorun explorer.exe içinde olup olmadığı (kurulu proxy / elle yüklenmiş / yok);
  her çizimde `FindEngineInShell` ile yeniden bakılır

### Sanal değerler

Kayıt defterinde tutulmayan (örneğin bir Windows API çağrısıyla okunup yazılan) ayarlar için değer adı
`Virtualized_{CLSID}_Ad` biçiminde yazılır ve satır `;` ile yoruma alınır:

```
;b %R:1234%
;"Virtualized_{7A2C3F1E-4B8D-4E62-9C5A-1F0D6B8E2A47}_Ad"=dword:00000000
```

Okuma ve yazma `src/gui/registry.c` içindeki `g_virtualValues` tablosuna yönlendirilir. Dışa aktarılan dosyada
da yorum olarak kalırlar; içe aktarmada ve sıfırlamada elle yeniden uygulanırlar.

## Yeni sayfa eklemek

1. `strings.h` içinde sayfa için 100'lük yeni bir kimlik bloğu ayırın (ör. 1101-1199).
2. Metinleri `lang/gui.en-US.rc` ve `lang/gui.tr-TR.rc` dosyalarına ekleyin.
3. `settings.reg` içine `;T %R:1101%` ile sayfayı ve altına seçenekleri ekleyin.
4. Sayfa sayısı en fazla 20'dir (`GUI.sectionNames`).
