/* Renderizador 3D (voxels + luz + sombras) para o FCEUmm.
 *
 * Porte direto do Famicom_Retro_FDS_v8_render3D.html (WebGL2 -> GLES3):
 * mesma geometria (relevo do cenario por nivel de cor, sprites extrudados,
 * moldura e chao), mesmos shaders e mesmas predefinicoes de luz.
 *
 * Controles (porta 1):
 *   analogico direito .... orbita da camera
 *   L2 / R2 .............. zoom
 *   L3 ................... proxima predefinicao de luz
 *   R3 ................... reseta a camera
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl3.h>

#include "r3d.h"
#include "r3d_shaders.h"

#define W     256
#define H     240
#define NPIX  (W * H)
#define QMAX  250000
#define D2R   0.017453292519943295f

/* ------------------------------------------------------------------ */
/* Capturas (preenchidas pela PPU)                                     */
/* ------------------------------------------------------------------ */
int     r3d_capture = 0;
uint8_t r3d_bgidx[256 * 256 + 64];
uint8_t r3d_bgraw[256 * 240];

static uint32_t pal_rgb[1024];          /* indice de retro_palette -> r | g<<8 | b<<16 */
static uint8_t  snap_oam[256];
static uint8_t  snap_pal[32];
static uint8_t  snap_ctrl, snap_mask;
static unsigned snap_count;

