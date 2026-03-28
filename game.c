#include <atari.h>
#include <peekpoke.h>
#include <string.h>

typedef unsigned char byte;
typedef unsigned int  word;

/* ---- PAMIĘĆ ---- */
#define PMG0_BASE    0x4000
#define PMG1_BASE    0x4800
#define SCREEN_ADDR  0x5000

/* FONT dla HUD (1KB, wyrównany) */
#define HUD_FONT_ADDR 0x3800  /* 1KB: 0x3800..0x3BFF */

/* ---- Playfield ---- */
#define STRIDE        64
#define VIS_LINES    192
#define MAX_SX_PIX   ((64 - 40) * 4)
#define FIXED_COARSE_Y  32

/* HUD (ANTIC 2, 40 kolumn) */
#define HUD_COLS     40
#define HUD_POS      36     /* 3 cyfry w kolumnach 36..38 */
#define HUD_BLANK    10     /* kod znaku "pusty" w naszym foncie */

/* HW Registers */
#define REG_DMACTL   0xD400
#define REG_DLISTL   0xD402
#define REG_DLISTH   0xD403
#define REG_HSCROL   0xD404
#define REG_PMBAS    0xD407
#define REG_CHBASE   0xD409
#define REG_VCOUNT   0xD40B
#define REG_NMIEN    0xD40E
#define REG_PORTA    0xD300
#define REG_GRACTL   0xD01D
#define REG_TRIG0    0xD010
#define REG_PRIOR    0xD01B

/* Playfield colors */
#define COLPF0       0xD016
#define COLPF1       0xD017
#define COLPF2       0xD018
#define COLPF3       0xD019
#define COLBK        0xD01A

/* GTIA PMG */
#define HPOSP0       0xD000
#define HPOSP1       0xD001
#define HPOSP2       0xD002
#define HPOSP3       0xD003
#define HPOSM0       0xD004
#define SIZEM        0xD00C
#define SIZEP3       0xD00B
#define COLPM0       0xD012
#define COLPM1       0xD013
#define COLPM2       0xD014
#define COLPM3       0xD015

/* PMG Offsets */
#define OFF_MISS     0x0300
#define OFF_P0       0x0400
#define PMG_BLOCK    2048
#define HPOS_BASE    48

static byte dlist[256];
static byte* lms_ptr;

/* HUD line memory (ANTIC 2 = 40 bytes) */
static byte hud_line[HUD_COLS];
static int last_score_hud = -1;

typedef struct { int active; int x, y; } Bullet;
typedef struct { int active; int x, y; int speed; } Enemy;

#define MAX_BULLETS 4
static Bullet bullets[MAX_BULLETS];

/* >>> USUWAMY 3ciego enemy: tylko 2 <<< */
#define ENEMY_COUNT 2
static Enemy  enemies[ENEMY_COUNT];

static word rng = 0xACE1;
static int score = 0;

/* Cyfry 8x8 (bit 7 = lewy pixel) */
static const byte DIGITS[10][8] = {
    {0x3E,0x41,0x41,0x41,0x41,0x41,0x3E,0x00}, // 0
    {0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0x00}, // 1
    {0x3E,0x01,0x01,0x3E,0x40,0x40,0x7F,0x00}, // 2
    {0x3E,0x01,0x01,0x3E,0x01,0x01,0x3E,0x00}, // 3
    {0x41,0x41,0x41,0x7F,0x01,0x01,0x01,0x00}, // 4
    {0x7F,0x40,0x40,0x7E,0x01,0x01,0x7E,0x00}, // 5
    {0x3E,0x40,0x40,0x7E,0x41,0x41,0x3E,0x00}, // 6
    {0x7F,0x01,0x02,0x04,0x08,0x08,0x08,0x00}, // 7
    {0x3E,0x41,0x41,0x3E,0x41,0x41,0x3E,0x00}, // 8
    {0x3E,0x41,0x41,0x3F,0x01,0x01,0x3E,0x00}  // 9
};

/* ---- Pomocnicze ---- */
static int abs_i(int a) { return (a < 0) ? -a : a; }
static word rnd16(void) {
    rng ^= (rng << 7); rng ^= (rng >> 9); rng ^= (rng << 8);
    return rng;
}
static byte x_to_hpos(int x) {
    int v = HPOS_BASE + x;
    return (v < 0) ? 0 : (v > 255) ? 255 : (byte)v;
}

