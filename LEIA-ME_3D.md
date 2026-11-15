# FCEUmm 3D — núcleo RetroArch (Android) com modo 3D

Base: FCEUmm (já emula **NES e Famicom Disk System**). O modo 3D é o renderizador do
`Famicom_Retro_FDS_v8_render3D.html` portado para C/OpenGL ES 3: relevo do cenário por cor,
sprites extrudados, AO, sombras com a luz do sol, céu, 5 predefinições de luz.

## Compilar (o jeito mais fácil: GitHub Actions)
1. Crie um repositório e suba esta pasta.
2. Aba **Actions → Android core (3D) → Run workflow**.
3. Baixe o artefato `fceumm_3d_libretro_android` (arm64-v8a, armeabi-v7a, x86_64).

Local (precisa do Android NDK): `cd jni && ndk-build APP_PLATFORM=android-24 APP_ABI=arm64-v8a`
→ `libs/arm64-v8a/libretro.so`.

## Instalar no RetroArch (Android)
- Copie o `.so` (renomeie para `fceumm_3d_libretro_android.so`) para a pasta `cores` do RetroArch
  e o `fceumm_3d_libretro.info` para a pasta `info`. Ou use **Carregar Núcleo → Instalar/Restaurar**.
- FDS: coloque `disksys.rom` na pasta `system`. Troca de lado do disco: menu rápido → *Controle de disco*.
- Driver de vídeo do RetroArch: **gl** (ou glcore). O modo 3D pede contexto OpenGL ES 3;
  se o frontend recusar, o núcleo volta sozinho ao 2D normal.

## Controles do 3D (jogador 1)
| Entrada | Ação |
|---|---|
| Analógico direito | orbitar a câmera |
| L2 / R2 | zoom |
| L3 | próxima predefinição de luz |
| R3 | resetar a câmera |

## Opções (Menu rápido → Opções → Vídeo)
Modo 3D (liga/desliga, requer reiniciar o jogo), resolução 960x540 / 1280x720 / 1920x1080,
palco/mesa, relevo (cor/luminância/plano) e altura, sprites 3D e espessura, iluminação,
sombras e qualidade, câmera orbitando, câmera ortográfica.
Se ficar pesado: resolução 960x540, sombras 1024 ou desligadas.

## O que foi alterado no FCEUmm
- `src/drivers/libretro/r3d.c/.h/_shaders.h` — renderizador (novo)
- `src/ppu.c`, `src/pputile.h` — captura do cenário (valor de 2 bits por pixel + cor sem sprites) e do OAM/paleta ao fim do quadro
- `src/drivers/libretro/libretro.c` — pede contexto GLES3, desvia o vídeo para o renderizador
- `libretro_core_options.h`, `Makefile.common`, `jni/Android.mk` — opções e build (`HAVE_R3D=1`)
- `.github/workflows/android.yml` — compilação automática
