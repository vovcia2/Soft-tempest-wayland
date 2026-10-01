# Specyfikacja dla Claude Code — Linux/Wayland soft-TEMPEST display masking overlay

> Wersja Linux/Wayland, równoległa do `SPEC.md` (Android) i `SYNC.md` (Windows).
> W docelowym, osobnym repo nazwij ten plik po prostu `SPEC.md`.
> Ta sama idea: pełnoekranowa, klikalna-na-wylot warstwa szumu pikseli losowanego
> **co klatkę**, składana przez kompozytor nad całą treścią. Narzędzie **ochronne**
> dla własnego urządzenia — nic nie przechwytuje, nie nadaje, nie atakuje.

## Cel projektu

Zbuduj aplikację na Linux/Wayland, która renderuje pełnoekranową (na wszystkich
wyjściach/monitorach) warstwę szumu z per-pikselową alfą, w warstwie **overlay**
kompozytora, aby podnieść poziom szumu w emisji elektromagnetycznej toru wyświetlania
(soft TEMPEST wg Kuhn & Anderson, 1998). Warstwa musi być przezroczysta dla wejścia
(mysz/klawiatura przechodzą pod spód) i renderowana przez GPU.

## Model zagrożenia i świadome ograniczenia (WAŻNE — umieść w README)

1. **Tylko kompozytory z `wlr-layer-shell`.** Protokół `zwlr_layer_shell_v1`
   implementują kompozytory oparte na wlroots (Sway, Hyprland, river, Wayfire…) oraz
   KWin (KDE Plasma). **GNOME/Mutter go NIE wspiera** — tam aplikacja nie zadziała.
   To jest główne ograniczenie, odpowiednik `FLAG_SECURE`/Secure Desktop z innych
   platform. README ma to mówić wprost; wykryj brak globalu i zakończ z czytelnym
   komunikatem zamiast cichej awarii.
2. **Ekran blokady.** Blokady przez `ext-session-lock-v1` rysują nad warstwą overlay —
   nakładka ich nie pokryje (i dobrze). Nie obchodzimy tego.
3. **Direct scanout.** Kompozytor potrafi oddać bufor pełnoekranowej aplikacji wprost
   do wyświetlacza, pomijając kompozycję. Obecność naszej warstwy overlay zwykle
   wymusza z powrotem kompozycję (więc nakładka działa), ale na nietypowych
   konfiguracjach warto to zweryfikować.
4. **Uśrednianie N ramek.** Statyczny szum spada jako √N. Dlatego szum MUSI być
   losowany niezależnie w każdej klatce (time-seeded PRNG w fragment shaderze). Naiwny
   statyczny overlay = złudzenie ochrony.
5. **Bez pomiaru nie ma dowodu skuteczności.** To prototyp/uzupełnienie ekranowania
   sprzętowego, nie zamiennik.
6. **GPU/bateria.** Ciągłe renderowanie pełnoekranowe obciąża GPU; rozważ limit FPS.
7. **X11 to osobny cel.** Na sesji X11 odpowiednikiem jest okno `override-redirect`
   z pustym regionem wejścia — poza zakresem tego repo (Wayland-only).

## Stack techniczny

- Język: **C11** (wayland-client, EGL, OpenGL ES 3.0). Czysto, bez frameworków.
  (Alternatywa: Rust + `smithay-client-toolkit`; jeśli wybierzesz, utrzymaj tę samą
  architekturę.)
- Protokoły: **`wlr-layer-shell-unstable-v1`** (z repo `wlr-protocols`) + rdzeń Wayland
  (`wl_compositor`, `wl_output`, `wl_region`). `xdg-shell` z `wayland-protocols`
  bywa potrzebny jako zależność nagłówków — dołącz, jeśli kompozytor tego wymaga.
- Generowanie kodu protokołów: **`wayland-scanner`** (nagłówki + glue `.c`).
- Grafika: **EGL** (`EGL_PLATFORM_WAYLAND_KHR`) + `wl_egl_window` + **GLES 3.0**,
  animacja fragment shaderem.
- Build: **Meson + Ninja** (idiomatyczne dla projektów Wayland).
- CI: **GitHub Actions**, runner `ubuntu-latest`, artefakt — zbudowany plik wykonywalny.

## Architektura — warstwa overlay per-output

- Zbinduj globale z `wl_registry`: `wl_compositor`, `zwlr_layer_shell_v1`, wszystkie
  `wl_output`. Jeśli `zwlr_layer_shell_v1` nie istnieje → komunikat i wyjście (p. 1).
- **Dla każdego `wl_output`** utwórz osobną powierzchnię:
  1. `wl_compositor.create_surface()` → `wl_surface`.
  2. `zwlr_layer_shell_v1.get_layer_surface(surface, output,
     ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "soft-tempest")`.
  3. `zwlr_layer_surface_v1.set_anchor(TOP|BOTTOM|LEFT|RIGHT)` → pełny ekran.
  4. `set_exclusive_zone(-1)` → pokryj także obszary paneli/belek.
  5. `set_keyboard_interactivity(NONE)`.
  6. **Klik-na-wylot:** `wl_surface.set_input_region()` z **pustym** regionem
     (`wl_compositor.create_region()` bez `add`). Wejście przechodzi pod spód.
  7. `wl_surface.commit()`, poczekaj na `configure`, zrób `ack_configure`.