static const byte PLANE_SPR[16] = {
    0x18,0x3C,0x7E,0xDB,0xFF,0x7E,0x3C,0x18,
    0x18,0x3C,0x7E,0xDB,0xFF,0x7E,0x3C,0x18
};
static const byte ENEMY_SPR[16] = {
    0x3C,0x7E,0xDB,0xFF,0xFF,0xDB,0x7E,0x3C,
    0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x18
};

static void init_background(void) {
    word y, x;
    byte* mem = (byte*)SCREEN_ADDR;
    for (y = 0; y < 256; y++) {
        for (x = 0; x < STRIDE; x++) {
            mem[y * STRIDE + x] = (((y / 16) & 1) ^ ((x / 4) & 1)) ? 0x55 : 0xAA;
        }
    }
}

/* Własny font do HUD: znaki 0..9 = cyfry, znak 10 = pusty */
static void hud_font_init(void) {
    byte* font;
    int i, d, r;

    font = (byte*)HUD_FONT_ADDR;
    for (i = 0; i < 1024; i++) font[i] = 0x00;

    for (d = 0; d < 10; d++) {
        for (r = 0; r < 8; r++) {
            font[d * 8 + r] = DIGITS[d][r];
        }
    }
    /* HUD_BLANK (10) zostaje 0x00 -> puste */
}

static void hud_init(void) {
    int i;
    for (i = 0; i < HUD_COLS; i++) hud_line[i] = HUD_BLANK;
    hud_line[HUD_POS + 0] = 0;
    hud_line[HUD_POS + 1] = 0;
    hud_line[HUD_POS + 2] = 0;
}

static void hud_update_if_needed(void) {
    int s, h, t, u;
    if (score == last_score_hud) return;
    last_score_hud = score;

    s = score;
    h = (s / 100) % 10;
    t = (s / 10) % 10;
    u = s % 10;

    hud_line[HUD_POS + 0] = (byte)h;
    hud_line[HUD_POS + 1] = (byte)t;
    hud_line[HUD_POS + 2] = (byte)u;
}

static void build_dlist(void) {
    word p = 0, i;

    for(i=0; i<3; i++) dlist[p++] = 0x70;

    /* ANTIC 2 + LMS (1 wiersz HUD) */
    dlist[p++] = (byte)(0x02 | 0x40);
    dlist[p++] = (byte)((word)hud_line & 0xFF);
    dlist[p++] = (byte)((word)hud_line >> 8);

    /* Mode E first line with LMS + HSCROL enable */
    dlist[p++] = (byte)(0x0E | 0x40 | 0x20 | 0x10);
    lms_ptr = &dlist[p];
    dlist[p++] = (byte)((word)SCREEN_ADDR & 0xFF);
    dlist[p++] = (byte)((word)SCREEN_ADDR >> 8);

    for (i = 0; i < (VIS_LINES - 1); i++) dlist[p++] = (byte)(0x0E | 0x20 | 0x10);

    dlist[p++] = 0x41;
    dlist[p++] = (byte)((word)dlist & 0xFF);
    dlist[p++] = (byte)((word)dlist >> 8);
}