void r3d_set_palette_entry(unsigned index, uint8_t r, uint8_t g, uint8_t b)
{
   if (index < 1024)
      pal_rgb[index] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

void r3d_ppu_snapshot(const uint8_t *oam, const uint8_t *pal, uint8_t ctrl, uint8_t mask)
{
   memcpy(snap_oam, oam, 256);
   memcpy(snap_pal, pal, 32);
   snap_ctrl = ctrl;
   snap_mask = mask ? mask : 0x1E;
   snap_count++;
}

/* ------------------------------------------------------------------ */
/* Configuracoes (opcoes do nucleo)                                    */
/* ------------------------------------------------------------------ */
typedef struct {
   const char *name;
   float az, el, inten, temp, amb, emis;
   float zen[3], hor[3], gnd[3], ambS[3], ambG[3];
} preset_t;

static const preset_t PRESETS[5] = {
   { "dia",     -38, 50, 1.55f, 0.40f, 0.60f, 0.14f,
     {0.16f,0.38f,0.85f}, {0.62f,0.78f,0.98f}, {0.26f,0.28f,0.32f}, {0.50f,0.64f,0.95f}, {0.32f,0.30f,0.28f} },
   { "tarde",   -62, 24, 2.0f,  0.78f, 0.55f, 0.14f,
     {0.20f,0.34f,0.70f}, {1.00f,0.72f,0.46f}, {0.30f,0.22f,0.18f}, {0.55f,0.58f,0.85f}, {0.42f,0.30f,0.20f} },
   { "por",     -84, 9,  2.1f,  1.00f, 0.50f, 0.16f,
     {0.12f,0.14f,0.38f}, {1.00f,0.42f,0.20f}, {0.22f,0.12f,0.12f}, {0.40f,0.36f,0.70f}, {0.45f,0.22f,0.14f} },
   { "noite",    38, 40, 0.85f, 0.00f, 0.50f, 0.38f,
     {0.01f,0.02f,0.07f}, {0.05f,0.08f,0.20f}, {0.02f,0.03f,0.06f}, {0.14f,0.20f,0.45f}, {0.06f,0.07f,0.14f} },
   { "estudio", -30, 42, 1.45f, 0.50f, 0.75f, 0.14f,
     {0.30f,0.30f,0.33f}, {0.42f,0.42f,0.46f}, {0.16f,0.16f,0.18f}, {0.70f,0.70f,0.74f}, {0.38f,0.38f,0.40f} }
};

static struct {
   int   table;          /* 0 = palco vertical, 1 = mesa */
   int   sprites3d;
   int   relief;         /* 0 plano, 1 por cor, 2 por luminancia */
   float bgStep, spDepth, spLift;
   int   ortho, autoOrbit;
   float orbitSpeed;
   int   preset;
   float lightAz, lightEl, sunInt, temp, ambient, exposure;
   int   shadows, shadowRes;
   float soft, rough, spec, emis, pixGrid;
   int   aoOn;
   unsigned res_w, res_h;
} S = {
   0, 1, 1, 1.2f, 4.0f, 0.6f,
   0, 0, 14.0f,
   0, -38, 46, 1.55f, 0.40f, 0.60f, 0.95f,
   1, 1024,
   1.3f, 0.42f, 0.75f, 0.14f, 0.45f,
   1,
   1280, 720
};

typedef struct { float yaw, pitch, dist, fov, tx, ty, tz; } cam_t;
static cam_t cam  = { -26, 30, 430, 36, 0, 0, 0 };
static cam_t camT = { -26, 30, 430, 36, 0, 0, 0 };

static void apply_preset(int p)
{
   if (p < 0 || p > 4) p = 0;
   S.preset   = p;
   S.lightAz  = PRESETS[p].az;
   S.lightEl  = PRESETS[p].el;
   S.sunInt   = PRESETS[p].inten;
   S.temp     = PRESETS[p].temp;
   S.ambient  = PRESETS[p].amb;
   S.emis     = PRESETS[p].emis;
}

static int get_var(retro_environment_t env, const char *key, const char **out)
{
   struct retro_variable var;
   var.key   = key;
   var.value = NULL;
   if (env && env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      *out = var.value;
      return 1;
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* Matematica (matrizes 4x4 column-major)                              */
/* ------------------------------------------------------------------ */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float mixf(float a, float b, float t)   { return a + (b - a) * t; }

static void m_mul(float *o, const float *a, const float *b)
{
   float t[16];
   int i, j;
   for (j = 0; j < 4; j++)
      for (i = 0; i < 4; i++)
         t[j * 4 + i] = a[i] * b[j * 4] + a[4 + i] * b[j * 4 + 1] + a[8 + i] * b[j * 4 + 2] + a[12 + i] * b[j * 4 + 3];
   memcpy(o, t, sizeof(t));
}

static void m_persp(float *o, float fovy, float asp, float n, float f)
{
   float t = 1.0f / tanf(fovy / 2);
   memset(o, 0, 16 * sizeof(float));
   o[0] = t / asp; o[5] = t; o[10] = (f + n) / (n - f); o[11] = -1; o[14] = 2 * f * n / (n - f);
}

static void m_ortho(float *o, float l, float r, float b, float t, float n, float f)
{
   memset(o, 0, 16 * sizeof(float));
   o[0] = 2 / (r - l); o[5] = 2 / (t - b); o[10] = -2 / (f - n);
   o[12] = -(r + l) / (r - l); o[13] = -(t + b) / (t - b); o[14] = -(f + n) / (f - n); o[15] = 1;
}

static void v_sub(float *o, const float *a, const float *b) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void v_cross(float *o, const float *a, const float *b)
{
   float t[3];
   t[0] = a[1] * b[2] - a[2] * b[1];
   t[1] = a[2] * b[0] - a[0] * b[2];
   t[2] = a[0] * b[1] - a[1] * b[0];
   o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
}
static float v_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v_norm(float *o, const float *a)
{
   float l = sqrtf(v_dot(a, a));
   if (l == 0) l = 1;
   o[0] = a[0] / l; o[1] = a[1] / l; o[2] = a[2] / l;
}

static void m_look(float *o, const float *e, const float *c, const float *u)
{
   float z[3], x[3], y[3], d[3];
   v_sub(d, e, c);      v_norm(z, d);
   v_cross(d, u, z);    v_norm(x, d);
   v_cross(y, z, x);
   memset(o, 0, 16 * sizeof(float));
   o[0] = x[0]; o[1] = y[0]; o[2]  = z[0];
   o[4] = x[1]; o[5] = y[1]; o[6]  = z[1];
   o[8] = x[2]; o[9] = y[2]; o[10] = z[2];
   o[12] = -v_dot(x, e); o[13] = -v_dot(y, e); o[14] = -v_dot(z, e); o[15] = 1;
}

static const float MODEL_STAGE[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
/* mesa: mundo.x = x ; mundo.y = z (altura) ; mundo.z = -y */
static const float MODEL_TABLE[16] = { 1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1 };

/* ------------------------------------------------------------------ */
/* Geometria: buffer de quads (20 bytes por vertice)                   */
/* ------------------------------------------------------------------ */
typedef struct { float x, y, z; int8_t n[4]; uint32_t c; } r3d_vtx;

static r3d_vtx *vb;
static unsigned nq;

static void vtx(unsigned v, float x, float y, float z, int nx, int ny, int nz, int m, uint32_t c)
{
   r3d_vtx *p = &vb[v];
   p->x = x; p->y = y; p->z = z;
   p->n[0] = (int8_t)nx; p->n[1] = (int8_t)ny; p->n[2] = (int8_t)nz; p->n[3] = (int8_t)m;
   p->c = c;
}

static void quad(float x0, float y0, float z0, float x1, float y1, float z1,
      float x2, float y2, float z2, float x3, float y3, float z3,
      int nx, int ny, int nz, int m, uint32_t rgb, int a0, int a1, int a2, int a3)
{
   unsigned v;
   uint32_t base;
   if (nq >= QMAX) return;
   v = nq * 4; nq++;
   base = rgb & 0x00FFFFFFu;
   vtx(v,     x0, y0, z0, nx, ny, nz, m, base | ((uint32_t)a0 << 24));
   vtx(v + 1, x1, y1, z1, nx, ny, nz, m, base | ((uint32_t)a1 << 24));
   vtx(v + 2, x2, y2, z2, nx, ny, nz, m, base | ((uint32_t)a2 << 24));
   vtx(v + 3, x3, y3, z3, nx, ny, nz, m, base | ((uint32_t)a3 << 24));
}

static uint32_t rgb_u32(int r, int g, int b) { return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r; }

static const int AO_TAB[4] = { 255, 205, 165, 118 };
static int ao_f(int s1, int s2, int c) { return (s1 && s2) ? 3 : ((s1 ? 1 : 0) + (s2 ? 1 : 0) + (c ? 1 : 0)); }

static uint8_t  lev[NPIX];
static uint32_t grid_sp[8 * 16];
static uint32_t col_comp[NPIX];     /* quadro completo (com sprites) */
static uint32_t col_bg[NPIX];       /* so o cenario */

/* ---------- Cenario (BG) ---------- */
static void build_bg(const uint32_t *col, const uint8_t *idx)
{
   float step = S.bgStep;
   int relief = (S.relief != 0 && step > 0.01f);
   int ao_on, i, x, y;
   int wb;
   int yb;

   if (relief)
   {
      for (i = 0; i < NPIX; i++)
      {
         int p = idx[i] & 3, l = 0;
         if (p)
         {
            if (S.relief == 2)
            {
               uint32_t c = col[i];
               float lum = (0.299f * (float)(c & 255) + 0.587f * (float)((c >> 8) & 255) + 0.114f * (float)((c >> 16) & 255)) / 255.0f;
               l = 1 + (int)floorf(lum * 2 + 0.5f);
            }
            else
               l = p;
         }
         lev[i] = (uint8_t)l;
      }
   }
   else
      memset(lev, 0, sizeof(lev));
   ao_on = (S.aoOn && relief);

   /* ---- faces superiores (com fusao de corridas planas) ---- */
   for (y = 0; y < H; y++)
   {
      int rs = -1;
      uint32_t rc = 0;
      int rl = -1;
      float yT = (float)(120 - y), yB = (float)(119 - y);
      for (x = 0; x < W; x++)
      {
         int l2, a0 = 255, a1 = 255, a2 = 255, a3 = 255, flat = 1;
         uint32_t c2;
         i = y * W + x;
         l2 = lev[i];
         c2 = col[i] & 0xFFFFFFu;
         if (ao_on && l2 < 3)
         {
            int L  = x > 0 && lev[i - 1] > l2;
            int R  = x < 255 && lev[i + 1] > l2;
            int U  = y > 0 && lev[i - 256] > l2;
            int D  = y < 239 && lev[i + 256] > l2;
            int UL = x > 0 && y > 0 && lev[i - 257] > l2;
            int UR = x < 255 && y > 0 && lev[i - 255] > l2;
            int DL = x > 0 && y < 239 && lev[i + 255] > l2;
            int DR = x < 255 && y < 239 && lev[i + 257] > l2;
            if (L || R || U || D || UL || UR || DL || DR)
            {
               flat = 0;
               a0 = AO_TAB[ao_f(L, U, UL)]; a1 = AO_TAB[ao_f(R, U, UR)];
               a2 = AO_TAB[ao_f(R, D, DR)]; a3 = AO_TAB[ao_f(L, D, DL)];
            }
         }
         if (rs >= 0 && !(flat && l2 == rl && c2 == rc))
         {
            float zz = (float)rl * step;
            quad((float)(rs - 128), yT, zz, (float)(x - 128), yT, zz, (float)(x - 128), yB, zz, (float)(rs - 128), yB, zz,
                  0, 0, 1, 0, rc, 255, 255, 255, 255);
            rs = -1;
         }
         if (flat)
         {
            if (rs < 0) { rs = x; rc = c2; rl = l2; }
         }
         else
         {
            float z3 = (float)l2 * step;
            quad((float)(x - 128), yT, z3, (float)(x - 127), yT, z3, (float)(x - 127), yB, z3, (float)(x - 128), yB, z3,
                  0, 0, 1, 0, c2, a0, a1, a2, a3);
         }
      }
      if (rs >= 0)
      {
         float zr = (float)rl * step;
         quad((float)(rs - 128), yT, zr, 128.0f, yT, zr, 128.0f, yB, zr, (float)(rs - 128), yB, zr,
               0, 0, 1, 0, rc, 255, 255, 255, 255);
      }
   }
   if (!relief) return;

   /* ---- paredes entre colunas de alturas diferentes ---- */
   wb = ao_on ? 175 : 255;
   /* fronteiras verticais (entre x-1 e x): normal +-X */
   for (y = 0; y < H; y++)
   {
      float yT2 = (float)(120 - y), yB2 = (float)(119 - y);
      for (x = 0; x <= W; x++)
      {
         int lL = x > 0 ? lev[y * W + x - 1] : 0;
         int lR = x < W ? lev[y * W + x] : 0;
         float xx, zl, zh;
         uint32_t cc;
         if (lL == lR) continue;
         xx = (float)(x - 128);
         if (lL > lR)
         {
            cc = col[y * W + x - 1] & 0xFFFFFFu; zl = (float)lR * step; zh = (float)lL * step;
            quad(xx, yT2, zh, xx, yB2, zh, xx, yB2, zl, xx, yT2, zl, 1, 0, 0, 0, cc, 255, 255, wb, wb);
         }
         else
         {
            cc = col[y * W + x] & 0xFFFFFFu; zl = (float)lL * step; zh = (float)lR * step;
            quad(xx, yB2, zh, xx, yT2, zh, xx, yT2, zl, xx, yB2, zl, -1, 0, 0, 0, cc, 255, 255, wb, wb);
         }
      }
   }
   /* fronteiras horizontais (entre y-1 e y): normal +-Y, com fusao ao longo de x */
   for (yb = 0; yb <= H; yb++)
   {
      float yy = (float)(120 - yb);
      int rs2 = -1, rdir = 0;
      uint32_t rcol = 0;
      float rlo = 0, rhi = 0;
      for (x = 0; x <= W; x++)
      {
         int dir = 0;
         uint32_t wc = 0;
         float wlo = 0, whi = 0;
         if (x < W)
         {
            int lU = yb > 0 ? lev[(yb - 1) * W + x] : 0;
            int lD = yb < H ? lev[yb * W + x] : 0;
            if (lU > lD)      { dir = -1; wc = col[(yb - 1) * W + x] & 0xFFFFFFu; wlo = (float)lD * step; whi = (float)lU * step; }
            else if (lD > lU) { dir = 1;  wc = col[yb * W + x] & 0xFFFFFFu;       wlo = (float)lU * step; whi = (float)lD * step; }
         }
         if (rs2 >= 0 && !(dir == rdir && wc == rcol && wlo == rlo && whi == rhi))
         {
            if (rdir == -1)
               quad((float)(rs2 - 128), yy, rhi, (float)(rs2 - 128), yy, rlo, (float)(x - 128), yy, rlo, (float)(x - 128), yy, rhi,
                     0, -1, 0, 0, rcol, 255, wb, wb, 255);
            else
               quad((float)(rs2 - 128), yy, rlo, (float)(rs2 - 128), yy, rhi, (float)(x - 128), yy, rhi, (float)(x - 128), yy, rlo,
                     0, 1, 0, 0, rcol, wb, 255, 255, wb);
            rs2 = -1;
         }
         if (dir != 0 && rs2 < 0) { rs2 = x; rdir = dir; rcol = wc; rlo = wlo; rhi = whi; }
      }
   }
}

/* ---------- Sprites ---------- */
static int read_sprite_pixels(int n, int behind)
{
   int big = (snap_ctrl & 0x20) != 0, h = big ? 16 : 8;
   int y    = snap_oam[n * 4],     tile = snap_oam[n * 4 + 1];
   int attr = snap_oam[n * 4 + 2], x    = snap_oam[n * 4 + 3];
   int flipH = (attr & 0x40) != 0, flipV = (attr & 0x80) != 0;
   int any = 0, r, px;
   int bank = ((snap_mask >> 5) == 7) ? 0xC0 : ((snap_mask & 0xE0) ? 0x40 : 0x80);

   for (r = 0; r < h; r++)
   {
      int row = flipV ? h - 1 - r : r, addr, lo, hi, sy;
      if (!big)
         addr = ((snap_ctrl & 8) ? 0x1000 : 0) + tile * 16 + row;
      else
         addr = ((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16 + (row & 7) + (row >= 8 ? 16 : 0);
      lo = FCEUPPU_R3DRead((unsigned)addr);
      hi = FCEUPPU_R3DRead((unsigned)(addr + 8));
      sy = y + 1 + r;
      for (px = 0; px < 8; px++)
      {
         int bit = flipH ? px : 7 - px;
         int c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
         uint32_t out = 0;
         if (c)
         {
            int sx = x + px;
            if (sx < 256 && sy < 240 && !((snap_mask & 4) == 0 && sx < 8))
            {
               if (!(behind && (r3d_bgidx[sy * 256 + sx] & 3)))
               {
                  int ci = snap_pal[0x10 | ((attr & 3) << 2) | c] & 0x3F;
                  if (snap_mask & 1) ci &= 0x30;
                  out = pal_rgb[bank | ci] | 0xFF000000u;
                  any = 1;
               }
            }
         }
         grid_sp[r * 8 + px] = out;
      }
   }
   return any ? h : 0;
}

static void emit_sprite(int x, int y, int h, float zb, float zt, int bottom_ao)
{
   int r, px;
   int s_ao = S.aoOn ? bottom_ao : 255;
   for (r = 0; r < h; r++)
   {
      float my = (float)(120 - (y + 1 + r));
      px = 0;
      while (px < 8)
      {
         uint32_t c = grid_sp[r * 8 + px];
         int e;
         float xa, xb;
         if (!c) { px++; continue; }
         e = px + 1;
         while (e < 8 && grid_sp[r * 8 + e] == c) e++;
         xa = (float)(x + px - 128); xb = (float)(x + e - 128);
         quad(xa, my, zt, xb, my, zt, xb, my - 1, zt, xa, my - 1, zt, 0, 0, 1, 1, c, 255, 255, 255, 255);
         quad(xa, my - 1, zb, xb, my - 1, zb, xb, my, zb, xa, my, zb, 0, 0, -1, 1, c, 150, 150, 150, 150);
         px = e;
      }
      for (px = 0; px < 8; px++)
      {
         uint32_t c = grid_sp[r * 8 + px];
         float mx0;
         if (!c) continue;
         mx0 = (float)(x + px - 128);
         if (px == 0 || !grid_sp[r * 8 + px - 1])
            quad(mx0, my, zt, mx0, my - 1, zt, mx0, my - 1, zb, mx0, my, zb, -1, 0, 0, 1, c, 255, 255, s_ao, s_ao);
         if (px == 7 || !grid_sp[r * 8 + px + 1])
            quad(mx0 + 1, my - 1, zt, mx0 + 1, my, zt, mx0 + 1, my, zb, mx0 + 1, my - 1, zb, 1, 0, 0, 1, c, 255, 255, s_ao, s_ao);
         if (r == 0 || !grid_sp[(r - 1) * 8 + px])
            quad(mx0, my, zb, mx0 + 1, my, zb, mx0 + 1, my, zt, mx0, my, zt, 0, 1, 0, 1, c, s_ao, s_ao, 255, 255);
         if (r == h - 1 || !grid_sp[(r + 1) * 8 + px])
            quad(mx0 + 1, my - 1, zb, mx0, my - 1, zb, mx0, my - 1, zt, mx0 + 1, my - 1, zt, 0, -1, 0, 1, c, s_ao, s_ao, 255, 255);
      }
   }
}

static void build_sprites(void)
{
   int n;
   int relief = (S.relief != 0 && S.bgStep > 0.01f);
   float top = relief ? 3 * S.bgStep : 0;
   float depth = S.spDepth, lift = S.spLift;

   if (!(snap_mask & 0x10)) return;                 /* sprites desligados no PPUMASK */
   for (n = 63; n >= 0; n--)
   {
      int y = snap_oam[n * 4];
      int behind, h;
      float zb, zt, lim;
      if (y >= 0xEF) continue;
      behind = (snap_oam[n * 4 + 2] & 0x20) != 0;
      h = read_sprite_pixels(n, behind);
      if (!h) continue;
      if (behind)
      {
         zb = 0;
         lim = 1.0f + (relief ? S.bgStep * 0.5f : 0);
         zt = depth < lim ? depth : lim;
      }
      else
      {
         zb = top + lift;
         zt = zb + depth + (float)(64 - n) * 0.012f;
      }
      emit_sprite(snap_oam[n * 4 + 3], y, h, zb, zt, 170);
   }
}

/* ---------- Base estatica (moldura + chao) ---------- */
static void build_static_quads(void)
{
   float B = 7, T = 12, x0 = -128 - B, x1 = 128 + B, y0 = -120 - B, y1 = 120 + B;
   uint32_t fr = rgb_u32(54, 58, 74), wl = rgb_u32(30, 33, 44), gr = rgb_u32(34, 36, 46);
   float G = 2600, gz = -T - 0.05f;

   quad(x0, y1, 0, -128, y1, 0, -128, y0, 0, x0, y0, 0, 0, 0, 1, 2, fr, 255, 255, 255, 255);
   quad(128, y1, 0, x1, y1, 0, x1, y0, 0, 128, y0, 0, 0, 0, 1, 2, fr, 255, 255, 255, 255);
   quad(-128, y1, 0, 128, y1, 0, 128, 120, 0, -128, 120, 0, 0, 0, 1, 2, fr, 255, 255, 255, 255);
   quad(-128, -120, 0, 128, -120, 0, 128, y0, 0, -128, y0, 0, 0, 0, 1, 2, fr, 255, 255, 255, 255);
   quad(x0, y1, 0, x0, y0, 0, x0, y0, -T, x0, y1, -T, -1, 0, 0, 2, wl, 255, 255, 150, 150);
   quad(x1, y0, 0, x1, y1, 0, x1, y1, -T, x1, y0, -T, 1, 0, 0, 2, wl, 255, 255, 150, 150);
   quad(x1, y1, 0, x0, y1, 0, x0, y1, -T, x1, y1, -T, 0, 1, 0, 2, wl, 255, 255, 150, 150);
   quad(x0, y0, 0, x1, y0, 0, x1, y0, -T, x0, y0, -T, 0, -1, 0, 2, wl, 255, 255, 150, 150);
   quad(-G, G, gz, G, G, gz, G, -G, gz, -G, -G, gz, 0, 0, 1, 3, gr, 255, 255, 255, 255);
}

/* Constroi a cena dinamica a partir das capturas. Exportada para testes. */
unsigned r3d_build_scene(const uint8_t *xbuf)
{
   int i, flat;
   for (i = 0; i < NPIX; i++)
   {
      col_comp[i] = pal_rgb[xbuf[i]];
      col_bg[i]   = pal_rgb[r3d_bgraw[i]];
   }
   flat = (!S.sprites3d || !snap_count);
   nq = 0;
   build_bg(flat ? col_comp : col_bg, r3d_bgidx);
   if (!flat) build_sprites();
   return nq;
}

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */
static struct retro_hw_render_callback hw;
static int hw_active;
static int gl_ok;

static GLuint prog_main, prog_shadow, prog_sky;
static GLuint vao_dyn, vao_static, vao_empty;
static GLuint vbo_dyn, vbo_static, ibo;
static GLuint sh_tex, sh_fbo;
static int    sh_res;
static unsigned dyn_cap;           /* capacidade do VBO dinamico, em vertices */
static unsigned dyn_quads, static_quads;

static GLuint compile_shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok = 0;
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { glDeleteShader(s); return 0; }
   return s;
}

static GLuint make_prog(const char *vs, const char *fs)
{
   GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
   GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
   GLuint p;
   GLint ok = 0;
   if (!v || !f) return 0;
   p = glCreateProgram();
   glAttachShader(p, v);
   glAttachShader(p, f);
   glLinkProgram(p);
   glDeleteShader(v);
   glDeleteShader(f);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) { glDeleteProgram(p); return 0; }
   return p;
}

static void make_shadow_tex(int res)
{
   if (sh_tex) glDeleteTextures(1, &sh_tex);
   if (sh_fbo) glDeleteFramebuffers(1, &sh_fbo);
   glGenTextures(1, &sh_tex);
   glBindTexture(GL_TEXTURE_2D, sh_tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, res, res, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
   glGenFramebuffers(1, &sh_fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sh_tex, 0);
   {
      GLenum none = GL_NONE;
      glDrawBuffers(1, &none);
      glReadBuffer(GL_NONE);
   }
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);
   sh_res = res;
}

static void setup_vao(GLuint vao, GLuint vbo)
{
   glBindVertexArray(vao);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (const void*)0);
   glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_BYTE, GL_FALSE, 20, (const void*)12);
   glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, (const void*)16);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
   glBindVertexArray(0);
}

static void gl_destroy(void)
{
   if (prog_main)   glDeleteProgram(prog_main);
   if (prog_shadow) glDeleteProgram(prog_shadow);
   if (prog_sky)    glDeleteProgram(prog_sky);
   if (vao_dyn)     glDeleteVertexArrays(1, &vao_dyn);
   if (vao_static)  glDeleteVertexArrays(1, &vao_static);
   if (vao_empty)   glDeleteVertexArrays(1, &vao_empty);
   if (vbo_dyn)     glDeleteBuffers(1, &vbo_dyn);
   if (vbo_static)  glDeleteBuffers(1, &vbo_static);
   if (ibo)         glDeleteBuffers(1, &ibo);
   if (sh_tex)      glDeleteTextures(1, &sh_tex);
   if (sh_fbo)      glDeleteFramebuffers(1, &sh_fbo);
   prog_main = prog_shadow = prog_sky = 0;
   vao_dyn = vao_static = vao_empty = 0;
   vbo_dyn = vbo_static = ibo = 0;
   sh_tex = sh_fbo = 0;
   sh_res = 0;
   gl_ok = 0;
}

static void gl_init(void)
{
   uint32_t *idx;
   unsigned q;

   prog_main   = make_prog(r3d_vs_main,   r3d_fs_main);
   prog_shadow = make_prog(r3d_vs_shadow, r3d_fs_shadow);
   prog_sky    = make_prog(r3d_vs_sky,    r3d_fs_sky);
   if (!prog_main || !prog_shadow || !prog_sky)
   {
      gl_destroy();
      return;
   }

   idx = (uint32_t*)malloc((size_t)QMAX * 6 * sizeof(uint32_t));
   if (!idx) { gl_destroy(); return; }
   for (q = 0; q < QMAX; q++)
   {
      uint32_t b = q * 4, o = q * 6;
      idx[o] = b; idx[o + 1] = b + 1; idx[o + 2] = b + 2;
      idx[o + 3] = b; idx[o + 4] = b + 2; idx[o + 5] = b + 3;
   }
   glGenBuffers(1, &ibo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)QMAX * 6 * sizeof(uint32_t)), idx, GL_STATIC_DRAW);
   free(idx);

   dyn_cap = 420000;
   glGenBuffers(1, &vbo_dyn);
   glBindBuffer(GL_ARRAY_BUFFER, vbo_dyn);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)dyn_cap * 20, NULL, GL_DYNAMIC_DRAW);
   glGenVertexArrays(1, &vao_dyn);
   setup_vao(vao_dyn, vbo_dyn);

   nq = 0;
   build_static_quads();
   static_quads = nq;
   glGenBuffers(1, &vbo_static);
   glBindBuffer(GL_ARRAY_BUFFER, vbo_static);
   glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nq * 80, vb, GL_STATIC_DRAW);
   glGenVertexArrays(1, &vao_static);
   setup_vao(vao_static, vbo_static);
   nq = 0;

   glGenVertexArrays(1, &vao_empty);
   make_shadow_tex(S.shadowRes);
   dyn_quads = 0;
   gl_ok = 1;
}

