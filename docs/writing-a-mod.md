# Mod yazmak

Bir mod, `src/core/mods` altında tek bir kaynak dosyasıdır. Kendi kimliğini tanımlar, motorun API'sini
kullanır ve `mod_table.c` içinde listelenir. Başka hiçbir yere dokunmak gerekmez.

## İskelet

```cpp
#define SP_MOD_ID "ornek-mod"
#include "engine/modapi.h"

namespace {

BOOL Init()
{
    SP_Log(L"Başlıyor");
    return TRUE;
}

}   // namespace

SP_MOD_DEFINE(g_modOrnek) =
{
    /* id             */ SP_MOD_ID,
    /* name           */ L"Örnek mod",
    /* basedOn        */ nullptr,
    /* originalAuthor */ nullptr,
    /* targets        */ SP_TARGET_EXPLORER,
    /* minOsBuild     */ 0,
    /* Init           */ Init,
    /* AfterInit      */ nullptr,
    /* SettingsChanged*/ nullptr,
    /* BeforeUninit   */ nullptr,
    /* Uninit         */ nullptr,
};
```

`SP_MOD_ID` kayıt defteri anahtarının adı ve günlük etiketidir. ASCII, küçük harf ve kalıcı olmalı: değiştirmek
kullanıcının ayarlarını kaybettirir.

`targets` modun hangi süreçte yükleneceğini söyler: `SP_TARGET_EXPLORER` (görev çubuğu, masaüstü, Dosya Gezgini)
veya `SP_TARGET_STARTMENU` (StartMenuExperienceHost.exe, yani Başlat menüsü). İkincisini isteyen bir mod açıkken
kabuktaki motor çekirdek DLL'i Başlat sürecine yükler (`engine/hostinject.c`) ve orada ayrı bir motor çalışır.
Bir özelliğin iki süreçte de işi varsa aynı `SP_MOD_ID` ile iki ayrı `SP_Mod` tanımlanabilir; ikisi aynı ayar
anahtarını ve aynı açma/kapama anahtarını paylaşır (örnek: `taskbar_start_button_position.cpp` ve
`start_menu_on_left.cpp`).

### Ayar penceresinde metin değerleri için hazır liste

`settings.reg` içinde REG_SZ bir değer, serbest metin kutusu (`;w`) yerine hazır seçenekli bir açılır listeyle
sunulabilir:

```
;o <seçenek sayısı> <başlık>
;x <değer>|<etiket>
;x |<etiket>                  boş değer: kayıt defteri değeri silinir (varsayılan)
;x *|<etiket>[|<ipucu>]       "Özel...": kullanıcıdan değer ister; ipucu kutuda gösterilir
"Ad"="varsayılan"
```

Etiket ve ipucu `%R:<id>%` ile yerelleştirilmiş dize içerebilir. Hazır seçeneklerle eşleşmeyen bir değer olduğu
gibi gösterilir ve "Özel..." işaretlenir. (`;s` harfi koşullu bölümler için ayrılmıştır.)

## Dört adım

1. Dosyayı `src/core/mods/<ad>.cpp` olarak yaz.
2. `src/core/mods/mod_table.c` içine `SP_MOD_DECLARE` satırını ve tabloya girişini ekle.
3. `src/core/core.vcxproj` içindeki `Mods` grubuna `ClCompile` satırını ekle.
4. Ayar penceresinde görünmesi için `src/gui/resources/settings.reg`, `strings.h` ve **iki** dil dosyasına
   girişleri ekle.

## Windhawk modunu uyarlamak

Motorun API'si bilinçli olarak Windhawk'ınkine yakın tutulmuştur; karşılıklar:

| Windhawk | ShadePatcher |
|----------|--------------|
| `Wh_ModInit` | `SP_Mod` içindeki `Init` |
| `Wh_ModAfterInit` | `AfterInit` |
| `Wh_ModSettingsChanged` | `SettingsChanged` |
| `Wh_ModUninit` | `BeforeUninit` + `Uninit` |
| `Wh_SetFunctionHook` | `SP_SetFunctionHook` |
| `WindhawkUtils::HookSymbols` | `SP_HookSymbols` |
| `Wh_GetIntSetting` | `SP_GetIntSetting` |
| `Wh_GetStringSetting` | `SP_GetStringSetting` |
| `Wh_Log` | `SP_Log` |
| `SetWindowSubclassFromAnyThread` (masaüstü çift tık gibi paylaşımlı hareketler için) | `SP_SubscribeInput` |
| `SetWindowSubclassFromAnyThread` (başka bir pencere için) | `SP_SetWindowSubclassFromAnyThread` / `SP_RemoveWindowSubclassFromAnyThread` |
| `Taskbar.View.dll` yüklenene kadar bekleyen döngüler | `SP_WaitForModule` |