- Po `configure` znasz rozmiar wyjścia → utwórz `wl_egl_window` i powierzchnię EGL,
  potem pętlę renderowania.
- Reaguj na hotplug monitorów (nowe/usuwane `wl_output`): twórz/niszcz powierzchnie.
- Obsłuż `scale`/`fractional-scale` wyjścia — renderuj w pikselach fizycznych
  (bufor * scale), by szum był per-piksel fizyczny.

## Potok EGL/GLES

1. `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, NULL)`, `eglInitialize`.
2. `eglChooseConfig` z `EGL_ALPHA_SIZE 8` i `EGL_RENDERABLE_TYPE EGL_OPENGL_ES3_BIT`.
3. `wl_egl_window_create(surface, w, h)` → `eglCreateWindowSurface`.
4. Kontekst GLES3 (`EGL_CONTEXT_MAJOR_VERSION 3`).
5. Włącz blending: `glEnable(GL_BLEND)`, `glBlendFunc(GL_SRC_ALPHA,
   GL_ONE_MINUS_SRC_ALPHA)`; czyść na `glClearColor(0,0,0,0)`.
6. **Pętla klatek przez frame callback:** `wl_surface.frame()` → w callbacku renderuj
   i `eglSwapBuffers` (synchronizacja z kompozytorem). To daje per-klatkowy szum
   zgodny z odświeżaniem.

## Projekt fragment shadera (GLSL ES 3.0)

Taki sam jak w wersji Android — reużyj. Uniformy: `uTime (float)`,
`uAmplitude (float)`, `uMode (int)`, `uResolution (vec2)`.

- **Per-pixel, per-frame PRNG** (PCG hash z `gl_FragCoord.xy` + ziarno czasu), by szum
  zmieniał się co klatkę (kluczowe wobec √N).
- **Tryby**: `0` biały; `1` HF poziomy (energia w wysokich częstotliwościach poziomych —
  zegar pikseli; naprzemienny znak co kolumnę modulowany losowo); `2` temporal
  (dodatkowa losowa modulacja alfy per piksel/klatkę).
- Wyjście: `fragColor = vec4(vec3(n), uAmplitude * a);` (zwykła alfa, bo blending
  `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`).

Vertex shader: pełnoekranowy trójkąt z `gl_VertexID` (bez bufora), `glDrawArrays(
GL_TRIANGLES, 0, 3)`.

```glsl
// fragment, #version 300 es
precision highp float; precision highp int;
out vec4 fragColor;
uniform float uTime; uniform float uAmplitude; uniform int uMode; uniform vec2 uResolution;

uint pcg(uint v){ uint s=v*747796405u+2891336453u; uint w=((s>>((s>>28u)+4u))^s)*277803737u; return (w>>22u)^w; }
uint hash3(uvec3 v){ return pcg(v.x ^ pcg(v.y ^ pcg(v.z))); }
float rnd(uvec3 v){ return float(hash3(v) & 0x00FFFFFFu) / float(0x01000000u); }

void main(){
    uvec2 pix = uvec2(gl_FragCoord.xy);
    uint frame = uint(uTime * 240.0);
    float n = rnd(uvec3(pix, frame));
    float a = uAmplitude;
    if (uMode == 1){
        float alt = ((pix.x & 1u)==0u) ? 1.0 : -1.0;
        n = 0.5 + 0.5*alt*(2.0*n - 1.0);
    } else if (uMode == 2){
        a *= rnd(uvec3(pix.x ^ 2654435769u, pix.y, frame ^ 2246822519u));
    }
    fragColor = vec4(vec3(n), a);
}
```

## Sterowanie / UX

- **MVP:** uruchomienie startuje nakładkę; `SIGINT`/`SIGTERM` sprząta i wychodzi.
- Argumenty CLI: `--amplitude 0.25`, `--mode white|hf|temporal`, `--fps-cap N`,
  `--output <name>` (ograniczenie do jednego wyjścia).
- Odczyt/zapis konfiguracji z `$XDG_CONFIG_HOME/soft-tempest/config` (opcjonalnie).
- Integracja z sesją (autostart) przez plik `.desktop` w `~/.config/autostart/` lub
  jednostkę user systemd — jako dodatek, nie część MVP.

## Struktura plików (proponowana)

