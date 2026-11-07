# Sacred Gold en ultrawide (32:9)

*English summary: notes and tools from getting **Sacred Gold** (Steam, 2004) to run on a 5120x1440 (32:9)
monitor. The working solution is the open-source [SacredBild](https://github.com/kirides/sacred-bild) hook. This
repo documents the reverse-engineering path taken first (REA + Ghidra), a binary patcher that widens the display
mode and the world renderer, and why that path stops short. No game files are included.*

## Resultado

**Solución que funciona: [SacredBild](https://github.com/kirides/sacred-bild)** (GPL-3, `ddraw.dll` hook).
Probado a 5120x1440 con el `Sacred.exe` original de Steam:

- el mundo llena el 32:9 a su densidad de píxel original, con el personaje centrado;
- la barra inferior centrada y los demás paneles de la HUD en los bordes;
- menús y pantallas de carga escalados a la altura y centrados.

Instalación: descomprimir la release en la carpeta del juego y arrancar desde Steam como siempre. Antes del juego
sale su ventana de ajustes (resolución = escritorio por defecto). Es incompatible con dgVoodoo2: los dos son
`ddraw.dll`.

Alternativas probadas o valoradas:

| Opción | Estado |
|---|---|
| dgVoodoo2 solo | Escala el 4:3 con bandas negras; no amplía la vista. |
| Parche propio (este repo) | Amplía modo de vídeo y renderizador del mapa; el personaje y la HUD quedan mal. |
| [PureHD](https://www.pcgamingwiki.com/wiki/Sacred#High-resolution_rendering) 1.40 | Panorámico "hasta 1080p"; instalado pero sustituido por SacredBild sin probarlo en partida. |
| SacredBild | **Funciona.** Solo publica nightlies por ahora. |

## Qué hay aquí

| Ruta | Contenido |
|---|---|
| [`docs/hallazgos.md`](docs/hallazgos.md) | Direcciones y funciones encontradas con REA/Ghidra, y por qué el parche propio no basta. |
| [`patcher/patch_sacred.py`](patcher/patch_sacred.py) | Parcheador verificable: comprueba cada byte original antes de escribir. |
| [`tools/rea_bridge/`](tools/rea_bridge) | Puente TCP a una sesión persistente de REA (`rea-agents --mcp`) para no reanalizar en cada consulta. |
| [`tools/scan_constants.py`](tools/scan_constants.py) | Busca 1024/768 como inmediatos y floats en un PE. |
| [`tools/relative_mouse.py`](tools/relative_mouse.py) | Envía movimiento relativo de ratón (SendInput) para juegos con DirectInput. |

## Flujo de ingeniería inversa

1. **Quitar el DRM.** El `Sacred.exe` de Steam lleva SteamStub v2 (sección `.bind`, `.text` cifrado). Se
   desempaqueta con [Steamless](https://github.com/atom0s/Steamless) (`Steamless.CLI.exe Sacred.exe`).
2. **Analizar con REA + Ghidra 12.1 (JDK 21).** Por CLI, REA vuelve a analizar en cada llamada (unos 9 min con este
   exe). La solución es una sesión `--mcp` persistente detrás de [`tools/rea_bridge/bridge.py`](tools/rea_bridge/bridge.py), con:
   - `REA_GHIDRA_STARTUP_TIMEOUT_MS=7200000`: el valor por defecto de 330 s no basta;
   - `REA_GHIDRA_DISABLED_ANALYZERS="Decompiler Parameter ID"`: ese analizador es el que se come el tiempo.
3. **Parchear y probar.** El parcheador trabaja sobre el exe desempaquetado y se niega a escribir si algún byte
   esperado no coincide.

```bash
# Python 3, sin dependencias
python patcher/patch_sacred.py Sacred.unpacked.exe Sacred.exe 2730 768
```

Requiere dgVoodoo2 con `ExtraEnumeratedResolutions = 2730x768` para que el modo exista. Con eso el mapa llena el
ancho, pero el personaje no aparece y la HUD sigue a la izquierda: ver [hallazgos](docs/hallazgos.md#por-qué-no-basta).

```bash
# Puente REA (Windows; requiere Node.js, Ghidra 12.1 y un JDK 21)
set GHIDRA_INSTALL_DIR=C:\ruta\ghidra_12.1.4_PUBLIC
set JAVA_HOME=C:\ruta\jdk-21
python -I -u tools/rea_bridge/bridge.py
python tools/rea_bridge/call.py open_binary "{\"path\": \"C:/ruta/Sacred.unpacked.exe\"}"
python tools/rea_bridge/call.py procedure_pseudo_code "{\"procedure\": \"0x645260\"}"
```

## Avisos

- No se incluye ningún fichero del juego ni de terceros (exe, DLL, `Settings.cfg`): necesitas tu copia legal.
- Las direcciones son del `Sacred.exe` de Steam con md5 `007580390e5c177a9144bfb66c02d6fe`, una vez desempaquetado.
- Herramientas usadas: [REA](https://www.npmjs.com/package/rea-agents) 6.7.0, Ghidra 12.1.4, Steamless,
  dgVoodoo2 2.87.6.

Licencia de los scripts y la documentación: MIT.