`SP_WaitForModule(L"Taskbar.View.dll", 60000, callback, ctx)` modül yüklüyse hemen (yardımcı bir iş
parçacığında), değilse yüklendiği anda `callback(HMODULE, ctx)` çağırır. Yüklendiği anda demek, motorun
`LoadLibraryExW` kancası sayesinde **modülü yükleyen iş parçacığında**, yükleme dönmeden önce demek: görev
çubuğu için bu, görev çubuğunun kendi UI iş parçacığıdır. Kancalar bu geri çağırmanın içinde kurulur ve geri
çağırma hemen döner. Orada `Sleep` ile bir pencerenin belirmesini beklemek görev çubuğunu bekleme süresi
boyunca dondurur (bir dakika süren böyle bir bekleme "explorer yeniden başladı ama görev çubuğu gelmiyor"
şikayetinin kaynağıydı). Bir pencereyi beklemesi gereken mod `SP_WaitForModuleOnWorker` kullanır: aynı şey,
ama geri çağırma yüklemeden hemen sonra kendi iş parçacığında çalışır ve istediği kadar bekleyebilir. Mod
kaldırılınca bekleyişler motor tarafından iptal edilir. `SP_SetWindowSubclassFromAnyThread` `SetWindowSubclass`'ı
pencerenin kendi iş parçacığına taşır; comctl32 başka iş parçacığından gelen çağrıyı sessizce reddettiği için
motor iş parçacığından doğrudan `SetWindowSubclass` çağırmak çalışmaz.

Kodu kopyalamıyoruz. Özgün modun ne yaptığı okunur, aynı davranış bu API'ye karşı yeniden yazılır, `basedOn` ve
`originalAuthor` alanlarına özgün mod ve yazarı yazılır. Bu alanlar README'de ve ayar penceresinde görünür.

## Kanca kurmak

Birden çok kanca tek bir işlemde olmalı: ya hepsi uygulanır ya hiçbiri.

```cpp
if (!SP_HookBegin())
{
    return FALSE;
}
if (!SP_SetExportHook(L"kernelbase.dll", "RegGetValueW", MyHook, &g_original))
{
    SP_HookAbort();
    return FALSE;
}
return SP_HookCommit();
```

Tek bir kanca için `SP_SetFunctionHookNow` yeterlidir, kendi işlemini açar.

Mod kapatıldığında kancaları motor söker; modun yapması gereken bir şey yoktur.

## Belgelenmemiş fonksiyonlar

```cpp
static const wchar_t* const kNames[] = {
    L"public: void __cdecl winrt::Taskbar::implementation::Something::Method(void)",
};

SP_SymbolHook hooks[1] = {};
hooks[0].symbols = kNames;
hooks[0].symbolCount = ARRAYSIZE(kNames);
hooks[0].pOriginal = (void**)&g_original;
hooks[0].hookFunction = (void*)MyHook;
hooks[0].optional = FALSE;

if (!SP_HookSymbols(GetModuleHandleW(L"Taskbar.View.dll"), hooks, ARRAYSIZE(hooks)))
{
    return FALSE;
}
```

Birden çok ad verilebilir: Windows yapılar arasında sembol adlarını değiştirir, ilk eşleşen kazanır.
`optional = TRUE` olan bir sembol bulunamazsa `*pOriginal` `nullptr` kalır ve mod yine de yüklenir.

`hookFunction` `nullptr` ise yalnızca adres çözülür, hiçbir şey yamalanmaz. Çağırmak istediğin ama araya girmek
istemediğin bir fonksiyon için budur.

## Paylaşımlı hareketler

Masaüstünü veya görev çubuğunu **asla doğrudan subclass etme**. Abone ol:

```cpp
BOOL OnDoubleClick(const SP_InputEvent* ev, void* context)
{
    // TRUE döndürmek hareketi tüketir; arkadaki modlar görmez.
    return TRUE;
}

SP_SubscribeInput(SP_SURFACE_DESKTOP, SP_GESTURE_DOUBLE_CLICK, OnDoubleClick, nullptr);
```

Aynı hareketi isteyen iki mod, kullanıcının `InputPriority` ayarına göre sıralanır. Gerekçesi ve ayrıntısı
[engine.md](engine.md) içindedir.

## Ayarlar

```cpp
int mode = SP_GetIntSetting(L"Mode", 0);

wchar_t wszText[256];
SP_GetStringSetting(L"Text", wszText, ARRAYSIZE(wszText), L"varsayılan");
```

Okunan değer `HKCU\Software\ShadePatcher\Mods\<SP_MOD_ID>` altındadır. Değer yoksa varsayılan döner, yani temiz
bir kurulumda hiçbir şey yazmadan çalışır.

Ayar değişince motor `SettingsChanged` çağırır. Orada yeniden oku ve uygula; kabuğu yeniden başlatma.

## İş parçacığı

`Init`, `AfterInit`, `SettingsChanged`, `BeforeUninit`, `Uninit` motor iş parçacığında ve tek seferde tek mod
olacak şekilde çalışır. Kanca gövdesi ise kancalanan fonksiyonu çağıran iş parçacığında çalışır. İkisi arasında
paylaşılan durum için `std::atomic` kullan:

```cpp
std::atomic<int> g_mode{ 0 };                       // SettingsChanged yazar
g_mode.load(std::memory_order_relaxed);             // kanca okur
```

## Günlük

```cpp
SP_Log(L"Bir şey oldu: %d", value);
SP_LogDebug(L"Ayrıntı");
SP_LogError(L"Bu beklenmiyordu");
```

Günlük varsayılan olarak kapalıdır. Açmak için `HKCU\Software\ShadePatcher` altında `Logging` değerini 1 (bilgi)
veya 2 (ayrıntı) yap; `LogToFile` 1 ise `%LOCALAPPDATA%\ShadePatcher\logs` altına da yazılır. Kapalıyken
argümanlar hiç hesaplanmaz.

## Sınamak

Kabuğa kurmadan önce motoru doğrula:

```
build\bin\Release\sp_selftest.exe
```
