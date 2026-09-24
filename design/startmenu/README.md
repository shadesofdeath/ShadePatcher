# Handoff: Başlat Menüsü (Win32 · Direct2D · DirectComposition · DirectWrite)

## 1. Genel bakış
Windows için sade, açık temalı ve tek sütunlu bir Başlat menüsü. İçinde şunlar var: arama, 15 sabitlenmiş uygulama (5×3 ızgara), "Tüm uygulamalar" (A–Z liste), son dosyalar şeridi, kullanıcı ve güç menüsü. Menü klavyeyle baştan sona kullanılabiliyor.

## 2. Tasarım dosyaları hakkında
`Start Menu.dc.html` bir **HTML tasarım referansıdır**, üretim kodu değildir. Görünümü, ölçüleri ve davranışı birebir gösterir. Hedef, bu tasarımı **native C++ ile yeniden kurmaktır**: Win32 pencere, Direct2D çizim, DirectComposition görsel ağacı ve animasyon, DirectWrite metin. Dosyayı bir tarayıcıda açıp (support.js aynı klasörde olmalı) davranışı canlı test edebilirsin.

Prototipte yalnızca bağlam için bulunan öğeler şunlardır: masaüstü arka planı, alttaki hap şeklindeki görev çubuğu, "… açılıyor…" bildirimi ve harfli renkli ikonlar. Bunlar ürünün parçası değildir; karşılıkları §9'da.