static void ctx_reset(void)
{
   gl_destroy();
   gl_init();
}

static void ctx_destroy(void)
{
   gl_destroy();
}

/* ------------------------------------------------------------------ */
/* Luz / camera                                                        */
/* ------------------------------------------------------------------ */
static void sun_color(float *c)
{
   float t = S.temp;
   if (t < 0.5f)
   {
      float u = t / 0.5f;
      c[0] = mixf(0.78f, 1.0f, u); c[1] = mixf(0.86f, 0.97f, u); c[2] = mixf(1.0f, 0.92f, u);
   }
   else
   {
      float v = (t - 0.5f) / 0.5f;
      c[0] = 1.0f; c[1] = mixf(0.97f, 0.66f, v); c[2] = mixf(0.92f, 0.38f, v);
   }
   c[0] *= S.sunInt; c[1] *= S.sunInt; c[2] *= S.sunInt;
}

static void light_dir(float *L)
{
   float az = S.lightAz * D2R, el = S.lightEl * D2R;
   L[0] = cosf(el) * sinf(az);
   L[1] = sinf(el);
   L[2] = cosf(el) * cosf(az);
}

typedef struct { float eye[3], tgt[3], fwd[3], right[3], up[3]; } camv_t;

static void cam_vectors(camv_t *cv)
{
   float y = cam.yaw * D2R, p = cam.pitch * D2R;
   float off[3], d[3], upv[3];
   off[0] = sinf(y) * cosf(p); off[1] = sinf(p); off[2] = cosf(y) * cosf(p);
   cv->tgt[0] = cam.tx; cv->tgt[1] = cam.ty; cv->tgt[2] = cam.tz;
   cv->eye[0] = cv->tgt[0] + off[0] * cam.dist;
   cv->eye[1] = cv->tgt[1] + off[1] * cam.dist;
   cv->eye[2] = cv->tgt[2] + off[2] * cam.dist;
   v_sub(d, cv->tgt, cv->eye);
   v_norm(cv->fwd, d);
   upv[0] = 0; upv[1] = 1; upv[2] = 0;
   v_cross(d, cv->fwd, upv);
   v_norm(cv->right, d);
   v_cross(cv->up, cv->right, cv->fwd);
}

