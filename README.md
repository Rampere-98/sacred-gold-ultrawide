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
| `Pickup` | 1 | Recoge los objetos del suelo y abre cofres y otros recipientes, según la situación (ver abajo). |
| `LootRange` | 800 | Radio en píxeles para buscar objetos. |
| `Repeat` | 1000 | Cada cuántos milisegundos se repite la orden de ataque mientras el objetivo vive; 0 = una vez. |

Con el autoataque activo, el héroe ataca solo al enemigo más cercano dentro del radio y, cuando muere, pasa al
siguiente. No usa el ratón: le da al héroe la misma orden que da un clic sobre el enemigo (orden tipo 7 con el id del
objetivo, entregada al manejador de órdenes de la criatura, vtable +0x18, como hace el manejador del ratón del juego
en `0x6172C0`). Así el cursor queda libre y puedes usar el inventario y otras ventanas mientras pelea. Un clic tuyo
recupera el control al momento.

Decide según la situación, como un jugador: cada 180-340 ms mira a los enemigos (distancia, si se acercan, su vida)
y si el héroe ha perdido vida hace poco. Con amenaza (enemigos a menos de 0,45 × `Range`, acercándose o hiriéndole)
combate primero y solo recoge lo que tiene a sus pies; sin amenaza va a lo más cercano, botín o enemigo. Se queda con
el enemigo que combate (en el cuerpo a cuerpo «se acerca» no cuenta: hacía saltar de objetivo cada segundo), remata a
los heridos y reacciona a un objetivo nuevo tras 220-480 ms (90-220 bajo amenaza); la orden de ataque se repite a
intervalos irregulares. Con `Pickup=1` recoge con la misma orden de clic. Se recoge todo lo que el juego
clasifica como objeto de suelo (lo que muestra Alt: categoría de selección 2, oro incluido), y los `cChest3D` (cofres,
baúles; clase por RTTI de MSVC) se abren. Los postes y placas también son categoría 2 y de la misma clase que el oro
(`cSpecial3D`); los delata el bit 0 de sus banderas (`+0x14`: poste `04000001`, armas, anillos y oro `04000000`), así
que se ignoran (y si alguno abriera su cartel, se cierra con Intro y no se vuelve a él). La orden de recoger se da una
vez: repetirla reiniciaba la recogida. Cuando el objeto desaparece del suelo se esperan 1,3 s para no cortar la
animación de agacharse. Lo que no coge o no alcanza (inventario lleno, cofre abierto, tras una valla: 3,5 s sin
acercarse o 3 s junto a él) se reintenta más tarde, hasta tres veces. No actúa en menús, diálogos ni avisos, ni
mientras el mando lleva el juego. Probado en partida: mata enemigos seguidos, también con ventanas abiertas, recoge
oro, armas, armaduras, anillos y pociones e ignora el poste del camino.

### Solo contra quien ataca (`mod/auto-attack`)

Decisión del usuario: el autoataque lucha solo contra los enemigos que atacan al héroe y los que el jugador ataca. RE en
vivo (ReadProcessMemory, comparando enemigos que pegaban con otros quietos): la criatura guarda su objetivo en +0x104
(id) con la posición del objetivo en +0x108/+0x10C; quieta o andando, ahí hay un punto del mapa. Un enemigo cuenta
si apunta al héroe (o a su montura); los que el héroe apunta tras un clic del jugador se luchan hasta el final.
Probado: atacó a los dos que le pegaban y dejó en paz a los que estaban quietos cerca.

### A caballo, golpe rápido y atacantes lejanos (`mod/auto-attack`)

- **A caballo:** el «controlado» (`cEngine +0x13C`) es el caballo, pero el clic sobre una criatura el juego lo manda al
  jinete (orden tipo 7 al héroe, visto con un registro temporal de órdenes en el manejador vtable +0x18). Mandado al
  caballo, solo se acercaba y no pegaba. Ahora la orden de ataque va al héroe si va montado. Probado: mata montado.
- **Golpe lento:** cada orden de ataque es un golpe; repetirla cada 1000 ms pegaba una vez por segundo, mucho más lento
  que mantener el clic. `Repeat` por defecto 200 ms.
- **Atacantes lejanos:** los que atacan al héroe se persiguen hasta 4 veces `Range` (arqueros, magos). Sin probar con
  arqueros.

### Modos en el menú Inicio (`mod/auto-attack`)

En la sección «Ataque automático»: contra quién lucha («Solo quien me ataca (y a quien ataco yo)» o «Buscar combate»,
`[AutoAttack] Seek`), «Usar habilidades» y «Usar consumibles» (los mismos interruptores que `[Tactics]`), y la recogida
(«Solo al pulsar A» o «Recoger siempre», `AutoCollect`). Recoger siempre pulsa A por el jugador cuando hay objetos en
el radio y no hay combate; solo objetos (abrir cofres abre la ventana del baúl del pueblo), nunca antes de 2,5 s desde
el último clic ni con el héroe andando, y lo que no se pudo coger no se repite en bucle. Probado: combate, arte de
combate, y al acabar recoge el botín solo.

### Recogida con A (`mod/auto-attack`, `[AutoAttack] Pickup`)

El autoataque ya no recoge por su cuenta (decisión del usuario: nada de «estilo bot»). La recogida es la del juego:
la tecla A (y el botón de la barra) envía la orden 0x113 (`ENG 00618ba1`: `cOrder` con vtable `0x89095c`, el id del
jugador en +0x0C). El mod solo la mejora: el autoataque espera mientras el héroe recoge y, cuando se queda quieto
(0,9 s), toma uno a uno lo que quedó en `LootRange` (objetos que no alcanzó y cofres, que la A del juego no abre) con la
orden de clic. Un clic, un golpe recibido o A otra vez lo terminan.