```
/ (repo root)
├── meson.build
├── protocol/
│   ├── wlr-layer-shell-unstable-v1.xml   # z wlr-protocols
│   └── xdg-shell.xml                      # z wayland-protocols (jeśli potrzebny)
├── src/
│   ├── main.c            # wl_display, registry, pętla zdarzeń, sygnały, CLI
│   ├── overlay.c/.h      # layer-surface per-output, input region, configure
│   ├── render.c/.h       # EGL + GLES, program, uniformy, frame callback
│   ├── shaders.h         # źródła GLSL jako stałe (noise_vs, noise_fs)
│   └── config.c/.h       # odczyt CLI/pliku konfiguracyjnego
├── .github/workflows/build.yml
├── README.md             # w tym sekcja ograniczeń z tego dokumentu
└── SPEC.md               # ten plik (w repo pod tą nazwą)
```

## Generowanie kodu protokołów (Meson)

`wayland-scanner` generuje z XML nagłówki i glue. W `meson.build`:
- znajdź `wayland-scanner` (`find_program`) i `wayland-protocols` (pkg-config
  `pkgdatadir`),
- dla każdego `.xml`: wygeneruj `*-client-protocol.h` (`client-header`) i `*.c`
  (`private-code`), dołącz do źródeł targetu.
- `wlr-layer-shell-unstable-v1.xml` dołącz w repo (z wlr-protocols), bo nie zawsze jest
  w systemowym `wayland-protocols`.

## Zależności (pakiety dev)

`wayland-client`, `wayland-egl`, `wayland-protocols`, `wayland-scanner`, `egl`,
`glesv2`. Na Ubuntu: `libwayland-dev wayland-protocols libegl1-mesa-dev
libgles2-mesa-dev meson ninja-build pkg-config`.

## GitHub Actions — build

`.github/workflows/build.yml`:

```yaml
name: Build (Linux/Wayland)

on:
  push:
    branches: [ main ]
  pull_request:
  workflow_dispatch:

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install deps
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            libwayland-dev wayland-protocols \
            libegl1-mesa-dev libgles2-mesa-dev \
            meson ninja-build pkg-config

      - name: Configure
        run: meson setup build --buildtype=release

      - name: Build
        run: meson compile -C build

      - name: Upload binary
        uses: actions/upload-artifact@v4
        with:
          name: soft-tempest-linux
          path: build/soft-tempest
          if-no-files-found: error
```

Uwagi:
- CI tylko **buduje** — uruchomienie wymaga działającego kompozytora Wayland z
  layer-shell, czego na runnerze nie ma. Testy działania rób lokalnie na Sway/Hyprland.
- Jeśli dołączasz `wlr-layer-shell-unstable-v1.xml` w repo, build nie zależy od wersji
  systemowego `wayland-protocols` dla tego pliku.
- Dopasuj nazwę artefaktu/ścieżki do `project()` w `meson.build`.

## Szkic meson.build (dla Claude Code do rozwinięcia)

```meson
project('soft-tempest', 'c', default_options: ['c_std=c11', 'warning_level=2'])

wl_client = dependency('wayland-client')
wl_egl    = dependency('wayland-egl')
egl       = dependency('egl')
gles      = dependency('glesv2')

scanner = find_program('wayland-scanner')
# TODO: custom_target dla protocol/*.xml -> *-client-protocol.h + *.c (private-code)

executable('soft-tempest',
  ['src/main.c', 'src/overlay.c', 'src/render.c', 'src/config.c',
   # + wygenerowane pliki protokołów
  ],
  dependencies: [wl_client, wl_egl, egl, gles],
  install: true)
```

## Kolejność pracy dla Claude Code

1. Szkielet Meson + generowanie protokołów + `main.c`, który łączy się z `wl_display`,
   binduje globale i **wykrywa brak layer-shell** (komunikat + wyjście). CI ma budować.
2. Jedna powierzchnia layer-surface na pierwszym wyjściu: anchor all, pusty input region,
   configure/ack — pusta (przezroczysta) warstwa overlay.
3. EGL + GLES: kontekst, czyszczenie na przezroczysto, frame callback, `eglSwapBuffers`.
4. Fragment shader: biały szum per-frame; podłącz uniformy.
5. Tryby (HF poziomy, temporal) + amplituda + limit FPS; CLI.
6. Multi-output (po jednej powierzchni na `wl_output`) + obsługa scale i hotplug.
7. README z sekcją ograniczeń z tego dokumentu.
8. Utrzymuj zielony build w Actions na każdym etapie.

## Definicja „gotowe" (MVP)

- Binarka buduje się w GitHub Actions i jest do pobrania jako artefakt.
- Na kompozytorze z layer-shell (Sway/Hyprland/KWin) po uruchomieniu pojawia się
  klik-na-wylot szum na wszystkich wyjściach; `Ctrl+C` sprząta i kończy.
- Na kompozytorze bez layer-shell (GNOME) aplikacja wypisuje czytelny komunikat i
  kończy, zamiast cicho zawieść.
- Szum zmienia się co klatkę; amplituda i tryby działają.
- README zawiera uczciwy opis ograniczeń (brak layer-shell na GNOME, ekran blokady,
  direct scanout, √N, konieczność pomiaru, status „uzupełnienie ekranowania, nie
  zamiennik").
