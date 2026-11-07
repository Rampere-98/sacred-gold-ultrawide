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

## Sacred Gold ModKit (fork de SacredBild)

Los mods viven en [Sacred Gold ModKit](https://github.com/Rampere-98/Sacred-Gold-ModKit) (GPL-3, como el original):
`main` es el cuerpo principal (SacredBild sin cambios), cada mod es una rama `mod/*` y `modkit` los junta todos. Ver
la tabla de ramas en su README.

### Autoataque (`mod/auto-attack`)

Añade `[AutoAttack]` a
`SacredBild.ini`:

| Clave | Por defecto | Qué hace |
|---|---|---|
| `Enabled` | 0 | Activo al arrancar el juego. |
| `Key` | 122 (F11) | Tecla que lo activa o desactiva (código virtual-key); suena un pitido distinto para cada estado. |
| `Range` | 400 | Radio en píxeles de pantalla alrededor del héroe. |
| `Pause` | 700 | Milisegundos sin autoataque después de pulsar un botón del ratón (moverlo no lo pausa). |
| `Pickup` | 1 | Recoge los objetos del suelo y abre cofres y otros recipientes, siempre antes de atacar. |
| `LootRange` | 800 | Radio en píxeles para buscar objetos. |
| `Repeat` | 1000 | Cada cuántos milisegundos se repite la orden de ataque mientras el objetivo vive; 0 = una vez. |

Con el autoataque activo, el héroe ataca solo al enemigo más cercano dentro del radio y, cuando muere, pasa al
siguiente. No usa el ratón: le da al héroe la misma orden que da un clic sobre el enemigo (orden tipo 7 con el id del
objetivo, entregada al manejador de órdenes de la criatura, vtable +0x18, como hace el manejador del ratón del juego
en `0x6172C0`). Así el cursor queda libre y puedes usar el inventario y otras ventanas mientras pelea. Un clic tuyo
recupera el control al momento.

Con `Pickup=1` los objetos del suelo van siempre primero, con la misma orden de clic. Se recoge todo lo que el juego
clasifica como objeto de suelo (lo que muestra Alt: categoría de selección 2, oro incluido), y los `cChest3D` (cofres,
baúles; clase por RTTI de MSVC) se abren. Los postes y placas también son categoría 2 y de la misma clase que el oro
(`cSpecial3D`); los delata el bit 0 de sus banderas (`+0x14`: poste `04000001`, armas, anillos y oro `04000000`), así
que se ignoran (y si alguno abriera su cartel, se cierra con Intro y no se vuelve a él). La orden de recoger se da una
vez: repetirla reiniciaba la recogida. Cuando el objeto desaparece del suelo se esperan 1,3 s para no cortar la
animación de agacharse. Lo que no coge o no alcanza (inventario lleno, cofre abierto, tras una valla: 3,5 s sin
acercarse o 3 s junto a él) se reintenta más tarde, hasta tres veces. No actúa en menús, diálogos ni avisos, ni
mientras el mando lleva el juego. Probado en partida: mata enemigos seguidos, también con ventanas abiertas, recoge
oro, armas, armaduras, anillos y pociones e ignora el poste del camino.

### Pociones automáticas (`mod/auto-potion`)

`[AutoPotion]` en `SacredBild.ini` (`Enabled=1`, `FirstPercent=50`, `EmergencyPercent=25`, `Interval=1500`). Bebe una
poción de curación con la tecla del propio juego (Espacio) cuando al héroe le falta tanta vida como cura una poción.
Esa cantidad la aprende midiendo la vida ganada tras cada poción; hasta medir la primera, bebe al faltar el
`FirstPercent` de la vida, y por debajo del `EmergencyPercent` bebe siempre. No actúa con la línea de chat abierta (el
Espacio escribiría) ni con diálogos. Sin probar todavía en combate.

### Menú del ModKit (tecla Inicio)

Inicio abre y cierra, sobre el juego y sus menús, una ventana con los ajustes de cada mod instalado (sectores
amplios, ataque automático, pociones). Los cambios se aplican al momento; *Guardar en SacredBild.ini* los escribe en
el ini (respeta los comentarios). Esc también la cierra. Usa la superposición Dear ImGui de SacredBild, así que pide
`[DDraw] Backend=d3d9`. Cada mod añade su sección con `ModkitMenu::add({título, dibujar, guardar})`; los sectores
amplios solo cambian al reiniciar el juego.

### Arreglos para 32:9 (`mod/smooth-walk`, `mod/click-walk`, `mod/wide-sectors`)

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