int main(void) {
    word sx = 0, next_addr, frame = 0;
    byte fine_x, joy, trigPrev = 1;
    int planeX = 20, planeY = 90;
    word pmg_front = PMG0_BASE, pmg_back = PMG1_BASE;

    init_background();
    hud_font_init();
    hud_init();
    hud_update_if_needed();
    build_dlist();

    asm("sei");
    POKE(REG_NMIEN, 0x00);
    POKE(0xD301, 0xFE);
    POKE(REG_DMACTL, 0x00);

    /* ustaw CHBASE na nasz font */
    POKE(REG_CHBASE, (byte)(HUD_FONT_ADDR >> 8));

    POKE(REG_DLISTL, (byte)((word)dlist & 0xFF));
    POKE(REG_DLISTH, (byte)((word)dlist >> 8));

    /* kolory, żeby HUD był widoczny */
    POKE(COLBK,  0x00);
    POKE(COLPF1, 0x0E);

    POKE(REG_PRIOR, 0x01);
    POKE(SIZEM, 0x00);

    /* Player3 nieużywany – wyłączamy “efekt dużego enemy” */
    POKE(SIZEP3, 0x00);
    POKE(COLPM3, 0x00);
    POKE(HPOSP3, 0);

    POKE(REG_GRACTL, 0x03);

    POKE(COLPM0, 0x0F);
    POKE(COLPM1, 0x3A);
    POKE(COLPM2, 0x86);

    POKE(REG_PMBAS, (byte)(pmg_front >> 8));
    POKE(REG_DMACTL, 0x3E);

    while (1) {
        int i, j;

        joy = PEEK(REG_PORTA);
        if (!(joy & 1)) planeY -= 2; if (!(joy & 2)) planeY += 2;
        if (!(joy & 4)) planeX -= 2; if (!(joy & 8)) planeX += 2;

        if (planeX < 0) planeX = 0; if (planeX > 150) planeX = 150;
        if (planeY < 5) planeY = 5; if (planeY > (VIS_LINES-18)) planeY = (VIS_LINES-18);

        if (PEEK(REG_TRIG0) == 0 && trigPrev == 1) {
            for (i = 0; i < MAX_BULLETS; i++) {
                if (!bullets[i].active) {
                    bullets[i].active = 1;
                    bullets[i].x = planeX + 10;
                    bullets[i].y = planeY + 7;
                    break;
                }
            }
        }
        trigPrev = PEEK(REG_TRIG0);

        sx++; if (sx >= MAX_SX_PIX) sx = 0;
        fine_x = (byte)(sx & 3);
        next_addr = (word)(SCREEN_ADDR + (FIXED_COARSE_Y * STRIDE) + (sx >> 2));

        for (i = 0; i < MAX_BULLETS; i++) {
            if (bullets[i].active) {
                bullets[i].x += 4;
                if (bullets[i].x > 160) bullets[i].active = 0;
                for (j = 0; j < ENEMY_COUNT; j++) {
                    if (enemies[j].active &&
                        abs_i(bullets[i].x - enemies[j].x) < 8 &&
                        abs_i(bullets[i].y - enemies[j].y) < 14) {
                        bullets[i].active = 0; enemies[j].active = 0;
                        score++;
                        hud_update_if_needed();
                    }
                }
            }
        }

        if ((frame % 40) == 0) {
            for (i = 0; i < ENEMY_COUNT; i++) if (!enemies[i].active) {
                word r = rnd16();
                enemies[i].active = 1;
                enemies[i].x = 155;
                enemies[i].y = 30 + ((r >> 8) % 130);
                enemies[i].speed = 1; break;
            }
        }
        for (i = 0; i < ENEMY_COUNT; i++) if (enemies[i].active) {
            enemies[i].x -= enemies[i].speed;
            if (enemies[i].x < -10) enemies[i].active = 0;
        }

        memset((void*)pmg_back, 0, 2048);
        memcpy((void*)(pmg_back + OFF_P0 + planeY), PLANE_SPR, 16);

        /* enemies tylko na P1 i P2 */
        if (enemies[0].active) memcpy((void*)(pmg_back + 0x500 + enemies[0].y), ENEMY_SPR, 16);
        if (enemies[1].active) memcpy((void*)(pmg_back + 0x600 + enemies[1].y), ENEMY_SPR, 16);

        for (i = 0; i < MAX_BULLETS; i++) {
            if (bullets[i].active) {
                byte m = (byte)(3 << (i * 2));
                POKE(pmg_back + OFF_MISS + bullets[i].y, m);
                POKE(pmg_back + OFF_MISS + bullets[i].y + 1, m);
            }
        }

        while (PEEK(REG_VCOUNT) != 0);

        POKE(REG_HSCROL, (byte)(3 - fine_x));
        lms_ptr[0] = (byte)(next_addr & 0xFF);
        lms_ptr[1] = (byte)(next_addr >> 8);
        POKE(REG_PMBAS, (byte)(pmg_back >> 8));

        POKE(HPOSP0, x_to_hpos(planeX));
        POKE(HPOSP1, enemies[0].active ? x_to_hpos(enemies[0].x) : 0);
        POKE(HPOSP2, enemies[1].active ? x_to_hpos(enemies[1].x) : 0);
        POKE(HPOSP3, 0); /* zawsze off */

        for(i=0; i<4; i++) POKE(HPOSM0 + i, bullets[i].active ? x_to_hpos(bullets[i].x) : 0);

        { word tmp = pmg_front; pmg_front = pmg_back; pmg_back = tmp; }
        frame++;
    }
    return 0;
}