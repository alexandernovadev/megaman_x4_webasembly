# Código y datos de terceros

| Componente | Dónde | Autor | Licencia |
|---|---|---|---|
| Anime4K (redes `Restore_CNN_M`, `Upscale_CNN_x2_M`, `Upscale_CNN_x2_S`) | `web/anime4k/` | bloc97 | MIT (`web/anime4k/LICENSE`), commit `7684e95` |
| ScaleFX (5 pasadas) | `web/scalefx/` | Sp00kyFox, vía libretro/glsl-shaders | MIT (cabecera de cada archivo), commit `ad94768` |
| Direcciones de las rutinas de fondos, descarte, ventanas de actores y HUD de Mega Man X4 | `tools/recomp.py`, `runtime/widescreen.c`, `runtime/gpu.c` | análisis de mstan/MegaManX4Recomp | MIT (solo se usan los datos; la implementación es propia) |

Algoritmos implementados a partir de su descripción pública: xBR (Hyllian), sharp-bilinear,
CAS (AMD FidelityFX), Lanczos, Scale2x/EPX.
