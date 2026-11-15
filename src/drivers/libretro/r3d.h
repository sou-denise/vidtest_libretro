/* Renderizador 3D (voxels + luz + sombras) para o FCEUmm.
 * Portado do Famicom_Retro_FDS_v8_render3D.html (WebGL2) para GLES3. */
#ifndef R3D_H
#define R3D_H

#include <stdint.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- capturas feitas pela PPU (ppu.c) ---- */
extern int      r3d_capture;                 /* 1 = modo 3D ativo */
extern uint8_t  r3d_bgidx[256 * 256 + 64];   /* por pixel: pixel(2 bits) | paleta<<2, indexado por (P - XBuf) */
extern uint8_t  r3d_bgraw[256 * 240];        /* cor final (indice de retro_palette) so do cenario */

void r3d_ppu_snapshot(const uint8_t *oam, const uint8_t *pal, uint8_t ctrl, uint8_t mask);
void r3d_set_palette_entry(unsigned index, uint8_t r, uint8_t g, uint8_t b);

/* leitura de CHR (implementada em ppu.c) */
uint8_t FCEUPPU_R3DRead(unsigned addr);

/* ---- interface com o libretro.c ---- */
int  r3d_wants_hw(retro_environment_t env);       /* opcao do usuario liga o modo 3D? */
int  r3d_init_hw(retro_environment_t env);        /* pede contexto GLES3 ao frontend */
int  r3d_active(void);
void r3d_shutdown(void);
void r3d_check_options(retro_environment_t env);  /* le as opcoes do nucleo */
void r3d_get_geometry(unsigned *base_w, unsigned *base_h, unsigned *max_w, unsigned *max_h);
void r3d_run_frame(const uint8_t *xbuf, retro_video_refresh_t vcb,
      retro_input_state_t icb, retro_input_poll_t pcb, retro_environment_t env);

#ifdef __cplusplus
}
#endif

#endif
