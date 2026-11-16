# Sacred Gold ModKit

*English summary: a mod kit for Sacred Gold. The `main` branch is the main body (ultrawide, Direct3D 9, scaled UI,
gamepad); every `mod/*` branch adds one mod on top of it, and `modkit` merges them all, with an agent that plays like
a player. Each branch builds its own `ddraw.dll`.*

El **cuerpo principal** es el motor gráfico (rama `main`, detalles en [MOTOR.md](MOTOR.md)): ultrawide, Direct3D 9, UI escalada, mando. Cada **mod**
es una rama que parte de `main` y añade una sola cosa; la rama `modkit` los junta todos.

| Rama | Mod | Ajuste en `SacredBild.ini` |
|---|---|---|
| `main` | Cuerpo principal: el motor gráfico sin mods. | - |
| [`mod/auto-attack`](https://github.com/Rampere-98/sacred-modkit/tree/mod/auto-attack) | Autoataque con ratón y teclado (F11): lucha solo contra los enemigos que te atacan y los que tú atacas (con tu clic), o busca combate (`Seek`, elegible en el menú Inicio), decidiendo como un jugador (cerca, acercándose o hiriéndote primero) y ataca con las órdenes del propio juego, con tiempos de reacción naturales; la recogida es la del juego (tecla A, o siempre que no haya combate con `AutoCollect`, elegible en el menú Inicio), mejorada: al terminar toma lo que quedó en el radio, abre los cofres y usa todo lo que muestra la mano del cursor (escondites mágicos, pozos; no las runas ni letreros, que solo abren texto), y el autoataque espera a que acabe; además usa las artes de combate según para qué sirven (curas solo si falta vida, área solo con varios enemigos, golpes fuertes solo contra enemigos duros, mejoras al empezar una pelea dura; aprende qué hace cada una) y los consumibles (Mentor, Antídoto, Concentración, Muerte de no-muertos, curar mercenarios); el ratón queda libre para el inventario. | `[AutoAttack]` |
| [`mod/auto-potion`](https://github.com/Rampere-98/sacred-modkit/tree/mod/auto-potion) | Pociones de curación automáticas: bebe (tecla Espacio del juego) cuando falta tanta vida como cura una poción, cantidad que aprende de las que bebe. | `[AutoPotion]` |
| [`mod/smooth-walk`](https://github.com/Rampere-98/sacred-modkit/tree/mod/smooth-walk) | Sin tirones al andar en pantallas anchas: caché de capas del suelo de 0x40000 entradas. | - |
| [`mod/wide-sectors`](https://github.com/Rampere-98/sacred-modkit/tree/mod/wide-sectors) | Sin esquinas negras: carga y dibuja 5x5 sectores alrededor de la cámara en vez de 3x3. | `[Render] WideSectors` |
| [`mod/no-minimize`](https://github.com/Rampere-98/sacred-modkit/tree/mod/no-minimize) | Con `FULLSCREEN : 1` en `Settings.cfg` (ventana sin bordes), el juego ya no se minimiza al seleccionar otra ventana: se parchean sus dos procedimientos de ventana (pausa y `ShowWindow(SW_MINIMIZE)`). | - |
| [`mod/click-walk`](https://github.com/Rampere-98/sacred-modkit/tree/mod/click-walk) | Ruta automática: un clic lejano (más de 30 casillas, o cuando el juego no encuentra camino) planifica la ruta (A*) sobre los sectores cargados y la recorre por tramos; los clics cercanos van al juego tal cual, que sabe entrar por puertas y escaleras. Necesita `mod/wide-sectors` (parte de ella). | - |
| [`modkit`](https://github.com/Rampere-98/sacred-modkit/tree/modkit) | Todos los mods juntos. | todos |

**Instalar:** el juego carga un solo `ddraw.dll`, así que se instala una rama: la del mod que quieras o `modkit` para
todos. El workflow del repo (Actions → *Build*, *Run workflow* sobre la rama) genera el `ddraw.dll`; se copia en la
carpeta del juego con `SacredBild.ini` y `SacredBild\`.

**Menú del ModKit:** la tecla Inicio abre y cierra sobre el juego (también en los menús) los ajustes de cada mod
instalado; los cambios se aplican al momento y *Guardar en SacredBild.ini* los conserva. Necesita `[DDraw] Backend=d3d9`.

**Añadir un mod:** rama `mod/<nombre>` desde `main`, un módulo en `src/game/` con su sección en el ini y en el menú (`ModkitMenu::add`), y después
merge en `modkit`. Notas de ingeniería inversa y herramientas: [sacred-gold-ultrawide](https://github.com/Rampere-98/sacred-gold-ultrawide).

---
Licencia GPLv3: ver [LICENSE](LICENSE) y [NOTICE](NOTICE).
