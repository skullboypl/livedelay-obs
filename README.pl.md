# LiveDelay dla OBS

<sub>[English](README.md) · **Polski**</sub>

Włączasz i wyłączasz opóźnienie streamu w trakcie nadawania, bez zrywania połączenia. LiveDelay siedzi w OBS między OBS a platformami: **Arm**, potem **Activate**, i widzowie są N sekund z tyłu. **Cut** i są z powrotem na żywo.

- **Jedna wtyczka, bez dodatkowego programu.** Proxy opóźnienia działa w środku OBS (rdzeń w Ruście, ok. 0,1% CPU i 30 MB RAM przy 15 s opóźnienia i 6 Mb/s).
- **Natywny panel.** Okno Qt, nie przeglądarka, więc prawie nic nie kosztuje.
- **Skróty.** Arm, Activate, Cut i „Cut po wyemitowaniu” w Ustawienia → Skróty klawiszowe. Podepniesz je pod numpad albo Stream Deck.
- **Kilka platform naraz.** Twitch, YouTube, Kick, Facebook albo dowolny serwer RTMP/RTMPS.
- **Overlay.** Źródło Przeglądarka, które pokazuje widzom, co się dzieje: LIVE, ARMING…, ARMED, DELAY 25s.
- **Polski, English, Deutsch, Русский.**

![Stany overlayu](docs/overlay-preview.png)

## Jak to działa

OBS wysyła stream do LiveDelay na `rtmp://127.0.0.1:1935/live`. LiveDelay trzyma ostatnie sekundy w RAM i wysyła stream dalej na platformy.

- **Arm** zaczyna trzymać bufor na ustawione opóźnienie.
- **Activate** cofa widzów do klatki kluczowej sprzed co najmniej N sekund. Znaczniki czasu rosną dalej, więc odtwarzacz widzi jeden ciągły stream.
- **Cut** przeskakuje do najnowszej klatki kluczowej: z powrotem na żywo.
- **Cut po wyemitowaniu** zapamiętuje tę chwilę i wraca na żywo, gdy dotrze ona do widzów. Przydaje się po końcu meczu.

## Instalacja

1. Zamknij OBS.
2. Rozpakuj `livedelay-obs-<wersja>-windows-x64.zip` do folderu OBS (zwykle `C:\Program Files\obs-studio`) i potwierdź scalanie.
3. Uruchom OBS i otwórz **Narzędzia → LiveDelay**.
4. Kliknij **Przełącz OBS na LiveDelay**. Przenosi obecny serwer i klucz z OBS na listę platform i ustawia OBS na LiveDelay. Kliknij **Zapisz**.
5. Panel włączysz przez **Panele → LiveDelay**.

Wymaga OBS Studio 31 lub nowszego, Windows 10/11 x64.

## Overlay

Dodaj źródło Przeglądarka z adresem `http://127.0.0.1:7799/overlay` (albo kliknij **Dodaj do sceny** w ustawieniach). `http://127.0.0.1:7799/overlay?preview` pokazuje wszystkie stany naraz.

W ustawieniach zmienisz teksty dla stanów Na żywo, Uzbrajanie, Uzbrojone i Opóźnienie. `{s}` to opóźnienie w sekundach, `{buf}` to ile sekund jest już w buforze. Pusty tekst ukrywa overlay w danym stanie. Opcje dla jednego źródła w adresie: `?size=60` (rozmiar czcionki w px), `?live=&delayed=OPÓŹNIENIE%20{s}s`. Wygląd zmienisz polem „Własny CSS” w źródle (`#tag`, `.live`, `.arming`, `.armed`, `.delayed`).

## Stream Deck i skrypty

Akcja **Hotkey** ze skrótami z OBS albo lokalne API (tylko 127.0.0.1):

| Żądanie | Co robi |
|---|---|
| `POST /arm?delay=15` | zaczyna bufor na 15 s |
| `POST /activate` | włącza opóźnienie |
| `POST /cut` | wraca na żywo teraz |
| `POST /cut-after` | wraca na żywo, gdy ta chwila zostanie wyemitowana |
| `POST /disarm` | zwalnia bufor |
| `GET /state`, `GET /events` | stan jako JSON albo wypychany jako zdarzenia (SSE) |

Akcje działają też przez GET, więc zadziała akcja **Website** w Stream Decku. Żądania z innych stron WWW są odrzucane, więc żadna strona nie wytnie Ci streamu.

## Ograniczenia

- Brak Enhanced Broadcasting Twitcha (wiele ścieżek). **Przełącz OBS na LiveDelay** ustawia własny serwer, co je wyłącza.
- Zamknięcie OBS w trybie opóźnienia kończy stream od razu: ostatnie N sekund nie zostanie wyemitowane.
- Zapis ustawień w trakcie streamu uruchamia LiveDelay ponownie i przerywa stream na kilka sekund. Okno pyta o to wcześniej.

## Budowanie

Potrzebne: Visual Studio 2022 (C++), Rust (stable, MSVC) i w `../deps`: źródła obs-studio 31.0.3, Qt 6.6.3 z obs-deps 2024-09-12 oraz biblioteki importu z zainstalowanego OBS.

```
obs-plugin\make-import-libs.ps1
obs-plugin\build.bat
cargo test
package.ps1
```

`install.ps1` instaluje build na tym komputerze bez uprawnień administratora (`%ProgramData%\obs-studio\plugins`). Samodzielna wersja z ikoną w trayu na tym samym rdzeniu: `src/main.rs` (`cargo build --release`).

## Wsparcie

LiveDelay jest darmowy. Jeśli kiedyś uratuje Ci stream: [wesprzyj autora](https://donate.skullmedia.pl). Od tego samego autora: [SmoothWizard](https://smoothwizard.com), FPS booster dla Windows.

## Licencja

GPL-2.0-or-later. Zobacz [LICENSE](LICENSE).