## 3. Doğruluk
**Yüksek doğruluk (hi-fi).** Renkler, ölçüler, tipografi ve animasyon süreleri kesindir. Tüm ölçüler **DIP** cinsindendir (96 DPI'da 1 px). Fiziksel piksele dönüştürmek için `px = dip × dpi / 96` kullanılır.

---

## 4. Mimari

### 4.1 Pencere
```cpp
DWORD ex = WS_EX_NOREDIRECTIONBITMAP   // DComp ile piksel başına alfa
         | WS_EX_TOOLWINDOW            // Alt+Tab / görev çubuğunda görünmez
         | WS_EX_TOPMOST;
DWORD st = WS_POPUP;
```
- DPI farkındalığı: `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`. `WM_DPICHANGED` gelince tüm yüzeyler yeniden oluşturulur.
- Pencere boyutu, gölgeye yer bırakmak için **panel + gölge payı** olmalıdır: sol 80, sağ 80, üst 50, alt 110 DIP (§6.1'deki gölgeye göre hesaplandı).
- Konum: panelin alt kenarı çalışma alanının altına 12 DIP uzaklıkta durur (`MONITORINFO.rcWork`). Yatayda iki seçenek var: `Orta` modunda panel ortalanır, `Sol` modunda soldan 12 DIP içeride durur.
- Panel yüksekliği `min(600, rcWork.height − 24)` DIP, genişliği 580 DIP (sabit).
- Tıklamalar yalnızca panelin (ve açıksa güç menüsünün) içinde işlenmeli. Gölge alanında `WM_NCHITTEST` → `HTTRANSPARENT` döndür, ya da gölge payında tıklamayı "dışarı tıklama" say ve menüyü kapat.

### 4.2 Cihaz zinciri
```
D3D11CreateDevice(BGRA_SUPPORT) → IDXGIDevice
  → D2D1CreateDevice → ID2D1Device → ID2D1DeviceContext
  → DCompositionCreateDevice3(dxgi) → IDCompositionDesktopDevice
  → CreateTargetForHwnd(hwnd, topmost=TRUE)
```
Cihaz kaybedilirse (`DXGI_ERROR_DEVICE_REMOVED`, `D2DERR_RECREATE_TARGET`) tüm zincir yeniden kurulur.

### 4.3 Görsel ağaç (DirectComposition)
```
Root
└─ Container            ← açılış animasyonu burada (translate + scale + opacity)
   ├─ ShadowVisual      ← panel gölgesi, statik bitmap (boyut/DPI değişince yeniden)
   ├─ PanelVisual       ← RectangleClip, köşe yarıçapı 16
   │  ├─ BackgroundSurf ← zemin, kenar, footer zemini, ayraçlar
   │  ├─ ScrollHost     ← içerik alanına kırpılmış (clip)
   │  │  ├─ Highlight   ← tek seçim/hover kutusu (offset + opacity animasyonu)
   │  │  └─ ContentVSurf← IDCompositionVirtualSurface (ızgara/liste), kaydırma = offsetY
   │  ├─ ChromeSurf     ← arama kutusu, başlık satırı, son dosyalar, footer içerik
   │  └─ CaretVisual    ← 1.5×18 DIP, #1E9E78, GetCaretBlinkTime() ile yanıp söner
   └─ PowerFlyout       ← kendi gölgesiyle, açılınca görünür
```
**Neden böyle:** Seçim ve hover vurgusu tek bir visual'dır, bu yüzden hover için içeriği yeniden çizmeye gerek kalmaz; yalnızca highlight'ın offset'i değişir. Kaydırma da içeriği yeniden çizmez; yalnızca ContentVSurf'ün `SetOffsetY` değeri güncellenir.

### 4.4 Arka plan (acrylic / buzlu cam) ve köşeler
Prototipteki panel zemini: `rgba(250,250,248,0.88)` + `blur(40px) saturate(160%)`.
- **Önerilen (DirectComposition'ın sağlayabildiği):** Opak zemin **#F7F8F6** kullan. Bu, prototipteki yarı saydam zeminin pastel duvar kâğıdı üzerindeki görünümüne en yakın opak renk. DComp'un tek başına bir backdrop-blur özelliği yok.
- **İsteğe bağlı gerçek bulanıklık:** Windows.UI.Composition interop kullanılabilir (`ICompositorDesktopInterop::CreateDesktopWindowTarget`, `Compositor.CreateHostBackdropBrush()`, üstüne %88 `#FAFAF8` renk katmanı). D2D çizimi `ICompositorInterop::CreateGraphicsDevice` ile aynı ağaca bağlanır. Mimari aynı kalır; yalnızca "Background" katmanı değişir.
- DWM'in yuvarlak köşe ayarını (`DWMWA_WINDOW_CORNER_PREFERENCE`) kullanma; yarıçapı 8'de sabittir. Köşeyi `IDCompositionRectangleClip` ile ver: dört köşede `SetTopLeftRadiusX/Y` … = 16.

### 4.5 Metin (DirectWrite)
- Font: **Figtree** (400/500/600/700, SIL OFL). TTF dosyaları uygulamayla birlikte dağıtılır:
  `IDWriteFactory5::CreateFontSetBuilder` → `AddFontFile` ×4 → `CreateFontSet` → `CreateFontCollectionFromFontSet`.
- Yedek font: `Segoe UI Variable Text`, o da yoksa `Segoe UI`.
- Antialias: `D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE`, çünkü DComp yüzeyi alfalı olduğundan ClearType çalışmaz.
- Rakamlar (saat): `DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES`.
- Taşan metin: `IDWriteInlineObject` ellipsis (`CreateEllipsisTrimmingSign`) ve `DWRITE_TRIMMING_GRANULARITY_CHARACTER`, `DWRITE_WORD_WRAPPING_NO_WRAP`.
- Satır yüksekliği: tek satırlık etiketlerde `DWRITE_LINE_SPACING_METHOD_UNIFORM` ve font boyutu × 1.2. Dikey ortalama, `DWRITE_PARAGRAPH_ALIGNMENT_CENTER` ile kutuya göre yapılır.

---

## 5. Ekran / yerleşim (panel koordinatları, DIP)
Panel 580 × 600. 1 DIP'lik kenar çizgisi içe doğru çizilir, yani iç genişlik 578.

| Bölge | y | Yükseklik | Not |
|---|---|---|---|
| Arama bölgesi | 0 | 76 | padding 24/24/8/24 |
| Başlık satırı | 76 | 48 | padding 16 üst, 4 alt, 28 yatay |
| İçerik (kaydırılır) | 124 | kalan alan (600'de 368) | padding 4 üst, 16 yatay, 12 alt |
| Son dosyalar | alt − 60 − 46 | 46 | padding 0/24/16 |
| Footer | alt − 60 | 60 | |

Son dosyalar şeridi arama yaparken ve "Tüm uygulamalar" açıkken gizlenir; o durumda içerik alanı 46 DIP uzar.

### 5.1 Arama kutusu
- Kutu: x 24, y 24, 530 × 44, yarıçap 22. Zemin `#FFFFFF`, kenar 1 DIP `rgba(29,35,32,0.08)`. Kutuda yazı varken kenar `#1E9E78` olur ve renk 120 ms'de geçer.
- Büyüteç: 16×16, kutunun solundan 16 DIP içeride, dikeyde ortada. Çizgi `#6E7571`, kalınlık 1.6, uçlar yuvarlak. Merkezi (7,7) ve yarıçapı 4.6 olan bir daire ile (10.5,10.5) → (14,14) bir çizgiden oluşur.
- Metin: x = 16 + 16 + 10 = 42 (kutuya göre). Figtree 400, 15 DIP, `#1D2320`. Placeholder "Ara", `#8B918E`.
- İmleç (caret): `#1E9E78`.
- Temizle düğmesi (yalnızca yazı varken): 22×22 daire, sağdan 16 DIP içeride. Zemin `#EEF0EE`, içinde "×" (13 DIP, `#6E7571`).

### 5.2 Başlık satırı
- Başlık (solda, x 28): Figtree 700, 15 DIP, `#1D2320`. Duruma göre metin:
  - Varsayılan: **"Sabitlenmiş"**
  - Tüm uygulamalar açıkken: **"Tüm uygulamalar"**
  - Arama yaparken: **"En iyi eşleşmeler"**
- Geçiş düğmesi (sağda, sağdan 28 DIP içeride; arama yaparken gizlenir): 28 DIP yükseklik, yatay padding 12, yarıçap 14. Zemin `rgba(29,35,32,0.05)`, hover'da `0.09`. Metin Figtree 500, 13 DIP, `#4A514D`: **"Tümü ›"** veya **"‹ Geri"**.

### 5.3 Sabitlenmiş ızgara (varsayılan görünüm ve arama sonuçları)
- 5 sütun, aralık 2. Hücre genişliği (546 − 8) / 5 = **107.6**, yüksekliği **96**, köşe yarıçapı 12.
- Hücrenin içeriği dikeyde ortalanır: ikon (44) + boşluk (9) + etiket (yaklaşık 15) = 68. Bu yüzden ikonun üstü hücrenin 14 DIP altından başlar.
- İkon: 44×44, yatayda ortada (§9 gerçek ikonlar).
- Etiket: Figtree 500, 12.5 DIP, `#2B312E`, en fazla 96 DIP genişlik, taşarsa "…". Ortalanır.
- Seçili ya da hover durumunda hücre zemini `rgba(29,35,32,0.06)` olur.
- Basılıyken hücre ölçeği 0.96 olur (merkezden, 120 ms, ease). Bunun için hücre başına bir visual gerekmez; basıldığı anda o hücreyi geçici bir visual'a taşımak yeterli.

### 5.4 Tüm uygulamalar (A–Z liste)
- Uygulamalar Türkçe sıralamaya göre dizilir: `CompareStringEx(L"tr-TR", LINGUISTIC_IGNORECASE, …)`.
- Harf başlığı: 30 DIP yükseklik. Harf alta hizalıdır (alt padding 4), soldan 12 DIP içeride. Figtree 700, 12 DIP, `#1E9E78`.
- Satır: 44 DIP yükseklik, tam genişlik, yarıçap 10, soldan 12 DIP içeride. İkon 28×28 (yarıçap 8), ikonla metin arası 12. Ad Figtree 500, 14 DIP, `#1D2320`.
- Seçili ya da hover durumunda satır zemini `rgba(29,35,32,0.06)` olur.

### 5.5 Son dosyalar şeridi
- Çipler yan yana, aralarında 8 DIP boşluk; soldan 24 DIP içeride başlar. Sığmayan çipler gösterilmez (kaydırma yok).
- Çip: 30 DIP yükseklik, yarıçap 15, yatay padding 12. Zemin `#FFFFFF`, kenar 1 DIP `rgba(29,35,32,0.07)`, hover'da kenar `0.18`.
- Çipin içi: dosya türü rengindeki 6 DIP'lik nokta, 7 DIP boşluk, dosya adı (Figtree 400, 12 DIP, `#3A413D`).
- Prototipteki nokta renkleri: belge `#3A86E0`, görsel `#E0624A`, kod `#1E9E78`.

### 5.6 Footer
- Alan: 60 DIP yükseklik. Zemin `rgba(236,239,236,0.6)`, opak karşılığı **#EFF1EF**. Üstünde 1 DIP ayraç `rgba(29,35,32,0.06)`. Padding sol 20, sağ 16.
- Hesap düğmesi (solda): 40 DIP yükseklik, yarıçap 20, padding 0/10/0/6, hover'da zemin `rgba(29,35,32,0.05)`. İçinde:
  - Avatar: 28 DIP daire, 135° gradyan `#F2B38B` → `#E27D6A`. Ortasında adın baş harfi (Figtree 700, 13 DIP, beyaz). Gerçek bir hesap fotoğrafı varsa aynı daireye kırpılarak gösterilir.
  - Avatarın 10 DIP sağında ad (Figtree 600, 13 DIP, `#1D2320`).
- Güç düğmesi (sağda): 40 DIP daire, hover'da `rgba(29,35,32,0.06)`, menü açıkken `0.08`.
  - Simgesi 16×16, çizgi `#2B312E`, kalınlık 1.6, uçlar yuvarlak. (8,1.8) → (8,7.4) dikey bir çizgi ile merkezi (8,8.6) ve yarıçapı 5.4 olan, üstü açık bir yay (yaklaşık −48° ile 228° arası).
- Güç menüsü:
  - Kutu: 172 DIP genişlik, sağ kenarı düğmeyle hizalı, alt kenarı düğmenin üstünden 8 DIP yukarıda. Padding 6, yarıçap 12. Zemin `#FFFFFF`, kenar 1 DIP `rgba(29,35,32,0.06)`, gölge `0 12 32 rgba(30,45,40,0.16)`.
  - Öğeler: 36 DIP yükseklik, yarıçap 8, yatay padding 12. Figtree 500, 13 DIP. Hover'da `rgba(29,35,32,0.05)`.
  - Sırasıyla: **Kilitle · Uyku · Yeniden başlat · Kapat**.

---

## 6. Görsel ayrıntılar

### 6.1 Panel gölgesi (iki katman)
| Katman | Offset | Blur | Renk |
|---|---|---|---|
| 1 | 0, 30 | 80 | `rgba(30,45,40,0.20)` |
| 2 | 0, 4 | 14 | `rgba(30,45,40,0.08)` |

Uygulama: `CLSID_D2D1Shadow` efektiyle, girdi olarak 580×H boyutunda ve 16 yarıçaplı bir yuvarlak dikdörtgen maskesi kullanılır.
- **Önemli:** Kullanılacak değer **StandardDeviation = CSS blur / 2**. Yani katman 1 için 40, katman 2 için 7.
- İki katman tek bir bitmap'e birleştirilip ShadowVisual'a konur. Sonuç, yalnızca panel boyutu ya da DPI değişince yeniden hesaplanır.

### 6.2 Panel kenarı
1 DIP, `rgba(255,255,255,0.9)`. Kenar içe çizilir: 0.5 DIP içeri kaydırılmış yuvarlak dikdörtgen, yarıçap 15.5.

### 6.3 İkon karoları (prototipteki yer tutucular, bilgi için)
- 145° doğrusal gradyan, iki renk arasında (§7 ikon renkleri).
- Üst kenarda 1 DIP, beyaz %35 bir parlama çizgisi.
- Gölge `0 3 8`, ikinci rengin %25 opaklığı.

Gerçek uygulamada bunların yerine sistem ikonları gelir (§9).

---

## 7. Tasarım tokenları
Tokenların C++ karşılıkları `StartMenuTokens.h` dosyasında `constexpr` olarak hazır.

**Renkler**
| Token | Değer | Kullanım |
|---|---|---|
| text.primary | `#1D2320` | başlık, ad, arama metni |
| text.label | `#2B312E` | ızgara etiketi, güç simgesi |
| text.secondary | `#4A514D` | geçiş düğmesi |
| text.chip | `#3A413D` | son dosyalar |
| text.muted | `#6E7571` | büyüteç, × |
| text.placeholder | `#8B918E` | placeholder, boş durum |
| accent | `#1E9E78` | imleç, odak kenarı, harf başlıkları, başlat simgesi |
| surface.panel | `#FAFAF8` @0.88 (opak: `#F7F8F6`) | panel zemini |
| surface.footer | `#ECEFEC` @0.6 (opak: `#EFF1EF`) | footer |
| surface.field | `#FFFFFF` | arama, çip, flyout |
| surface.clear | `#EEF0EE` | × düğmesi |
| ink.a05 / a06 / a08 / a09 | `#1D2320` @0.05/0.06/0.08/0.09 | hover, seçim, kenar |
| line.hair | `#1D2320` @0.06 | ayraçlar |
| line.panel | `#FFFFFF` @0.9 | panel kenarı |
| shadow | `#1E2D28` @0.20 / 0.08 / 0.16 | gölgeler |

**İkon gradyanları (yer tutucu)**
| Ad | Başlangıç | Bitiş |
|---|---|---|
| mint | `#3CC79B` | `#1E9E78` |
| sky | `#6BB8F5` | `#3A86E0` |
| coral | `#F59A7B` | `#E0624A` |
| sun | `#F7CB5E` | `#EBA02C` |
| plum | `#B69AF2` | `#8465D8` |
| ink | `#4B5552` | `#2A302D` |
| rose | `#F29BB8` | `#D8638B` |
| teal | `#5CCBD0` | `#2C9EA8` |

**Tipografi (Figtree)**
| Boyut | Ağırlık | Kullanım |
|---|---|---|
| 15 | 700 | başlık |
| 15 | 400 | arama |
| 14 | 500 | liste satırı |
| 13 | 600 | kullanıcı adı |
| 13 | 500 | düğme, flyout |
| 12.5 | 500 | ızgara etiketi |
| 12 | 700 | harf başlığı |
| 12 | 400 | çip |

**Yarıçaplar:** panel 16 · ikon 13 · hücre 12 · flyout 12 · liste satırı 10 · liste ikonu 8 · flyout öğesi 8 · hap/daire = yükseklik / 2

**Boşluklar:** 2 · 4 · 6 · 7 · 8 · 9 · 10 · 12 · 16 · 20 · 24 · 28

---

## 8. Etkileşim ve animasyon

### 8.1 Açılış / kapanış (birebir)
Açılışta panel aşağıdan yukarı kayarken hafifçe büyür ve belirir. Kapanışta aynı adımlar tersine çalışır.

| Özellik | Kapalı | Açık | Süre | Eğri |
|---|---|---|---|---|
| Opacity | 0 | 1 | **160 ms** | `cubic-bezier(0, 0, 0.58, 1)` (ease-out) |
| TranslateY | +16 DIP | 0 | **220 ms** | `cubic-bezier(0.2, 0.9, 0.3, 1.1)`, hafif taşma (overshoot) |
| Scale (X ve Y) | 0.98 | 1.00 | **220 ms** | aynı eğri |

- Ölçeklemenin merkezi panelin merkezidir. Gölge dahil değil: pencere koordinatında `(80 + 290, 50 + H/2)`.
- Animasyon `Container` visual'ına uygulanır, yani gölge ve panel birlikte hareket eder. Kullanılacak transformlar:
  - `IDCompositionTranslateTransform::SetOffsetY(anim)`
  - `IDCompositionScaleTransform::SetScaleX/Y(anim)` + `SetCenterX/Y`
  - bu ikisi bir `CreateTransformGroup` içinde (önce ölçek, sonra öteleme)
  - `IDCompositionEffectGroup::SetOpacity(anim)`
- **Açılış sırası:**
  1. Değerleri kapalı duruma getir ve `Commit`.
  2. `ShowWindow(SW_SHOWNOACTIVATE)` + `SetForegroundWindow`.
  3. Animasyonları bağla ve `Commit`.
- **Kapanış sırası:** Aynı animasyonları ters yönde bağla ve `Commit`. 220 ms sonra (`SetTimer` ya da `DCompositionWaitForCompositorClock`) `ShowWindow(SW_HIDE)`.
- Kapanma animasyonu sürerken menü yeniden açılırsa, animasyon anlık değerden devam etmelidir. Bunun için değerleri kendi saatinle hesaplayıp yeni animasyonu o değerden başlat.
- Açılışta arama kutusu anında odak alır ve imleç görünür.

**Bezier → DComp:** `IDCompositionAnimation` yalnızca kübik polinom (`AddCubic`) segmentleri kabul eder. Bezier eğrisini 8 segmente bölüp her birini Hermite yaklaşımıyla ekle:
```cpp
// y(t) = bezierY(solveX(t)); her segment için a + b·dt + c·dt² + d·dt³
void AddBezier(IDCompositionAnimation* a, float from, float to, double durS,
               float x1, float y1, float x2, float y2, int N = 8) {
  auto B = [](double p1, double p2, double u) {        // 1D kübik bezier, uç noktalar 0 ve 1
    double v = 1 - u; return 3*v*v*u*p1 + 3*v*u*u*p2 + u*u*u; };
  auto dB = [](double p1, double p2, double u) {
    double v = 1 - u; return 3*v*v*p1 + 6*v*u*(p2 - p1) + 3*u*u*(1 - p2); };
  auto solve = [&](double x) { double u = x;            // Newton ile t'den u'yu bul
    for (int i = 0; i < 8; ++i) { double d = dB(x1, x2, u); if (fabs(d) < 1e-6) break;
      u -= (B(x1, x2, u) - x) / d; } return std::clamp(u, 0.0, 1.0); };
  auto Y = [&](double t) { return from + (to - from) * B(y1, y2, solve(t)); };
  auto dY = [&](double t) { double u = solve(t);         // dy/dt = (dy/du) / (dx/du)
    double dx = dB(x1, x2, u); return (to - from) * (dx > 1e-6 ? dB(y1, y2, u) / dx : 0) / durS; };
  double h = durS / N;
  for (int i = 0; i < N; ++i) {
    double t0 = double(i) / N, t1 = double(i + 1) / N;
    double p0 = Y(t0), p1 = Y(t1), m0 = dY(t0) * h, m1 = dY(t1) * h;
    double c = 3*(p1 - p0) - 2*m0 - m1, d = 2*(p0 - p1) + m0 + m1;
    a->AddCubic(i * h, (float)p0, (float)(m0 / h), (float)(c / (h*h)), (float)(d / (h*h*h)));
  }
  a->End(durS, to);
}
// Açılış:
AddBezier(opacity, 0, 1,     0.160, 0.0f, 0.0f, 0.58f, 1.0f);
AddBezier(transY,  16*s, 0,  0.220, 0.2f, 0.9f, 0.3f,  1.1f);   // s = dpi/96
AddBezier(scale,   0.98f, 1, 0.220, 0.2f, 0.9f, 0.3f,  1.1f);
```

### 8.2 Mikro animasyonlar
| Olay | Değer |
|---|---|
| Hover / seçim vurgusu | Highlight visual anında yeni hücreye taşınır; opacity 0→1, **80 ms**, lineer |
| Arama kenarı rengi | `ink.a08` → `accent`, **120 ms**. Chrome yüzeyi süre boyunca 60 Hz'de yeniden çizilir |
| Hücreye basma | scale 1 → 0.96, **120 ms**, ease `cubic-bezier(.25,.1,.25,1)`; bırakınca geri |
| Bildirim (prototip) | Üründe yok |
| Sistem animasyonları kapalıysa | `SPI_GETCLIENTAREAANIMATION` FALSE ise tüm süreler 0 olur |

### 8.3 Klavye
| Tuş | Davranış |
|---|---|
| **Win** | Menüyü aç/kapat. `WH_KEYBOARD_LL` hook ile yakalanır. LWin/RWin tek başına basılıp bırakılırsa sistem Başlat'ı bastırılır; Win+X gibi kombinasyonlar sisteme geçer |
| Yazmak | Arama kutusuna gider (odak her zaman aramadadır). `WM_CHAR` ile alınır, IME için `ImmSetCompositionWindow` |
| ← → | Izgarada ±1 (yalnızca ızgara modunda) |
| ↑ ↓ | Izgarada ±5, listede ±1. Sınırlarda durur, başa/sona sarmaz |
| Enter | Seçili öğeyi başlat |
| Esc | Sırasıyla ilk geçerli olanı yapar: güç menüsünü kapat → aramayı temizle → "Tüm uygulamalar"dan geri dön → menüyü kapat |
| Backspace / Ctrl+A / Ctrl+V | Standart düzenleme |

- Seçim her yeni aramada ya da görünüm değişiminde 0'a (ilk öğe) sıfırlanır.
- Klavyeyle seçilen öğe görünür alanın dışındaysa, kenardan 6 DIP pay bırakılarak kaydırılır.

### 8.4 Fare
- Hover seçimi değiştirir; klavye ve fare aynı `sel` değerini kullanır. Hover için `TrackMouseEvent` ve `WM_MOUSELEAVE` kullanılır.
- Tıklama öğeyi başlatır.
- Tekerlek: `WM_MOUSEWHEEL` ile 120 delta = 48 DIP. Kaydırma ScrollHost'ta, kaydırma çubuğu görünmez.
- Panelin dışına tıklamak ya da pencerenin odağı kaybetmesi (`WM_ACTIVATE` + `WA_INACTIVE`) menüyü kapatır.

### 8.5 Arama
- Tüm uygulamalar arasında Türkçe büyük/küçük harf duyarsız "içerir" araması: `FindNLSStringEx(L"tr-TR", FIND_FROMSTART | LINGUISTIC_IGNORECASE, …)`. Böylece "i" ile "İ" doğru eşleşir.
- Sonuçlar ızgara düzeninde gösterilir, başlık "En iyi eşleşmeler" olur.
- Sonuç yoksa içerik alanına 60 DIP üst padding ile ortalanmış **“{sorgu}” için sonuç yok** yazısı çıkar (14 DIP, `#8B918E`).
- Arama sırasında geçiş düğmesi ve son dosyalar gizlenir.

---

## 9. Veri ve sistem entegrasyonu
| Öğe | Kaynak |
|---|---|
| Tüm uygulamalar | `SHGetKnownFolderItem(FOLDERID_AppsFolder)` → `IEnumShellItems`. Win32 ve UWP uygulamalarını birlikte verir |
| İkonlar | `IShellItemImageFactory::GetImage(size, SIIGBF_ICONONLY)` → WIC → `ID2D1Bitmap1`. İstenen boyut: `44 × dpi/96`, en yakın büyük boyuta yuvarlanır (ızgara için 48/64/96). Arka plan karosu yok, ikon doğrudan 44×44 çizilir |
| Sabitlenmişler | `%LOCALAPPDATA%\<Uygulama>\pinned.json` içinde AppUserModelID ya da parsing name listesi. Varsayılan 15 öğe |
| Son dosyalar | `FOLDERID_Recent`, `.lnk` hedefleri, değişiklik tarihine göre en yeni 3 dosya |
| Başlatma | `IShellItem` → `IContextMenu` "open", ya da `ShellExecuteExW(SEE_MASK_INVOKEIDLIST)`. UWP için `IApplicationActivationManager` |
| Kullanıcı | `GetUserNameExW(NameDisplay)`. Fotoğraf varsa `%PUBLIC%\AccountPictures` |
| Kilitle | `LockWorkStation()` |
| Uyku | `SetSuspendState(FALSE, FALSE, FALSE)` |
| Yeniden başlat / Kapat | `SE_SHUTDOWN_NAME` yetkisi al, sonra `InitiateShutdownW(…, SHUTDOWN_RESTART / SHUTDOWN_POWEROFF, …)` |
| Hesap düğmesi | `ms-settings:yourinfo` |

**Performans:** Uygulama listesi ve ikonlar arka planda önceden yüklenir, menü açılırken hiçbir G/Ç yapılmaz. Pencere ve yüzeyler kalıcıdır; menü kapanınca yalnızca gizlenir. Böylece açılışta ilk kare 16 ms'nin altında çizilir.

---

## 10. Durum (state)
```cpp
struct MenuState {
  bool open;            // görünür mü
  bool showAll;         // false: Sabitlenmiş ızgara, true: A–Z liste
  std::wstring query;   // boş değilse arama modunda
  int  sel;             // seçili öğe indeksi (hover + klavye)
  bool powerOpen;       // güç menüsü açık mı
  float scrollY;        // içerik kaydırma
};
// Görünüm modu:
//   query boş değilse → Izgara (sonuçlar)
//   değilse showAll true ise → Liste
//   değilse → Izgara (sabitlenmiş)
// Menü kapanınca query="", sel=0, showAll=false, powerOpen=false
```

## 11. Dosyalar
- `Start Menu.dc.html`: etkileşimli tasarım referansı (`support.js` ile birlikte açılır).
- `StartMenuTokens.h`: renk, ölçü, font ve animasyon sabitleri.
- `README.md`: bu belge.