static void cam_reset(void)
{
   cam.yaw = camT.yaw = -26; cam.pitch = camT.pitch = 30;
   cam.dist = camT.dist = 430; cam.fov = camT.fov = 36;
   cam.tx = camT.tx = cam.ty = camT.ty = cam.tz = camT.tz = 0;
}

#define UNI(p, n) glGetUniformLocation((p), (n))

static void draw_frame(float dt)
{
   unsigned cw = S.res_w, ch = S.res_h;
   float aspect = (float)cw / (float)ch;
   float k = 1.0f - expf(-dt * 14.0f);
   camv_t cv;
   float fovy, tanY, sky_tan[2], proj[16], view[16], vp[16], upv[3], zero[3];
   float L[3], eyeL[3], upL[3], lp[16], lv[16], lvp[16], sunC[4];
   const float *model;
   const preset_t *P = &PRESETS[S.preset];
   int shadow_on, tm = 1;
   GLuint fbo;

   cam.yaw   += (camT.yaw   - cam.yaw)   * k;
   cam.pitch += (camT.pitch - cam.pitch) * k;
   cam.dist  += (camT.dist  - cam.dist)  * k;
   cam.fov   += (camT.fov   - cam.fov)   * k;
   if (S.autoOrbit) { camT.yaw += S.orbitSpeed * dt; cam.yaw += S.orbitSpeed * dt; }

   cam_vectors(&cv);
   fovy = clampf(cam.fov, 8, 100) * D2R;
   tanY = tanf(fovy / 2);
   if (S.ortho)
   {
      float hh = cam.dist * tanY, hw = hh * aspect;
      m_ortho(proj, -hw, hw, -hh, hh, -3000, cam.dist + 6000);
      sky_tan[0] = 0.45f * aspect; sky_tan[1] = 0.45f;
   }
   else
   {
      float nr = cam.dist * 0.08f, fr = cam.dist * 8 + 6000;
      if (nr < 2) nr = 2;
      m_persp(proj, fovy, aspect, nr, fr);
      sky_tan[0] = tanY * aspect; sky_tan[1] = tanY;
   }
   upv[0] = 0; upv[1] = 1; upv[2] = 0;
   m_look(view, cv.eye, cv.tgt, upv);
   m_mul(vp, proj, view);

   light_dir(L);
   eyeL[0] = L[0] * 450; eyeL[1] = L[1] * 450; eyeL[2] = L[2] * 450;
   if (fabsf(L[1]) > 0.98f) { upL[0] = 0; upL[1] = 0; upL[2] = -1; }
   else                     { upL[0] = 0; upL[1] = 1; upL[2] = 0; }
   zero[0] = zero[1] = zero[2] = 0;
   m_ortho(lp, -232, 232, -232, 232, 1, 900);
   m_look(lv, eyeL, zero, upL);
   m_mul(lvp, lp, lv);
   model = S.table ? MODEL_TABLE : MODEL_STAGE;
   sun_color(sunC);

   fbo = (GLuint)hw.get_current_framebuffer();
   shadow_on = (S.shadows && dyn_quads > 0);
   if (S.shadows && sh_res != S.shadowRes) make_shadow_tex(S.shadowRes);

   /* ----- sombra ----- */
   if (shadow_on)
   {
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo);
      glViewport(0, 0, sh_res, sh_res);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
      glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
      glClear(GL_DEPTH_BUFFER_BIT);
      glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(1.5f, 3.0f);
      glUseProgram(prog_shadow);
      glUniformMatrix4fv(UNI(prog_shadow, "u_lvp"), 1, GL_FALSE, lvp);
      glUniformMatrix4fv(UNI(prog_shadow, "u_model"), 1, GL_FALSE, model);
      glBindVertexArray(vao_dyn);
      glDrawElements(GL_TRIANGLES, (GLsizei)(dyn_quads * 6), GL_UNSIGNED_INT, 0);
      glDisable(GL_POLYGON_OFFSET_FILL);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   }

   /* ----- cena ----- */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glViewport(0, 0, (GLsizei)cw, (GLsizei)ch);
   glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   glDepthMask(GL_TRUE);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   /* ceu */
   glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
   glUseProgram(prog_sky);
   glUniform3fv(UNI(prog_sky, "u_fwd"), 1, cv.fwd);
   glUniform3fv(UNI(prog_sky, "u_right"), 1, cv.right);
   glUniform3fv(UNI(prog_sky, "u_up"), 1, cv.up);
   glUniform2fv(UNI(prog_sky, "u_tan"), 1, sky_tan);
   glUniform3fv(UNI(prog_sky, "u_skyZen"), 1, P->zen);
   glUniform3fv(UNI(prog_sky, "u_skyHor"), 1, P->hor);
   glUniform3fv(UNI(prog_sky, "u_skyGnd"), 1, P->gnd);
   glUniform3fv(UNI(prog_sky, "u_L"), 1, L);
   glUniform3fv(UNI(prog_sky, "u_sunCol"), 1, sunC);
   glUniform1f(UNI(prog_sky, "u_exposure"), S.exposure);
   glUniform1f(UNI(prog_sky, "u_tm"), (float)tm);
   glBindVertexArray(vao_empty);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL);

   /* geometria */
   glUseProgram(prog_main);
   glUniformMatrix4fv(UNI(prog_main, "u_vp"), 1, GL_FALSE, vp);
   glUniformMatrix4fv(UNI(prog_main, "u_model"), 1, GL_FALSE, model);
   glUniformMatrix4fv(UNI(prog_main, "u_lvp"), 1, GL_FALSE, lvp);
   glUniform3fv(UNI(prog_main, "u_cam"), 1, cv.eye);
   glUniform3fv(UNI(prog_main, "u_L"), 1, L);
   glUniform3fv(UNI(prog_main, "u_sunCol"), 1, sunC);
   glUniform3fv(UNI(prog_main, "u_skyZen"), 1, P->zen);
   glUniform3fv(UNI(prog_main, "u_skyHor"), 1, P->hor);
   glUniform3fv(UNI(prog_main, "u_skyGnd"), 1, P->gnd);
   glUniform3f(UNI(prog_main, "u_ambS"), P->ambS[0] * S.ambient, P->ambS[1] * S.ambient, P->ambS[2] * S.ambient);
   glUniform3f(UNI(prog_main, "u_ambG"), P->ambG[0] * S.ambient, P->ambG[1] * S.ambient, P->ambG[2] * S.ambient);
   glUniform1f(UNI(prog_main, "u_exposure"), S.exposure);
   glUniform1f(UNI(prog_main, "u_tm"), (float)tm);
   glUniform1f(UNI(prog_main, "u_shadowOn"), shadow_on ? 1.0f : 0.0f);
   glUniform1f(UNI(prog_main, "u_texel"), 1.0f / (float)(sh_res ? sh_res : 1));
   glUniform1f(UNI(prog_main, "u_soft"), S.soft);
   glUniform1f(UNI(prog_main, "u_nbias"), 0.30f);
   glUniform1f(UNI(prog_main, "u_lrange"), 899.0f);
   glUniform1f(UNI(prog_main, "u_rough"), S.rough);
   glUniform1f(UNI(prog_main, "u_spec"), S.spec);
   glUniform1f(UNI(prog_main, "u_emis"), S.emis);
   glUniform1f(UNI(prog_main, "u_pixGrid"), S.pixGrid);
   glUniform1f(UNI(prog_main, "u_aoOn"), S.aoOn ? 1.0f : 0.0f);
   glUniform1f(UNI(prog_main, "u_fog"), 0.0012f);
   glUniform1f(UNI(prog_main, "u_plOn"), 0.0f);
   glUniform3fv(UNI(prog_main, "u_plPos"), 1, zero);
   glUniform3f(UNI(prog_main, "u_plCol"), 1.0f, 0.78f, 0.52f);
   glUniform1f(UNI(prog_main, "u_plRad"), 110.0f);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, sh_tex);
   glUniform1i(UNI(prog_main, "u_shadow"), 0);

   glBindVertexArray(vao_static);
   glDrawElements(GL_TRIANGLES, (GLsizei)(static_quads * 6), GL_UNSIGNED_INT, 0);
   if (dyn_quads > 0)
   {
      glBindVertexArray(vao_dyn);
      glDrawElements(GL_TRIANGLES, (GLsizei)(dyn_quads * 6), GL_UNSIGNED_INT, 0);
   }

   /* devolve o estado ao frontend */
   glBindVertexArray(0);
   glBindTexture(GL_TEXTURE_2D, 0);
   glUseProgram(0);
   glDisable(GL_DEPTH_TEST);
   glDepthMask(GL_TRUE);
}

