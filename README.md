# LiveDelay

Lekki przełącznik opóźnienia streamu dla OBS na Windows. OBS wysyła stream do LiveDelay, a LiveDelay przesyła go dalej na platformy: na żywo albo z opóźnieniem, przełączane bez zrywania połączenia.

## Uruchomienie

1. Uruchom `livedelay.exe`. Przy pierwszym starcie obok exe powstaje `livedelay.ini`. Wpisz tam adres i klucz platformy, ustaw `enabled = yes` i uruchom ponownie.
2. W OBS: Ustawienia → Transmisja → Usługa **Własna**, Serwer `rtmp://127.0.0.1:1935/live`, Klucz dowolny.
3. Steruj przez API (panel w OBS i tray w przygotowaniu).

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
```
