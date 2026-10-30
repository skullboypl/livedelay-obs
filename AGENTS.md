# LiveDelay — wspólny silnik

Rust i plik DLL silnika są tylko w tym repozytorium (`skullboypl/livedelay-obs`). Aplikacja obok Streamlabs jest w osobnym repozytorium `skullboypl/livedelay-standalone-app` i nie ma własnej kopii silnika.

## Co gdzie leży

- `src/*.rs`, `Cargo.toml` — jedyny kod opóźnienia, RTMP i HTTP.
- `cargo build --release` w tym katalogu daje dwa pliki z tej samej skrzynki:
  - `target/release/livedelay.lib` — staticlib, linkowane do wtyczki `livedelay-obs.dll`
  - `target/release/livedelay.dll` — cdylib, ładowane przez exe aplikacji
- `obs-plugin/` — tylko OBS (doki, enkodery, GoLive, tłumaczenia). Dalej linkuje staticlib, nie LoadLibrary.
- `src/main.rs` i `src/tray.rs` — stary host Rust z ikoną przy zegarze. Nowy interfejs jest w repozytorium aplikacji. Nie dokładaj tam drugiego silnika.

## Czego nie robić

- Nie kopiuj `src/*.rs`, `Cargo.toml` ani `target/` do `livedelay-standalone-app`.
- Nie buduj drugiej kopii proxy w C++ ani w OpenGL. Exe woła `ld_start` / `ld_stop` i HTTP `127.0.0.1:7799`.
- Nie commituj `dist/` ani `target/`.
- Dwa zipy, dwa repozytoria, jeden silnik. Zmiana RTMP, delay albo API należy tutaj, potem ponowny `cargo build --release` i podmiana `livedelay.dll` obok exe.
- Panel ustawień w przeglądarce to `GET /panel` w tym silniku, w tym samym kroju co livedelay.vxh.pl. Fonty leżą w `src/fonts/` i serwuje je `GET /fonts/…`, bo przeglądarka nie weźmie ich z obcej domeny. `GET /config` i `POST /config` tylko z localhost, żeby klucze streamu nie wyszły poza maszynę.
