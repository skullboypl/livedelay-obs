# LiveDelay

Lekki przełącznik opóźnienia streamu dla OBS na Windows. OBS wysyła stream do LiveDelay, a LiveDelay przesyła go dalej na platformy: na żywo albo z opóźnieniem, przełączane bez zrywania połączenia.

Dwie części:
- `livedelay.exe` (Rust): proxy z ikoną w trayu. Kolor ikony: zielona na żywo, pomarańczowa uzbrojone, czerwona opóźnienie. Prawy klik: Arm, Activate, Cut, ustawienia.
- `livedelay-obs.dll` (C++/Qt): panel w OBS (Panele → LiveDelay) i skróty w Ustawienia → Skróty klawiszowe („LiveDelay: …”), np. pod numpad. Panel uruchamia proxy razem z OBS.

## Instalacja

1. Zbuduj (niżej) i uruchom `install.ps1`. Kopiuje oba pliki do `%ProgramData%\obs-studio\plugins\livedelay-obsin4bit`.
2. W `%APPDATA%\LiveDelay\livedelay.ini` (otworzysz też z menu w trayu) wpisz adres i klucz platformy, ustaw `enabled = yes`, uruchom proxy ponownie.
3. W OBS: Ustawienia → Transmisja → Usługa **Własna**, Serwer `rtmp://127.0.0.1:1935/live`, Klucz dowolny.

Stream Deck: akcja „Hotkey” ze skrótem z OBS albo akcja „Website” z adresem `http://127.0.0.1:7799/cut` itp.

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
cargo build --release
cargo test
obs-pluginuild.bat
```

Wtyczka potrzebuje w `../deps`: źródeł obs-studio 31.0.3 (nagłówki), Qt 6.6.3 z obs-deps 2024-09-12 i bibliotek importu `obs.lib`/`obs-frontend-api.lib` wygenerowanych z zainstalowanego OBS (`deps/obslibs`).
