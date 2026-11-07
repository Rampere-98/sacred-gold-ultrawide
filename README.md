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

## Autoataque (fork de SacredBild)

Rama [`auto-attack`](https://github.com/Rampere-98/sacred-bild/tree/auto-attack) del fork
[Rampere-98/sacred-bild](https://github.com/Rampere-98/sacred-bild) (GPL-3, como el original). Añade `[AutoAttack]` a
`SacredBild.ini`:

| Clave | Por defecto | Qué hace |
|---|---|---|
| `Enabled` | 0 | Activo al arrancar el juego. |
| `Key` | 122 (F11) | Tecla que lo activa o desactiva (código virtual-key); suena un pitido distinto para cada estado. |
| `Range` | 400 | Radio en píxeles de pantalla alrededor del héroe. |
| `Pause` | 700 | Milisegundos sin autoataque después de mover el ratón o pulsar un botón. |

Con el autoataque activo y el ratón quieto, el héroe ataca al enemigo más cercano dentro del radio. Lo hace igual que
si mantuvieras el clic izquierdo sobre él: reutiliza la selección de objetivo del mando de SacredBild y pone el cursor
del juego sobre el enemigo, así el clic nunca cae en la HUD. Cuando ese enemigo muere, pasa al siguiente. Si mueves el
ratón o haces clic, recuperas el control al momento. Solo actúa en el mundo (no en menús, diálogos ni ventanas
abiertas) y nunca mientras el mando lleva el juego.

La misma rama trae dos arreglos para 32:9:

- **Tirones al andar.** Con la vista ultrawide la caché de capas del suelo (0x8000 entradas) se llenaba, y cada casilla
  nueva obligaba al juego a recorrerla entera: fotogramas de 60-180 ms al entrar en zonas nuevas. Ahora tiene 0x40000
  entradas (~12 MB) y andando se mantiene en 52-60 fps.
- **Ruta automática.** Un clic cerca del borde de una pantalla de 5120 px cae a ~50 casillas, fuera del alcance del
  buscador de rutas (±32): el héroe andaba 30 casillas y se paraba. Ahora, en clics a más de 12 casillas, se planifica
  la ruta con A* sobre todo lo cargado (5x5 sectores, 320x320 casillas, más que la pantalla). Usa la misma prueba de
  casilla transitable que el juego (`0x636C10`) y se recorre en tramos de hasta 24 casillas que el buscador del juego
  sí resuelve. Si un tramo no avanza por la ruta (al cruzar un puente el juego no resuelve tramos largos), se repite
  a 12 y 6 casillas; después marca ese trozo como bloqueado y replanifica. Probado: un solo clic desde el río lleva
  al patio de la ciudad amurallada por el puente (196 casillas). Si no hay ruta, quedan los tramos rectos y girados
  como respaldo; otra orden tuya detiene la marcha.
- **Esquinas negras.** El juego carga 3x3 sectores de 64x64 casillas alrededor de la cámara; en 32:9 con el zoom
  alejado, las esquinas de la pantalla quedaban fuera y salían triángulos negros. `[Render] WideSectors=1` (por
  defecto) carga y dibuja 5x5: el anillo exterior se pide a la caché de sectores del propio juego y se marca para que
  no se descargue. El recorrido de filas y el agua se redirigen a una tabla de 25 sectores, y cada byte se verifica
  antes de parchear (`src/game/wide_sectors.cpp`). El zoom no cambia.

Compilación: el workflow del propio repo (GitHub Actions) genera `ddraw.dll`; basta sustituir el de SacredBild.

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
