# LiveDelay

Lekki przełącznik opóźnienia streamu dla OBS na Windows. OBS wysyła stream do LiveDelay, a LiveDelay przesyła go dalej na platformy: na żywo albo z opóźnieniem, przełączane bez zrywania połączenia.

Wszystko jest w jednej wtyczce OBS (`livedelay-obs.dll`, C++/Qt z wkompilowanym proxy w Ruście):
- **Panel** (Panele → LiveDelay): stan, opóźnienie, Arm / Activate / Cut / Cut po wyemitowaniu.
- **Ustawienia** (Narzędzia → LiveDelay albo przycisk w panelu): domyślne opóźnienie, limit pamięci, platformy z kluczami, teksty overlayu. Przycisk „Przełącz OBS na LiveDelay” przenosi obecny serwer i klucz z OBS na listę platform i ustawia OBS na `rtmp://127.0.0.1:1935/live`.
- **Skróty** w Ustawienia → Skróty klawiszowe („LiveDelay: …”), np. pod numpad.
- **Overlay**: źródło Przeglądarka z adresem `http://127.0.0.1:7799/overlay` (przycisk „Dodaj do sceny”). Pokazuje LIVE / ARMING… / ARMED / DELAY N s; teksty zmienisz w ustawieniach albo w adresie, np. `?delayed=OPÓŹNIENIE {s}s&live=&size=60`.

Stream Deck: akcja „Hotkey” ze skrótem z OBS albo akcja „Website” z adresem `http://127.0.0.1:7799/cut` itp.

## Instalacja

1. Zbuduj: `obs-pluginuild.bat` (buduje Rusta i wtyczkę).
2. Uruchom `install.ps1`: kopiuje DLL do `%ProgramData%\obs-studio\plugins\livedelay-obsin4bit` (bez admina).
3. W OBS: Narzędzia → LiveDelay → „Przełącz OBS na LiveDelay”, Zapisz.

Ustawienia leżą w `%APPDATA%\obs-studio\plugin_config\livedelay-obs\livedelay.ini`.

Jest też samodzielna wersja z ikoną w trayu (`cargo build --release` → `targetelease\livedelay.exe`) do użycia bez wtyczki.

## Sterowanie (HTTP, tylko 127.0.0.1:7799)

| Akcja | Co robi |
|---|---|
| `POST /arm?delay=15` | zaczyna trzymać bufor (domyślne opóźnienie z ini) |
| `POST /activate` | przełącza na opóźnienie, widzowie cofają się do klatki kluczowej sprzed N s |
| `POST /cut` | wraca na żywo, skok do najnowszej klatki kluczowej |
| `POST /cut-after` | wraca na żywo, gdy obecna chwila dotrze do widzów |
| `POST /disarm` | zwalnia bufor |
| `GET /state`, `GET /events` | stan jako JSON / strumień zdarzeń (SSE) |

GET też działa dla akcji (np. akcja „Website” w Stream Decku).

## Budowanie

```
cargo test
obs-pluginuild.bat
```

Wtyczka potrzebuje w `../deps`: źródeł obs-studio 31.0.3 (nagłówki), Qt 6.6.3 z obs-deps 2024-09-12 i bibliotek importu `obs.lib`/`obs-frontend-api.lib` z zainstalowanego OBS (`obs-plugin/make-import-libs.ps1` tworzy je w `deps/obslibs`).