### Pociones automáticas (`mod/auto-potion`)

`[AutoPotion]` en `SacredBild.ini` (`Enabled=1`, `FirstPercent=50`, `EmergencyPercent=25`, `Interval=1500`). Bebe una
poción de curación con la tecla del propio juego (Espacio) cuando al héroe le falta tanta vida como cura una poción.
Esa cantidad la aprende midiendo la vida ganada tras cada poción (menos de 8 puntos o del 6 % es regeneración, no poción: sin pociones aprendía «cura 2» y bebía sin parar); hasta medir la primera, bebe al faltar el
`FirstPercent` de la vida, y por debajo del `EmergencyPercent` bebe siempre. No actúa con la línea de chat abierta (el
Espacio escribiría) ni con diálogos. Sin probar todavía en combate.

### Tácticas: artes de combate y consumibles (`mod/auto-attack`, `[Tactics]`)

Con el autoataque, cada ranura de arte (teclas 6-0) se usa según para qué sirve:
- **Curas:** solo si falta un 35 % de vida (un 25 % en apuros).
- **Área:** solo con 3 enemigos o más junto al objetivo (2 en apuros).
- **Golpes de un objetivo:** solo contra enemigos que lo merecen (1,8 veces más vida que el enemigo habitual, o más
  de 8 s con el ataque normal); nunca contra los que caen en menos de 3 s.
- **Mejoras:** al empezar una pelea dura, como mucho cada 45 s.

El uso de cada ranura se aprende de lo que hace el arte:
- vida recuperada → cura;
- varios enemigos heridos → área;
- el objetivo herido → un objetivo;
- nada visible con el objetivo cerca, cuatro veces → mejora.

Lo aprendido queda en `[Tactics.Learned]` y se puede fijar a mano en el menú (Inicio).

Cómo se lanza un arte sin ratón:
1. La tecla de la ranura selecciona el arte (eso fija `engine+0x58`, el tipo, y `+0x5A`, el arte).
2. Se da la misma orden que el clic derecho, tipo 0x105 por `cEngine::sendOrder`:
   - `+0x10` objetivo; `+0x14` palabra de posición; `+0x18`/`+0x1C` x, y; `+0x28` 100;
   - `+0x2C` tipo de arte; `+0x30` = `0x552370(arte, 0)`, que devuelve el propio id; `+0x34` capa.
3. Después se vuelve a la ranura que tenía el jugador.

Lo comprobé comparando en el log mi orden con la del clic derecho del juego.

Tabla de ranuras de cada jugador: `*0xAD4448 + (id del héroe & 0xFFFF) * 0x1180`.
- `+0xC90`: entradas de 20 bytes, dos por ranura (barra alternativa, `+0x1162` bit 1); tipo (palabra) y arte en `+4`.
- `+0xD5A`: la ranura activa.

Consumibles: `usePotion` (`0x612FA0`, thiscall sobre el motor, `(tipo, aMercenarios)`) devuelve si quedaba poción.
Tipos: 1 curación (Espacio; B la da a los mercenarios), 2 Muerte de no-muertos (Q), 3 Mentor (W), 4 Antídoto (E),
5 Concentración (R). Cuándo se usa cada uno:
- **Mentor:** en peleas grandes (4 enemigos o uno fuerte), como mucho cada 2 min.
- **Antídoto:** si la vida baja sin enemigos cerca (veneno).
- **Concentración:** si las artes necesarias se están recargando.
- **Muerte de no-muertos:** en apuros con 3 enemigos o más encima.
- **Curar mercenarios:** si un aliado que acompaña al héroe más de 20 s baja del 45 %.

Si no queda poción, vuelve a mirar al cabo de 60 s.

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

**Costura en el suelo (corregida):** con 5x5 sectores la vista alejada baja más allá de los 3x3 del juego aunque su
esquina esté dentro; el recorrido de filas del juego sigue entonces con índices de sector > 8 (10 bajo el sector de
abajo a la izquierda), que en la rejilla 5x5 nombran otro sector: la mitad inferior se dibujaba con el suelo de otro
sitio, sin objetos y con un borde en zigzag. Esas filas toman ahora la posición calculada desde el ancla del fotograma
(`resolution.cpp`, `renderRowClipped`).

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

**Recoger todo lo que muestra la mano (2026-11-07):** la lista de selección del juego (`cWorldView` +0x96B24) trae,
además de objetos, entradas con el bit 31: cosas fijas del mapa. Sus registros están en `[0xAB45B8]` (0x58 bytes,
índice = 16 bits bajos del id; nombre de guion en +0x18). Usables (campo +4 = 0x00010000): `BRUNNEN` (pozos),
`F_Gold`, `VERSTECK` (marcadores sin gráfico). De texto o guion (+4 = 0.25f): `F_Stein` (runas), `Wegweiser`,
`setvar`, `Powerup`: esos no se cogen. El clic del juego les da la orden 7 con el id y el punto del mundo bajo el
cursor (`0x617939`). "Recoger siempre" ya incluye cofres; si uno abre una ventana (arcón), no se vuelve a él.
Objetivos a más de 24 casillas por eje (el buscador de rutas cubre ±32) reciben antes una orden de andar, que la ruta
automática divide en tramos. Pendiente: identificar qué objeto es el "escondite mágico" que el jugador ve.
