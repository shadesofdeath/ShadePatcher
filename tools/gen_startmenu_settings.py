# Generates src/core/startmenu/settings_table.inc: the Settings pages the search knows, with Turkish and English
# names and keywords. Non-ASCII characters are written as \x escapes (the sources are compiled in the system code
# page), so edit this generator rather than the .inc.
out = r'C:\Users\shades\Desktop\ShadePatcher\src\core\startmenu\settings_table.inc'

rows = [
    # uri, en name, tr name, keywords (both languages, space separated)
    ('ms-settings:display', 'Display', 'Ekran', 'ekran çözünürlük parlaklık ölçek monitör hdr display resolution brightness scale monitor'),
    ('ms-settings:nightlight', 'Night light', 'Gece ışığı', 'gece ışığı mavi ışık night light blue'),
    ('ms-settings:sound', 'Sound', 'Ses', 'ses hoparlör mikrofon kulaklık sound volume speaker microphone headphones'),
    ('ms-settings:notifications', 'Notifications', 'Bildirimler', 'bildirim notifications'),
    ('ms-settings:quiethours', 'Do not disturb', 'Rahatsız etmeyin', 'rahatsız etme odak sessiz focus do not disturb quiet'),
    ('ms-settings:powersleep', 'Power & battery', 'Güç ve pil', 'güç pil uyku enerji ekran kapanma power battery sleep energy'),
    ('ms-settings:storagesense', 'Storage', 'Depolama', 'depolama disk alan temizle storage disk space cleanup'),
    ('ms-settings:multitasking', 'Multitasking', 'Çoklu görev', 'çoklu görev ekran yapıştır pencere multitasking snap windows'),
    ('ms-settings:clipboard', 'Clipboard', 'Pano', 'pano kopyala geçmiş clipboard history'),
    ('ms-settings:about', 'About', 'Hakkında', 'hakkında sistem bilgisi bilgisayar adı sürüm about system info pc name version'),
    ('ms-settings:bluetooth', 'Bluetooth & devices', 'Bluetooth ve cihazlar', 'bluetooth cihaz eşle devices pair'),
    ('ms-settings:printers', 'Printers & scanners', 'Yazıcılar ve tarayıcılar', 'yazıcı tarayıcı printer scanner'),
    ('ms-settings:mousetouchpad', 'Mouse', 'Fare', 'fare imleç tekerlek mouse cursor wheel'),
    ('ms-settings:devices-touchpad', 'Touchpad', 'Dokunmatik yüzey', 'dokunmatik yüzey touchpad'),
    ('ms-settings:network-status', 'Network & internet', 'Ağ ve internet', 'ağ internet bağlantı network connection ethernet'),
    ('ms-settings:network-wifi', 'Wi-Fi', 'Wi-Fi', 'wifi wi-fi kablosuz wireless'),
    ('ms-settings:network-vpn', 'VPN', 'VPN', 'vpn'),
    ('ms-settings:network-proxy', 'Proxy', 'Ara sunucu', 'proxy ara sunucu'),
    ('ms-settings:personalization-background', 'Background', 'Arka plan', 'arka plan duvar kağıdı resim wallpaper background'),
    ('ms-settings:colors', 'Colours', 'Renkler', 'renk koyu mod açık mod vurgu saydamlık colors dark mode light mode accent transparency'),
    ('ms-settings:themes', 'Themes', 'Temalar', 'tema masaüstü simgeleri themes desktop icons'),
    ('ms-settings:lockscreen', 'Lock screen', 'Kilit ekranı', 'kilit ekranı lock screen'),
    ('ms-settings:taskbar', 'Taskbar', 'Görev çubuğu', 'görev çubuğu taskbar'),
    ('ms-settings:personalization-start', 'Start', 'Başlat', 'başlat menü start menu'),
    ('ms-settings:fonts', 'Fonts', 'Yazı tipleri', 'yazı tipi font'),
    ('ms-settings:appsfeatures', 'Installed apps', 'Yüklü uygulamalar', 'uygulama kaldır yüklü program apps uninstall installed programs'),
    ('ms-settings:defaultapps', 'Default apps', 'Varsayılan uygulamalar', 'varsayılan uygulama tarayıcı default apps browser'),
    ('ms-settings:startupapps', 'Startup apps', 'Başlangıç uygulamaları', 'başlangıç açılış startup'),
    ('ms-settings:yourinfo', 'Your info', 'Bilgileriniz', 'hesap bilgi resim account info picture'),
    ('ms-settings:signinoptions', 'Sign-in options', 'Oturum açma seçenekleri', 'oturum açma parola pin hello yüz parmak izi sign-in password face fingerprint'),
    ('ms-settings:dateandtime', 'Date & time', 'Tarih ve saat', 'tarih saat saat dilimi date time zone clock'),
    ('ms-settings:regionlanguage', 'Language & region', 'Dil ve bölge', 'dil bölge klavye biçim language region keyboard format'),
    ('ms-settings:typing', 'Typing', 'Yazma', 'yazma otomatik düzeltme dokunmatik klavye typing autocorrect touch keyboard'),
    ('ms-settings:gaming-gamebar', 'Game Bar', 'Oyun Çubuğu', 'oyun kayıt game bar recording'),
    ('ms-settings:gaming-gamemode', 'Game Mode', 'Oyun Modu', 'oyun modu game mode'),
    ('ms-settings:easeofaccess', 'Accessibility', 'Erişilebilirlik', 'erişilebilirlik yazı boyutu büyüteç anlatıcı accessibility text size magnifier narrator'),
    ('ms-settings:privacy', 'Privacy & security', 'Gizlilik ve güvenlik', 'gizlilik izin güvenlik konum kamera privacy permissions security location camera'),
    ('ms-settings:windowsdefender', 'Windows Security', 'Windows Güvenliği', 'virüs güvenlik defender antivirus security firewall'),
    ('ms-settings:windowsupdate', 'Windows Update', 'Windows Update', 'güncelleme update'),
    ('ms-settings:recovery', 'Recovery', 'Kurtarma', 'kurtarma sıfırla geri al recovery reset'),
    ('ms-settings:activation', 'Activation', 'Etkinleştirme', 'etkinleştirme lisans ürün anahtarı activation license product key'),
    ('ms-settings:developers', 'For developers', 'Geliştiriciler için', 'geliştirici developer mode'),
    ('ms-settings:optionalfeatures', 'Optional features', 'İsteğe bağlı özellikler', 'isteğe bağlı özellik optional features'),
]

def esc(text):
    out = []
    pending_hex = False
    for ch in text:
        if ord(ch) < 128:
            if pending_hex and ch in '0123456789abcdefABCDEF':
                out.append('" L"')   # end the hex escape before a hex digit
            out.append('\\\\' if ch == '\\' else ('\\"' if ch == '"' else ch))
            pending_hex = False
        else:
            out.append('\\x%04X' % ord(ch))
            pending_hex = True
    return 'L"' + ''.join(out) + '"'

lines = ['// Generated by tools/gen_startmenu_settings.py; see startmenu/search.cpp. Do not edit by hand.']
for uri, en, tr, keys in rows:
    lines.append('{ %s, %s, %s, %s },' % (esc(uri), esc(en), esc(tr), esc(keys)))
open(out, 'w', encoding='ascii', newline='\n').write('\n'.join(lines) + '\n')
print(len(rows), 'rows')
