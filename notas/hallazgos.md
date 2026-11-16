# Hallazgos (REA + Ghidra 12.1.4)

Binario: `Sacred.exe` de Steam (md5 `007580390e5c177a9144bfb66c02d6fe`) desempaquetado con Steamless.
Image base `0x400000`; en `.text`, `.rdata` y `.data` el offset en disco coincide con el RVA.

## Modo de vídeo

- `FUN_00815f70` (WinMain / init): en `0x816c6f` hay `push 0x300`, `push 0x400` y luego `call FUN_00644130`. Esa
  llamada busca el modo (ancho, alto, bpp) en la lista que enumera DirectDraw.
- `FUN_00644830` construye el driver y llama a `FUN_00645260` (`cDxDriver7::setDirectDrawMode`). Este guarda el
  ancho en `driver+0x20` y el alto en `driver+0x1c` y llama a `SetCooperativeLevel` y a
  `SetDisplayMode(w, h, bpp)`.
- El `ChangeDisplaySettings` de 1024x768 (`0x816088`) solo se ejecuta si el escritorio tiene menos de 8 bpp.
- Parche: los inmediatos en `0x816c70` (alto) y `0x816c75` (ancho).

## Renderizador del mundo (0x623000–0x632000)

Usa constantes float compartidas de `.rdata`:

| Constante | Dirección | Uso |
|---|---|---|
| 1024.0f | `0x8906f4` | `FCOMP`: descarte en x (7 sitios); `FMUL`: columnas de tiles (`0x624a12`); escala de proyección (`0x623ba2`, no tocar). |
| 768.0f | `0x892040` | Lo mismo en y. |
| 512.0f | `0x8906fc` | `FSUB`/`FADD`: centro X para el zoom (12 sitios en el bloque). |
| 384.0f | `0x8909f0` | Centro Y. |

El parcheador redirige esos operandos (`D8 /r [abs32]`) a dos floats nuevos (ancho y medio ancho). Los coloca en el
hueco a ceros del final de `.rdata` (`0x8e6f00`) y amplía el VirtualSize de `.rdata` a `0x57000`. Con eso el mapa
llena todo el ancho.

## Por qué no basta

- El paso de coordenadas del mundo a la pantalla resta el centro de la cámara **en enteros**:
  `pos - (cam - 0x200)` y `pos - (cam - 0x180)` (por ejemplo `FUN_0041ec20`). Esas restas están repartidas por el
  código; SacredBild documenta 17 sitios que asumían el centro 512/384.
- El personaje es un modelo Granny 3D (`cGranny::renderComplex`, `FUN_00406510`) y no aparece tras mover solo el
  centro float.
- La HUD se dibuja en coordenadas fijas de 1024x768. Centrarla obliga a desplazar cada ventana de la interfaz y su
  zona de clic. SacredBild lo hace dibujando cada ventana en su propia copia desplazada del espacio 1024x768.
- Otros rectángulos fijos `(0,0,1024,768)`:
  - ventanas `UI_WND_*`: `FUN_006eccf0`, `0x6f9010`, `0x6fe980`, `0x707d00`, `0x709f80`, `0x70a9a0`, `0x70b1d0`,
    `0x70c1d0`, `0x722500`, `0x722d80`, `0x759af0`, `0x75af90`;
  - pantalla de carga: `FUN_006468a0`;
  - captura de pantalla: `FUN_00613670` y `FUN_004cc900`.

## Pruebas en el juego

- Si el juego arranca a pantalla completa sin el foco, se queda minimizado y "no responde" en el vídeo de intro.
  Para recuperarlo: `ShowWindowAsync(h, 9)` + `SetForegroundWindow(h)`. `ShowWindow` síncrono se cuelga.
- Sin SacredBild, el juego lee el ratón por DirectInput y muchos clics inyectados no llegan al menú. Con SacredBild
  sí llegan.