/* ------------------------------------------------------------------ */
/* Entrada                                                             */
/* ------------------------------------------------------------------ */
static float stick(int16_t v)
{
   float f = (float)v / 32767.0f;
   if (f > -0.15f && f < 0.15f) return 0;
   return f;
}

static void poll_camera(retro_input_state_t icb, float dt)
{
   static int l3_prev, r3_prev;
   int l3, r3;
   float sx, sy;

   if (!icb) return;
   sx = stick(icb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
   sy = stick(icb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
   if (sx != 0 || sy != 0)
   {
      camT.yaw   += sx * 110.0f * dt;
      camT.pitch  = clampf(camT.pitch - sy * 70.0f * dt, -5.0f, 88.0f);
   }
   if (icb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
      camT.dist = clampf(camT.dist * (1.0f - 0.9f * dt), 90.0f, 1400.0f);
   if (icb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
      camT.dist = clampf(camT.dist * (1.0f + 0.9f * dt), 90.0f, 1400.0f);

   l3 = icb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) ? 1 : 0;
   r3 = icb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3) ? 1 : 0;
   if (l3 && !l3_prev) apply_preset((S.preset + 1) % 5);
   if (r3 && !r3_prev) cam_reset();
   l3_prev = l3; r3_prev = r3;
}

/* ------------------------------------------------------------------ */
/* Interface com o libretro.c                                          */
/* ------------------------------------------------------------------ */
int r3d_wants_hw(retro_environment_t env)
{
   const char *v;
   if (get_var(env, "fceumm_3d_mode", &v))
      return strcmp(v, "enabled") == 0;
   return 1;   /* padrao: ligado */
}

int r3d_init_hw(retro_environment_t env)
{
   if (!vb)
   {
      vb = (r3d_vtx*)malloc((size_t)QMAX * 4 * sizeof(r3d_vtx));
      if (!vb) return 0;
   }
   r3d_check_options(env);

   memset(&hw, 0, sizeof(hw));
   hw.context_type    = RETRO_HW_CONTEXT_OPENGLES3;
   hw.version_major   = 3;
   hw.version_minor   = 0;
   hw.context_reset   = ctx_reset;
   hw.context_destroy = ctx_destroy;
   hw.depth           = true;
   hw.stencil         = false;
   hw.bottom_left_origin = true;
   hw.cache_context   = false;

   if (!env(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw))
   {
      hw_active = 0;
      r3d_capture = 0;
      return 0;
   }
   hw_active = 1;
   r3d_capture = 1;
   return 1;
}

int r3d_active(void)
{
   return hw_active;
}

void r3d_shutdown(void)
{
   r3d_capture = 0;
   hw_active   = 0;
   if (vb) { free(vb); vb = NULL; }
}

void r3d_get_geometry(unsigned *base_w, unsigned *base_h, unsigned *max_w, unsigned *max_h)
{
   *base_w = S.res_w; *base_h = S.res_h;
   *max_w  = 1920;    *max_h  = 1080;
}

void r3d_check_options(retro_environment_t env)
{
   const char *v;
   unsigned old_w = S.res_w, old_h = S.res_h;
   int old_preset = S.preset;
   int p;

   if (get_var(env, "fceumm_3d_resolution", &v))
   {
      if (!strcmp(v, "960x540"))        { S.res_w = 960;  S.res_h = 540; }
      else if (!strcmp(v, "1920x1080")) { S.res_w = 1920; S.res_h = 1080; }
      else                              { S.res_w = 1280; S.res_h = 720; }
   }
   if (get_var(env, "fceumm_3d_orient", &v))        S.table = !strcmp(v, "table");
   if (get_var(env, "fceumm_3d_sprites", &v))       S.sprites3d = !strcmp(v, "enabled");
   if (get_var(env, "fceumm_3d_relief", &v))        S.relief = !strcmp(v, "flat") ? 0 : (!strcmp(v, "luma") ? 2 : 1);
   if (get_var(env, "fceumm_3d_relief_height", &v)) S.bgStep = (float)atof(v);
   if (get_var(env, "fceumm_3d_sprite_depth", &v))  S.spDepth = (float)atof(v);
   if (get_var(env, "fceumm_3d_shadows", &v))       S.shadows = !strcmp(v, "enabled");
   if (get_var(env, "fceumm_3d_shadow_res", &v))    S.shadowRes = !strcmp(v, "2048") ? 2048 : 1024;
   if (get_var(env, "fceumm_3d_autoorbit", &v))     S.autoOrbit = !strcmp(v, "enabled");
   if (get_var(env, "fceumm_3d_ortho", &v))         S.ortho = !strcmp(v, "enabled");
   if (get_var(env, "fceumm_3d_light", &v))
   {
      p = 0;
      if (!strcmp(v, "tarde")) p = 1;
      else if (!strcmp(v, "por")) p = 2;
      else if (!strcmp(v, "noite")) p = 3;
      else if (!strcmp(v, "estudio")) p = 4;
      /* so reaplica quando a opcao muda (L3 pode ter trocado a luz) */
      {
         static int last_opt = -1;
         if (p != last_opt) { last_opt = p; apply_preset(p); }
      }
   }
   (void)old_preset;

   if (hw_active && (old_w != S.res_w || old_h != S.res_h))
   {
      struct retro_game_geometry g;
      g.base_width   = S.res_w;
      g.base_height  = S.res_h;
      g.max_width    = 1920;
      g.max_height   = 1080;
      g.aspect_ratio = (float)S.res_w / (float)S.res_h;
      env(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
   }
}

void r3d_run_frame(const uint8_t *xbuf, retro_video_refresh_t vcb,
      retro_input_state_t icb, retro_input_poll_t pcb, retro_environment_t env)
{
   const float dt = 1.0f / 60.0f;
   (void)pcb; (void)env;

   if (!gl_ok || !xbuf)
   {
      vcb(NULL, S.res_w, S.res_h, 0);     /* repete o quadro anterior */
      return;
   }

   poll_camera(icb, dt);

   /* reconstroi a cena e envia ao VBO dinamico */
   nq = 0;
   dyn_quads = r3d_build_scene(xbuf);
   glBindBuffer(GL_ARRAY_BUFFER, vbo_dyn);
   if (dyn_quads * 4 > dyn_cap)
   {
      dyn_cap = (unsigned)((float)dyn_quads * 4 * 1.25f);
      glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)dyn_cap * 20, NULL, GL_DYNAMIC_DRAW);
   }
   glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)dyn_quads * 80, vb);
   glBindBuffer(GL_ARRAY_BUFFER, 0);

   draw_frame(dt);
   vcb(RETRO_HW_FRAME_BUFFER_VALID, S.res_w, S.res_h, 0);
}
