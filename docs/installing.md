# Kurulum, kaldırma ve bir şey ters giderse

## Kurulum ne yapıyor

Windows'un "kodumu kabuğun içinde çalıştır" diye desteklediği bir yol yok. Bu ürünün kullandığı yol yükleyicinin
kendi arama sırası:

- Bir programın kendi klasörü System32'den önce aranır.
- explorer.exe `C:\Windows` içindedir.
- Dolayısıyla `C:\Windows\dxgi.dll` adında bir dosya, explorer tarafından gerçeğinin yerine yüklenir.

O dosya `ShadePatcher.dll`'in bir kopyasıdır. Mod motorunu başlatır ve bütün DXGI çağrılarını
`System32\dxgi.dll`'e iletir.

Kurulum dört şey bırakır:

| Yer | Ne |
|-----|-----|
| `%ProgramFiles%\ShadePatcher\` | Ürün dosyaları ve kaldırma için kurulum programının kendisi |
| `C:\Windows\dxgi.dll` | `ShadePatcher.dll` kopyası; explorer'ın yüklediği dosya |
| `HKLM\...\Uninstall\ShadePatcher` | Ayarlar > Uygulamalar listesinde görünmesi için |
| `%ProgramData%\...\Start Menu\Programs\ShadePatcher Ayarları.lnk` | Başlat'ta "ShadePatcher" yazınca ayarların bulunması için (İngilizce sistemde "ShadePatcher Settings") |

`sp_setup.exe` tek başına yeterlidir: `ShadePatcher.dll` ve `sp_gui.dll` içine kaynak (RCDATA 101/102) olarak
gömülüdür. Yanında aynı adlı dosyalar varsa (derleme klasöründen çalıştırıldığında) onlar tercih edilir, böylece
yeni derlenen bir DLL kurulum programı yeniden derlenmeden kurulabilir.

Kurulumsuz kullanımda (sp_inject ya da yeniden başlatma yardımcısıyla derleme klasöründen yüklenen motor) aynı
kısayolu motor, kullanıcının kendi Başlat menüsüne, yüklendiği DLL'i gösterecek şekilde kendisi yazar. Kaldırma
her iki kısayolu da siler.

### Etki alanı ne kadar geniş

`C:\Windows` diğer programların da varsayılan arama yolunda. Ama System32 ondan **önce** geliyor, yani başka her
süreç gerçek `dxgi.dll`'i buluyor. Yalnızca kendi klasörü `C:\Windows` olan bir program bu dosyayı alır ve pratikte
o da explorer.exe'dir.

## Kurmak

Yönetici hakkı gerekir (`C:\Windows` ve Program Files'a yazıyor). Kurulum programı manifestinde bunu istediği
için çift tıklandığında normal UAC penceresi çıkar.

```
build\bin\Release\sp_setup.exe
```

Kurulum sırasında Dosya Gezgini yeniden başlatılır; bu zaten motorun yüklenmesi için gerekli.

## Kaldırmak

Ayarlar > Uygulamalar listesinden ya da doğrudan:

```
"%ProgramFiles%\ShadePatcher\sp_setup.exe" /uninstall
```

Kullanıcı ayarları (`HKCU\Software\ShadePatcher`) bilerek silinmez, yeniden kurarsanız geri gelir.

`/quiet` her iki işlemde de pencere göstermez.

## Kabuk neden sonlandırılıyor

`C:\Windows\dxgi.dll` explorer tarafından açık tutulur, yani yazılmadan veya silinmeden önce explorer'ın gitmesi
gerekir. Kurulum programı kabuğu **sonlandırır**, nazikçe çıkmasını istemez. Aradaki fark önemli:

- Sonlandırılan kabuğu Windows kendisi geri getirir, hem kullanıcının kendi oturumunda hem de yönetici hakkı
  olmadan. İstenen budur.
- Nazikçe çıkması istenen kabuk geri gelmez. Kurulum programı onu kendisi başlatsa yönetici haklarıyla açılır,
  bu da kullanıcıya yükseltilmiş bir masaüstü bırakır.

Kabuğun geri gelmesi bir an sürdüğü için dosya işlemleri birkaç saniye boyunca yeniden denenir. Yine de olmazsa
değişiklik bir sonraki yeniden başlatmaya kuyruklanır ve kullanıcıya söylenir.

Masaüstü kendiliğinden gelmezse kurulum programı uyarır. Elle açmak için: Ctrl+Shift+Esc, Yeni görev çalıştır,
`explorer.exe`.

## Bir mod kabuğu çökertirse

Motorda çökme döngüsü koruması var (`src/core/engine/safemode.c`).

Motor her kabuk başlangıcını kaydeder. Kabuk **60 saniye içinde üç kez** başlarsa motor kendisini sorumlu sayar,
o oturumda **hiçbir modu yüklemez** ve bunu kayıt defterine yazar. Masaüstü geri gelir, ayar penceresi normal
açılır, kullanıcı suçlu modu kapatabilir ya da kaldırabilir.

Güvenli kipte ayar değişiklikleri o oturumda uygulanmaz; az önce çökmeye yol açan modu hemen geri yüklemek
kullanıcıyı doğrudan döngüye sokardı. Değişiklik bir sonraki kabuk başlangıcında geçerli olur.

Kabuk 60 saniye ayakta kalırsa sayaç sıfırlanır, yani sıradan bir oturum kapatma açma koruma tetiklemez.

İlgili değerler, `HKCU\Software\ShadePatcher`:

| Değer | Anlamı |
|-------|--------|
| `SafeMode` | 1 ise son başlangıçta hiçbir mod yüklenmedi |
| `ShellStartCount` | Pencere içindeki ardışık başlangıç sayısı |
| `ShellLastStart` | Son başlangıcın zamanı (FILETIME) |

## Kurmadan önce doğrulama

İki denetim var, ikisi de kabuğa dokunmadan çalışır:

```
build\bin\Release\sp_selftest.exe     motoru ve modları sınar
python tools\check_proxy.py           proxy'nin gerçek dxgi.dll'e ilettiğini doğrular
```

`check_proxy.py` özellikle önemli: explorer proxy'yi gerçek `dxgi.dll` yerine yüklüyor, bu yüzden gerçeğe
ulaşmayan bir çağrı kabuğun çizim yapmasını engeller. Betik derlenmiş DLL'i yükler ve explorer'ın yaptığı çağrının
aynısını yapar.

Dışa aktarımların eksiksiz olduğunu da doğrulamak gerekir; eksik bir dışa aktarım explorer'ın DLL'i hiç
yükleyememesine yol açar:

```
dumpbin /EXPORTS C:\Windows\System32\dxgi.dll
dumpbin /EXPORTS build\bin\Release\ShadePatcher.dll
```

İkincisi birincinin tamamını, artı `ZZGUI`'yi içermelidir.
