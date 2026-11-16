# Texturas con IA y ReShade

## 1. Volcar
`SacredBild.ini`:
```ini
[Textures]
Dump=1
Replace=0
```
Cada contenido de textura distinto que el juego sube se escribe una vez en `SacredBild\textures\dump\<hash>.png`, con una línea en `dump\index.txt` (hash, tamaño, formato). El hash cubre píxeles, tamaño y formato. Es estable entre partidas: en la segunda partida en el mismo sitio salieron 22 texturas nuevas en 2 min (en la primera, ~500). Casi todo es 256×256 A4R4G4B4: hojas de rombos de suelo, objetos y la UI. Las hojas de fuente cambian al vuelo (caché de glifos); de cada superficie se vuelcan como mucho 16 contenidos.

## 2. Escalar
Herramienta: Real-ESRGAN ncnn-vulkan v0.2.5.0 (github.com/xinntao/Real-ESRGAN, BSD-3). Si en este equipo falta `vulkan-1.dll` en System32, copia el cargador del driver de NVIDIA (`DriverStore\FileRepository\nv_dispi.inf_*\vulkan-1-x64.dll`) junto al exe como `vulkan-1.dll`.
```
python -I upscale.py <dump> <salida> <carpeta de realesrgan> --scale 2 [--list hashes.txt]
```
- Las texturas opacas se rellenan envolviendo los bordes, para que los tiles sigan casando.
- Las que tienen alfa se procesan aparte: el color de los píxeles transparentes se rellena con el de sus vecinos (sin halos oscuros), y color y alfa se escalan por separado.
- El resultado x4 se reduce a 2× con Lanczos. Coste: unos 1,1 s por textura de 256 en la RTX 3060, con las 8 copias jugando.

Copia los PNG a `SacredBild\textures\` y pon `Replace=1`. `sheet.py` hace hojas de contacto del volcado.

## 3. Sustituir
Con `Replace=1`, la carpeta se lista una sola vez al arrancar. Los PNG se decodifican en un hilo aparte y la textura D3D9 se crea en el hilo de dibujo; hasta que está lista se dibuja la original.
- Las texturas sustituidas quedan fuera de las páginas del atlas.
- Un suelo sustituido apaga la malla de suelo (`Ground mesh: off`) durante esa sesión. El suelo lo dibuja entonces el camino del juego con el batcher.

## 4. ReShade
ReShade 6.8.0 de 32 bits (`ReShade32.dll`, sacado del instalador oficial de reshade.me).
- Con `[DDraw] On12=0` (sin probar en esta tanda): la vía prevista es `d3d9.dll` junto a `Sacred.exe` (el motor carga sola una `d3d9.dll` local), o `[DDraw] D3D9=`.
- Con `[DDraw] On12=1` (comprobado en A4):
  - Como `dxgi.dll` se engancha a D3D12 bajo D3D9On12 y funciona con FXAA. Es lo que corre en A4.
  - **SMAA hace caer `dxilconv.dll`** (el D3D12 de 32 bits convierte su sombreador), tanto como `d3d9.dll` como como `dxgi.dll`. Usa FXAA.
  - Como `d3d9.dll` con FXAA no está probado.
- Configuración en `reshade/`: FXAA, LiftGammaGain (gamma 1,05, ganancia 1,02), Vibrance 0,2 y CAS (nitidez 0,7). Shaders de CeeJayDK/SweetFX (MIT) más `ReShade.fxh` y `ReShadeUI.fxh` de crosire/reshade-shaders (rama slim), en `reshade-shaders\Shaders`.
- El overlay se abre con la tecla Fin (Inicio es del menú del ModKit). Ctrl+Bloq Despl activa y desactiva los efectos.
