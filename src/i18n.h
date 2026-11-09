#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <cwctype>

namespace PinTrayLocale {

struct Text {
    const wchar_t* shortcuts;
    const wchar_t* startup;
    const wchar_t* quit;
    const wchar_t* occupied;
    const wchar_t* pin;
    const wchar_t* unpin;
};

// Windows UI language, not keyboard layout or the regional formatting locale.
// The first item in the system MUI list is the user's highest-priority display language.
inline std::wstring PrimaryLanguage() {
    ULONG count = 0;
    ULONG length = 0;
    GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length);
    if (length > 1 && length < 32768) {
        std::vector<wchar_t> languages(length, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages.data(), &length)
            && !languages.empty() && languages[0]) {
            std::wstring name(languages.data());
            for (auto& ch : name) ch = static_cast<wchar_t>(std::towlower(ch));
            return name;
        }
    }
    wchar_t fallback[LOCALE_NAME_MAX_LENGTH]{};
    const LCID lcid = MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT);
    if (LCIDToLocaleName(lcid, fallback, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        std::wstring name(fallback);
        for (auto& ch : name) ch = static_cast<wchar_t>(std::towlower(ch));
        return name;
    }
    return L"en";
}

inline Text ForLanguage(std::wstring_view name) {
    static constexpr Text english = {
        L"Keyboard shortcut", L"Start with Windows", L"Exit",
        L"This keyboard shortcut is already in use.",
        L"Pin window", L"Unpin window"
    };
    static constexpr Text simplified = {
        L"快捷键", L"开机自启", L"退出", L"该快捷键已被其他程序占用。",
        L"置顶窗口", L"取消置顶"
    };
    static constexpr Text traditional = {
        L"快速鍵", L"開機自動啟動", L"結束", L"此快速鍵已被其他程式使用。",
        L"視窗置頂", L"取消置頂"
    };
    if (name.starts_with(L"zh")) {
        if (name.find(L"hant") != name.npos || name.starts_with(L"zh-tw") ||
            name.starts_with(L"zh-hk") || name.starts_with(L"zh-mo"))
            return traditional;
        return simplified;
    }
    struct Entry { std::wstring_view language; Text text; };
    static constexpr Entry translations[] = {
        {L"ja", {L"ショートカット", L"Windows 起動時に実行", L"終了",
                 L"このショートカットは既に使用されています。", L"最前面に固定", L"固定を解除"}},
        {L"ko", {L"단축키", L"Windows 시작 시 실행", L"종료",
                 L"이 단축키는 이미 사용 중입니다.", L"창 항상 위에 고정", L"고정 해제"}},
        {L"fr", {L"Raccourci clavier", L"Démarrer avec Windows", L"Quitter",
                 L"Ce raccourci est déjà utilisé.", L"Épingler la fenêtre", L"Désépingler la fenêtre"}},
        {L"de", {L"Tastenkürzel", L"Mit Windows starten", L"Beenden",
                 L"Dieses Tastenkürzel wird bereits verwendet.", L"Fenster anheften", L"Anheften aufheben"}},
        {L"es", {L"Atajo de teclado", L"Iniciar con Windows", L"Salir",
                 L"Este atajo ya está en uso.", L"Fijar ventana", L"Desfijar ventana"}},
        {L"pt", {L"Atalho de teclado", L"Iniciar com o Windows", L"Sair",
                 L"Este atalho já está em uso.", L"Fixar janela", L"Desafixar janela"}},
        {L"it", {L"Scorciatoia da tastiera", L"Avvia con Windows", L"Esci",
                 L"Questa scorciatoia è già in uso.", L"Fissa finestra", L"Rimuovi fissaggio"}},
        {L"ru", {L"Горячая клавиша", L"Запускать с Windows", L"Выход",
                 L"Это сочетание клавиш уже используется.", L"Закрепить окно", L"Открепить окно"}},
        {L"uk", {L"Комбінація клавіш", L"Запускати з Windows", L"Вийти",
                 L"Це сполучення клавіш уже використовується.", L"Закріпити вікно", L"Відкріпити вікно"}},
        {L"pl", {L"Skrót klawiaturowy", L"Uruchamiaj z Windows", L"Zakończ",
                 L"Ten skrót jest już używany.", L"Przypnij okno", L"Odepnij okno"}},
        {L"nl", {L"Sneltoets", L"Starten met Windows", L"Afsluiten",
                 L"Deze sneltoets is al in gebruik.", L"Venster vastmaken", L"Venster losmaken"}},
        {L"sv", {L"Kortkommando", L"Starta med Windows", L"Avsluta",
                 L"Kortkommandot används redan.", L"Fäst fönstret", L"Lossa fönstret"}},
        {L"fi", {L"Pikanäppäin", L"Käynnistä Windowsin mukana", L"Lopeta",
                 L"Pikanäppäin on jo käytössä.", L"Kiinnitä ikkuna", L"Irrota ikkuna"}},
        {L"da", {L"Genvejstast", L"Start med Windows", L"Afslut",
                 L"Genvejstasten er allerede i brug.", L"Fastgør vindue", L"Frigør vindue"}},
        {L"nb", {L"Hurtigtast", L"Start med Windows", L"Avslutt",
                 L"Hurtigtasten er allerede i bruk.", L"Fest vindu", L"Løsne vindu"}},
        {L"nn", {L"Snarveg", L"Start med Windows", L"Avslutt",
                 L"Snarvegen er allereie i bruk.", L"Fest vindauge", L"Løysne vindauge"}},
        {L"cs", {L"Klávesová zkratka", L"Spouštět s Windows", L"Ukončit",
                 L"Tato zkratka se již používá.", L"Připnout okno", L"Odepnout okno"}},
        {L"sk", {L"Klávesová skratka", L"Spúšťať s Windows", L"Ukončiť",
                 L"Táto skratka sa už používa.", L"Pripnúť okno", L"Odopnúť okno"}},
        {L"hu", {L"Billentyűparancs", L"Indítás a Windowsszal", L"Kilépés",
                 L"Ez a billentyűparancs már foglalt.", L"Ablak rögzítése", L"Rögzítés feloldása"}},
        {L"ro", {L"Comandă rapidă", L"Pornire odată cu Windows", L"Ieșire",
                 L"Această comandă rapidă este deja utilizată.", L"Fixează fereastra", L"Anulează fixarea"}},
        {L"bg", {L"Клавишна комбинация", L"Стартиране с Windows", L"Изход",
                 L"Комбинацията вече се използва.", L"Закачи прозореца", L"Откачи прозореца"}},
        {L"el", {L"Συντόμευση", L"Εκκίνηση με τα Windows", L"Έξοδος",
                 L"Η συντόμευση χρησιμοποιείται ήδη.", L"Καρφίτσωμα παραθύρου", L"Ξεκαρφίτσωμα"}},
        {L"tr", {L"Klavye kısayolu", L"Windows ile başlat", L"Çıkış",
                 L"Bu kısayol zaten kullanılıyor.", L"Pencereyi sabitle", L"Sabitlemeyi kaldır"}},
        {L"ar", {L"اختصار لوحة المفاتيح", L"البدء مع Windows", L"خروج",
                 L"هذا الاختصار مستخدم بالفعل.", L"تثبيت النافذة", L"إلغاء التثبيت"}},
        {L"he", {L"קיצור מקשים", L"הפעלה עם Windows", L"יציאה",
                 L"קיצור המקשים הזה כבר בשימוש.", L"הצמד חלון", L"בטל הצמדה"}},
        {L"fa", {L"میانبر صفحه‌کلید", L"اجرای خودکار با ویندوز", L"خروج",
                 L"این میانبر قبلاً استفاده شده است.", L"سنجاق کردن پنجره", L"برداشتن سنجاق"}},
        {L"hi", {L"कीबोर्ड शॉर्टकट", L"Windows के साथ शुरू करें", L"बाहर निकलें",
                 L"यह शॉर्टकट पहले से उपयोग में है।", L"विंडो पिन करें", L"विंडो अनपिन करें"}},
        {L"bn", {L"কিবোর্ড শর্টকাট", L"Windows চালু হলে শুরু করুন", L"প্রস্থান",
                 L"এই শর্টকাটটি ইতিমধ্যে ব্যবহৃত হচ্ছে।", L"উইন্ডো পিন করুন", L"পিন সরান"}},
        {L"ta", {L"விசைப்பலகை குறுக்குவழி", L"Windows உடன் தொடங்கு", L"வெளியேறு",
                 L"இந்த குறுக்குவழி ஏற்கனவே பயன்பாட்டில் உள்ளது.", L"சாளரத்தைப் பின் செய்", L"பின்னை நீக்கு"}},
        {L"th", {L"แป้นพิมพ์ลัด", L"เริ่มพร้อม Windows", L"ออก",
                 L"แป้นพิมพ์ลัดนี้ถูกใช้งานแล้ว", L"ปักหมุดหน้าต่าง", L"เลิกปักหมุด"}},
        {L"vi", {L"Phím tắt", L"Khởi động cùng Windows", L"Thoát",
                 L"Phím tắt này đã được sử dụng.", L"Ghim cửa sổ", L"Bỏ ghim cửa sổ"}},
        {L"id", {L"Pintasan keyboard", L"Mulai bersama Windows", L"Keluar",
                 L"Pintasan ini sudah digunakan.", L"Sematkan jendela", L"Lepas sematan"}},
        {L"ms", {L"Pintasan papan kekunci", L"Mula dengan Windows", L"Keluar",
                 L"Pintasan ini sudah digunakan.", L"Semat tetingkap", L"Nyahsemat"}},
        {L"fil", {L"Keyboard shortcut", L"Simulan kasama ng Windows", L"Lumabas",
                 L"Ginagamit na ang shortcut na ito.", L"I-pin ang window", L"Alisin ang pin"}},
        {L"hr", {L"Tipkovnički prečac", L"Pokreni s Windowsom", L"Izlaz",
                 L"Ovaj se prečac već koristi.", L"Prikvači prozor", L"Otkvači prozor"}},
        {L"sr", {L"Пречица на тастатури", L"Покрени уз Windows", L"Излаз",
                 L"Ова пречица се већ користи.", L"Закачи прозор", L"Откачи прозор"}},
        {L"sl", {L"Bližnjica na tipkovnici", L"Zaženi z Windows", L"Izhod",
                 L"Ta bližnjica je že v uporabi.", L"Pripni okno", L"Odpni okno"}},
        {L"lt", {L"Spartusis klavišas", L"Paleisti su Windows", L"Išeiti",
                 L"Šis spartusis klavišas jau naudojamas.", L"Prisegti langą", L"Atsegti langą"}},
        {L"lv", {L"Īsinājumtaustiņš", L"Sākt kopā ar Windows", L"Iziet",
                 L"Šis īsinājumtaustiņš jau tiek izmantots.", L"Piespraust logu", L"Atspraust logu"}},
        {L"et", {L"Kiirklahv", L"Käivita koos Windowsiga", L"Välju",
                 L"See kiirklahv on juba kasutusel.", L"Kinnita aken", L"Eemalda kinnitus"}},
        {L"ca", {L"Drecera de teclat", L"Inicia amb Windows", L"Surt",
                 L"Aquesta drecera ja s'utilitza.", L"Fixa la finestra", L"Desfixa la finestra"}},
        {L"eu", {L"Teklatuko lasterbidea", L"Abiarazi Windowsekin", L"Irten",
                 L"Lasterbide hau erabiltzen ari da.", L"Finkatu leihoa", L"Askatu leihoa"}},
        {L"gl", {L"Atallo de teclado", L"Iniciar con Windows", L"Saír",
                 L"Este atallo xa está en uso.", L"Fixar xanela", L"Soltar xanela"}},
        {L"af", {L"Sleutelbordkortpad", L"Begin saam met Windows", L"Verlaat",
                 L"Hierdie kortpad word reeds gebruik.", L"Speld venster vas", L"Maak venster los"}},
        {L"sw", {L"Njia ya mkato", L"Anza na Windows", L"Toka",
                 L"Njia hii ya mkato tayari inatumika.", L"Bandika dirisha", L"Bandua dirisha"}}
    };

    std::wstring_view primary = name.substr(0, name.find(L'-'));
    for (const Entry& candidate : translations) {
        if (candidate.language == primary) return candidate.text;
    }
    return english; // Safe fallback for every other Windows display language.
}

inline Text Detect() { return ForLanguage(PrimaryLanguage()); }

} // namespace PinTrayLocale
