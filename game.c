#include <atari.h>
#include <peekpoke.h>
#include <string.h>
#define CFGFILE atari.cfg

typedef unsigned char byte;
typedef unsigned int  word;

/* ---- MEMORY ---- */
#define PMG0_BASE     0x4000
#define PMG1_BASE     0x4800
#define SCREEN_ADDR   0x5000
#define HUD_FONT_ADDR 0x3800

/* ---- PLAYFIELD ---- */
#define STRIDE          64
#define VIS_LINES       192
#define MAX_SX_PIX      ((64-40)*4)
#define FIXED_COARSE_Y  32

/* ---- HUD ---- */
#define HUD_COLS       40
#define CH_BLANK       10
#define CH_HEART       11
#define CH_EMPTY       12
#define CH_BAR_FULL    13
#define CH_BAR_EMPTY   14
/* Litery dla GAME OVER */
#define CH_G           15
#define CH_A           16
#define CH_M           17
#define CH_E           18
#define CH_O           19
#define CH_V           20
#define CH_R           21

#define HUD_SCORE_POS   1
#define HUD_LIVES_POS   8
#define HUD_ENERGY_POS 14

/* ---- HW ---- */
#define REG_DMACTL  0xD400
#define REG_DLISTL  0xD402
#define REG_DLISTH  0xD403
#define REG_HSCROL  0xD404
#define REG_PMBAS   0xD407
#define REG_CHBASE  0xD409
#define REG_VCOUNT  0xD40B
#define REG_NMIEN   0xD40E
#define REG_PORTA   0xD300
#define REG_GRACTL  0xD01D
#define REG_TRIG0   0xD010
#define REG_PRIOR   0xD01B
#define COLPF1      0xD017
#define COLBK       0xD01A
#define HPOSP0      0xD000
#define HPOSP1      0xD001
#define HPOSP2      0xD002
#define HPOSP3      0xD003
#define HPOSM0      0xD004
#define SIZEP0      0xD008
#define SIZEP1      0xD009
#define SIZEP2      0xD00A
#define SIZEP3      0xD00B
#define SIZEM       0xD00C
#define COLPM0      0xD012
#define COLPM1      0xD013
#define COLPM2      0xD014
#define COLPM3      0xD015

#define OFF_MISS    0x0300
#define OFF_P0      0x0400
#define OFF_P1      0x0500
#define OFF_P2      0x0600
#define OFF_P3      0x0700
#define HPOS_BASE   48

/* ---- GAMEPLAY ---- */
#define MAX_LIVES         3
#define MAX_ENERGY        10
#define INVINCIBLE_FRAMES 90
#define ENEMY_COUNT       4
#define MAX_BULLETS       4
#define MAX_EBULLETS      2

#define PMG_TOP  (32 + 8)
#define PMG_BOT  (32 + VIS_LINES - 8)

/*
 * SPAWN_ENTRY_X: pozycja startowa X wroga – tuż za prawą krawędzią widzialną.
 * Wróg wjeżdża z prawej strony, więc jest widoczny od razu przy krawędzi,
 * a nie pojawia się z „powietrza" na środku ekranu.
 */
#define SPAWN_ENTRY_X  162

/*
 * Agresja rośnie co AGGR_INTERVAL klatek.
 * aggression_level wyznacza:
 *   - prędkość wroga (speed)
 *   - czy wróg naprowadza się na gracza (chase)
 *   - częstość strzałów
 */
#define AGGR_INTERVAL  600   /* ~10 s przy 60 fps */
#define AGGR_MAX       6

/* ---- STRUCTS ---- */
typedef struct { byte active; int x, y; } Bullet;
typedef struct {
    byte active;
    int  x, y;
    byte speed;
    int  dy;
    byte stimer;
    byte chase;   /* 1 = leci w stronę gracza */
} Enemy;
typedef struct { byte active; int x, y; } EBullet;

/* ---- GLOBALS ---- */
static byte  dlist[256];
static byte *lms_ptr;
static byte  hud_line[HUD_COLS];

static int  score      = 0;
static byte lives      = MAX_LIVES;
static byte energy     = MAX_ENERGY;
static byte invincible = 0;
static byte game_over  = 0;

static byte last_score  = 255;
static byte last_lives  = 255;
static byte last_energy = 255;

static Bullet  bullets [MAX_BULLETS];
static Enemy   enemies [ENEMY_COUNT];
static EBullet ebullets[MAX_EBULLETS];

static word rng   = 0xACE1;
static word frame = 0;

/* Poziom agresji: 0..AGGR_MAX */
static byte aggression_level = 0;

/* go_line: jeden wiersz wyświetlany podczas GAME OVER */
static byte go_line[HUD_COLS];

/* ---- SPRITES ---- */
static const byte PLANE_P0[16] = {
    0x00,0x18,0x3C,0x7E,0x7E,0xFF,0xFF,0xFF,
    0xFF,0xFF,0x7E,0x7E,0x3C,0x18,0x00,0x00
};
static const byte PLANE_P1[16] = {
    0x81,0xC3,0x00,0x00,0x3C,0x3C,0x00,0x00,
    0x00,0x00,0x00,0x00,0xC3,0x81,0x00,0x00
};
static const byte ENEMY_SPR[16] = {
    0x00,0x00,0x03,0x07,0x1F,0x3F,0x7F,0xFF,
    0xFF,0x7F,0x3F,0x1F,0x07,0x03,0x00,0x00
};

/* ---- FONT DATA ---- */
static const byte DIGITS[10][8] = {
    {0x3E,0x41,0x41,0x41,0x41,0x41,0x3E,0x00},
    {0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0x00},
    {0x3E,0x01,0x01,0x3E,0x40,0x40,0x7F,0x00},
    {0x3E,0x01,0x01,0x3E,0x01,0x01,0x3E,0x00},
    {0x41,0x41,0x41,0x7F,0x01,0x01,0x01,0x00},
    {0x7F,0x40,0x40,0x7E,0x01,0x01,0x7E,0x00},
    {0x3E,0x40,0x40,0x7E,0x41,0x41,0x3E,0x00},
    {0x7F,0x01,0x02,0x04,0x08,0x08,0x08,0x00},
    {0x3E,0x41,0x41,0x3E,0x41,0x41,0x3E,0x00},
    {0x3E,0x41,0x41,0x3F,0x01,0x01,0x3E,0x00}
};
static const byte HEART_G[8]     = {0x00,0x6C,0xFE,0xFE,0x7C,0x38,0x10,0x00};
static const byte EMPTY_G[8]     = {0x00,0x6C,0x92,0x82,0x44,0x28,0x10,0x00};
static const byte BAR_FULL_G[8]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const byte BAR_EMPTY_G[8] = {0xFF,0x81,0x81,0x81,0x81,0x81,0x81,0xFF};

/* Litery: G A M E   O V E R */
static const byte LETTER_G[8] = {0x3E,0x41,0x40,0x4F,0x41,0x41,0x3E,0x00};
static const byte LETTER_A[8] = {0x1C,0x22,0x41,0x7F,0x41,0x41,0x41,0x00};
static const byte LETTER_M[8] = {0x41,0x63,0x55,0x49,0x41,0x41,0x41,0x00};
static const byte LETTER_E[8] = {0x7F,0x40,0x40,0x7E,0x40,0x40,0x7F,0x00};
static const byte LETTER_O[8] = {0x3E,0x41,0x41,0x41,0x41,0x41,0x3E,0x00};
static const byte LETTER_V[8] = {0x41,0x41,0x41,0x41,0x22,0x14,0x08,0x00};
static const byte LETTER_R[8] = {0x7E,0x41,0x41,0x7E,0x48,0x44,0x43,0x00};

/* ---- HELPERS ---- */
static int abs_i(int a) { return a<0 ? -a : a; }
static word rnd16(void) {
    rng^=(rng<<7); rng^=(rng>>9); rng^=(rng<<8); return rng;
}
static byte x_to_hpos(int x) {
    int v=HPOS_BASE+x;
    return v<0?0:v>255?255:(byte)v;
}

/* ---- BACKGROUND ---- */
static void init_bg(void) {
    word y,x; byte *m=(byte*)SCREEN_ADDR;
    for(y=0;y<256;y++)
        for(x=0;x<STRIDE;x++)
            m[y*STRIDE+x]=(((y/16)&1)^((x/4)&1))?0x55:0xAA;
}

/* ---- FONT ---- */
static void font_init(void) {
    byte *f=(byte*)HUD_FONT_ADDR;
    int i,d,r;
    for(i=0;i<1024;i++) f[i]=0;
    for(d=0;d<10;d++) for(r=0;r<8;r++) f[d*8+r]=DIGITS[d][r];
    for(r=0;r<8;r++) f[CH_HEART    *8+r]=HEART_G[r];
    for(r=0;r<8;r++) f[CH_EMPTY    *8+r]=EMPTY_G[r];
    for(r=0;r<8;r++) f[CH_BAR_FULL *8+r]=BAR_FULL_G[r];
    for(r=0;r<8;r++) f[CH_BAR_EMPTY*8+r]=BAR_EMPTY_G[r];
    /* Litery GAME OVER */
    for(r=0;r<8;r++) f[CH_G*8+r]=LETTER_G[r];
    for(r=0;r<8;r++) f[CH_A*8+r]=LETTER_A[r];
    for(r=0;r<8;r++) f[CH_M*8+r]=LETTER_M[r];
    for(r=0;r<8;r++) f[CH_E*8+r]=LETTER_E[r];
    for(r=0;r<8;r++) f[CH_O*8+r]=LETTER_O[r];
    for(r=0;r<8;r++) f[CH_V*8+r]=LETTER_V[r];
    for(r=0;r<8;r++) f[CH_R*8+r]=LETTER_R[r];
}

/* ---- HUD ---- */
static void hud_init(void) {
    int i;
    for(i=0;i<HUD_COLS;i++) hud_line[i]=CH_BLANK;
    hud_line[HUD_SCORE_POS]  =0;
    hud_line[HUD_SCORE_POS+1]=0;
    hud_line[HUD_SCORE_POS+2]=0;
    hud_line[HUD_LIVES_POS]  =CH_HEART;
    hud_line[HUD_LIVES_POS+1]=CH_HEART;
    hud_line[HUD_LIVES_POS+2]=CH_HEART;
    for(i=0;i<MAX_ENERGY;i++) hud_line[HUD_ENERGY_POS+i]=CH_BAR_FULL;
}

static void hud_update(void) {
    int i,s;
    byte sc=(byte)(score%1000);
    if(sc!=last_score){
        last_score=sc; s=score;
        hud_line[HUD_SCORE_POS]  =(byte)((s/100)%10);
        hud_line[HUD_SCORE_POS+1]=(byte)((s/10)%10);
        hud_line[HUD_SCORE_POS+2]=(byte)(s%10);
    }
    if(lives!=last_lives){
        last_lives=lives;
        for(i=0;i<MAX_LIVES;i++)
            hud_line[HUD_LIVES_POS+i]=(i<lives)?CH_HEART:CH_EMPTY;
    }
    if(energy!=last_energy){
        last_energy=energy;
        for(i=0;i<MAX_ENERGY;i++)
            hud_line[HUD_ENERGY_POS+i]=(i<energy)?CH_BAR_FULL:CH_BAR_EMPTY;
    }
}

/* ---- DISPLAY LIST ---- */
static void build_dlist(void) {
    word p=0,i;
    dlist[p++]=0x70;
    dlist[p++]=0x70;
    dlist[p++]=0x70;
    dlist[p++]=(byte)(0x02|0x40);
    dlist[p++]=(byte)((word)hud_line&0xFF);
    dlist[p++]=(byte)((word)hud_line>>8);
    dlist[p++]=(byte)(0x0E|0x40|0x20|0x10);
    lms_ptr=&dlist[p];
    dlist[p++]=(byte)(SCREEN_ADDR&0xFF);
    dlist[p++]=(byte)(SCREEN_ADDR>>8);
    for(i=0;i<(VIS_LINES-1);i++) dlist[p++]=(byte)(0x0E|0x20|0x10);
    dlist[p++]=0x41;
    dlist[p++]=(byte)((word)dlist&0xFF);
    dlist[p++]=(byte)((word)dlist>>8);
}

/*
 * show_go: wypełnia go_line napisem "GAME OVER" wyśrodkowanym.
 * "GAME OVER" = 9 znaków, przy HUD_COLS=40 => offset=(40-9)/2=15
 * Kolejność: G A M E SP O V E R
 */
static void show_go(void) {
    int i;
    for(i=0;i<HUD_COLS;i++) go_line[i]=CH_BLANK;
    /* pozycja 15..23 */
    go_line[15]=CH_G;
    go_line[16]=CH_A;
    go_line[17]=CH_M;
    go_line[18]=CH_E;
    go_line[19]=CH_BLANK;   /* spacja */
    go_line[20]=CH_O;
    go_line[21]=CH_V;
    go_line[22]=CH_E;
    go_line[23]=CH_R;
}

static void reset_all(int *px, int *py) {
    int i;
    score=0; lives=MAX_LIVES; energy=MAX_ENERGY;
    invincible=0; game_over=0;
    aggression_level=0;
    last_score=255; last_lives=255; last_energy=255;
    for(i=0;i<ENEMY_COUNT; i++) enemies [i].active=0;
    for(i=0;i<MAX_BULLETS; i++) bullets [i].active=0;
    for(i=0;i<MAX_EBULLETS;i++) ebullets[i].active=0;
    *px=20; *py=110;
    hud_init(); hud_update();
    build_dlist();
}

static void take_hit(int dmg) {
    energy=(byte)(energy>dmg ? energy-dmg : 0);
    last_energy=255;
    if(energy==0){
        lives--; energy=MAX_ENERGY;
        invincible=INVINCIBLE_FRAMES;
        last_lives=255; last_energy=255;
        if(lives==0){ lives=0; game_over=1; show_go(); }
    }
    hud_update();
}

/* ================================================================
   MAIN
   ================================================================ */
int main(void) {
    word sx=0,next_addr;
    byte fine_x,joy,blink;
    byte trigPrev;
    int  planeX=20,planeY=110;
    word pmg_front=PMG0_BASE, pmg_back=PMG1_BASE;
    int  i,j;

    init_bg();
    font_init();
    hud_init();
    hud_update();
    build_dlist();

    asm("sei");
    POKE(REG_NMIEN,0x00);
    POKE(0xD301,0xFE);
    POKE(REG_DMACTL,0x00);

    POKE(REG_CHBASE,(byte)(HUD_FONT_ADDR>>8));
    POKE(REG_DLISTL,(byte)((word)dlist&0xFF));
    POKE(REG_DLISTH,(byte)((word)dlist>>8));

    POKE(COLBK, 0x00);
    POKE(COLPF1,0x0E);
    POKE(REG_PRIOR,0x01);
    POKE(SIZEP0,0x00); POKE(SIZEP1,0x00);
    POKE(SIZEP2,0x00); POKE(SIZEP3,0x00);
    POKE(SIZEM, 0x00);
    POKE(REG_GRACTL,0x03);
    POKE(COLPM0,0x96);
    POKE(COLPM1,0xF8);
    POKE(COLPM2,0xC4);
    POKE(COLPM3,0x34);
    POKE(REG_PMBAS,(byte)(pmg_front>>8));
    POKE(REG_DMACTL,0x3E);

    trigPrev = PEEK(REG_TRIG0);

    while(1){

        /* GAME OVER */
        if(game_over){
            /*
             * Podczas GAME OVER: pasek górny pokazuje "GAME OVER" (go_line).
             * Bez migania – stały napis, tylko Fire wznawia grę.
             */
            POKE(dlist+4,(byte)((word)go_line&0xFF));
            POKE(dlist+5,(byte)((word)go_line>>8));

            if(PEEK(REG_TRIG0)==0) reset_all(&planeX,&planeY);
            goto render;
        }

        /* Przywracamy normalny HUD gdy gra trwa */
        POKE(dlist+4,(byte)((word)hud_line&0xFF));
        POKE(dlist+5,(byte)((word)hud_line>>8));

        /* INPUT */
        joy=PEEK(REG_PORTA);
        if(!(joy&1)) planeY-=2;
        if(!(joy&2)) planeY+=2;
        if(!(joy&4)) planeX-=2;
        if(!(joy&8)) planeX+=2;
        if(planeX<0)       planeX=0;
        if(planeX>150)     planeX=150;
        if(planeY<PMG_TOP) planeY=PMG_TOP;
        if(planeY>PMG_BOT) planeY=PMG_BOT;

        /* STRZAL GRACZA */
        if(PEEK(REG_TRIG0)==0 && trigPrev==1){
            for(i=0;i<MAX_BULLETS;i++){
                if(!bullets[i].active){
                    bullets[i].active=1;
                    bullets[i].x=planeX+12;
                    bullets[i].y=planeY+6;
                    break;
                }
            }
        }
        trigPrev=PEEK(REG_TRIG0);

        /* SCROLL */
        sx++; if(sx>=MAX_SX_PIX) sx=0;
        fine_x=(byte)(sx&3);
        next_addr=(word)(SCREEN_ADDR + (FIXED_COARSE_Y * STRIDE) + (sx>>2));

        /* ROSNĄCA AGRESJA */
        if(frame>0 && (frame % AGGR_INTERVAL)==0){
            if(aggression_level < AGGR_MAX) aggression_level++;
        }

        /* POCISKI GRACZA */
        for(i=0;i<MAX_BULLETS;i++){
            if(!bullets[i].active) continue;
            bullets[i].x+=4;
            if(bullets[i].x>165){ bullets[i].active=0; continue; }
            for(j=0;j<ENEMY_COUNT;j++){
                if(enemies[j].active &&
                   abs_i(bullets[i].x-enemies[j].x)<8 &&
                   abs_i(bullets[i].y-enemies[j].y)<10){
                    bullets[i].active=0; enemies[j].active=0;
                    score++; last_score=255; hud_update();
                }
            }
        }

        /* SPAWN WROGOW
         *
         * Wrogowie startują z SPAWN_ENTRY_X (162) – poza prawym brzegiem ekranu.
         * Dzięki temu „wjeżdżają" z krawędzi zamiast pojawiać się z nikąd.
         *
         * Agresja wpływa na:
         *   speed  = 1 + aggression_level/2
         *   stimer = krótszy (częstsze strzały)
         *   chase  = przy aggression_level>=2 część wrogów naprowadza się na gracza
         *
         * Transparentność (przenikanie) zapewnia hardware Atari – PMG domyślnie
         * używa OR na playfield (PRIOR=0x01), więc dwa sprite'y na tym samym
         * miejscu nie zasłaniają się wzajemnie – widać oba.
         */
        if((frame%50)==0){
            for(i=0;i<ENEMY_COUNT;i++){
                if(!enemies[i].active){
                    word r=rnd16();
                    byte spd;
                    byte do_chase;

                    spd = 1 + (aggression_level >> 1);  /* 1..4 */
                    if(spd>4) spd=4;

                    /*
                     * Chase: przy aggression>=2 co drugi nowo-spawowany wróg
                     * naprowadza się na gracza (dy wyznaczamy w fazie ruchu).
                     */
                    do_chase = (aggression_level >= 2) && ((r & 0x01) != 0);

                    enemies[i].active = 1;
                    enemies[i].x      = SPAWN_ENTRY_X;
                    enemies[i].y      = PMG_TOP + ((r>>8) % (PMG_BOT - PMG_TOP - 16));
                    enemies[i].speed  = spd;
                    enemies[i].dy     = (i&1) ? 1 : -1;
                    enemies[i].stimer = (byte)(60 - aggression_level*5 + (r&0x3F));
                    if(enemies[i].stimer < 20) enemies[i].stimer = 20;
                    enemies[i].chase  = do_chase;
                    break;
                }
            }
        }

        /* RUCH WROGOW */
        for(i=0;i<ENEMY_COUNT;i++){
            if(!enemies[i].active) continue;
            enemies[i].x -= enemies[i].speed;
            if(enemies[i].x < -10){ enemies[i].active=0; continue; }

            if(enemies[i].chase){
                /*
                 * Naprowadzanie: dy przesuwa wroga w kierunku Y gracza.
                 * Prędkość pionowa ograniczona do 2 px/klatkę.
                 */
                if(enemies[i].y < planeY - 2)       enemies[i].dy =  2;
                else if(enemies[i].y > planeY + 2)  enemies[i].dy = -2;
                else                                  enemies[i].dy =  0;
            } else {
                /* Normalne odbijanie od granic */
                if(enemies[i].y < PMG_TOP)           { enemies[i].y=PMG_TOP;    enemies[i].dy= 1; }
                if(enemies[i].y > PMG_BOT-16)        { enemies[i].y=PMG_BOT-16; enemies[i].dy=-1; }
            }
            enemies[i].y += enemies[i].dy;
            /* Zawsze pilnujemy granic bezpieczeństwa */
            if(enemies[i].y < PMG_TOP)    enemies[i].y = PMG_TOP;
            if(enemies[i].y > PMG_BOT-16) enemies[i].y = PMG_BOT-16;

            if(enemies[i].stimer>0) enemies[i].stimer--;
            if(enemies[i].stimer==0){
                /* Czas do następnego strzału maleje z agresją */
                enemies[i].stimer = (byte)(80 - aggression_level*6 + (rnd16()&0x3F));
                if(enemies[i].stimer < 20) enemies[i].stimer = 20;

                if(enemies[i].x > 4){
                    for(j=0;j<MAX_EBULLETS;j++){
                        if(!ebullets[j].active){
                            ebullets[j].active=1;
                            ebullets[j].x=enemies[i].x-4;
                            ebullets[j].y=enemies[i].y+6;
                            break;
                        }
                    }
                }
            }
        }

        /* POCISKI WROGOW */
        for(i=0;i<MAX_EBULLETS;i++){
            if(!ebullets[i].active) continue;
            ebullets[i].x-=3;
            if(ebullets[i].x<-4){ ebullets[i].active=0; continue; }
            if(invincible==0 &&
               abs_i(ebullets[i].x-planeX)<10 &&
               abs_i(ebullets[i].y-planeY)<10){
                ebullets[i].active=0;
                take_hit(1);
            }
        }

        /* KOLIZJA GRACZ-WROG */
        if(invincible==0){
            for(i=0;i<ENEMY_COUNT;i++){
                if(!enemies[i].active) continue;
                if(abs_i(planeX-enemies[i].x)<10 &&
                   abs_i(planeY-enemies[i].y)<10){
                    enemies[i].active=0;
                    take_hit(3);
                }
            }
        }
        if(invincible>0) invincible--;

render:
        memset((void*)pmg_back,0,2048);

        blink=(byte)((invincible>>2)&1);
        if(!blink||!invincible){
            memcpy((void*)(pmg_back+OFF_P0+planeY),PLANE_P0,16);
            memcpy((void*)(pmg_back+OFF_P1+planeY),PLANE_P1,16);
        }

        /*
         * Renderowanie wrogów: P2 obsługuje wrogów 0 i 2, P3 obsługuje 1 i 3.
         * Dwa sprite'y na tym samym kanale PMG renderowane przez OR bitów –
         * hardware Atari daje naturalną transparentność (przeźroczystość):
         * piksele ustawione przez pierwszego wroga i drugiego sumują się,
         * tło przebija tam gdzie żaden bit nie jest ustawiony.
         */
        if(enemies[0].active) memcpy((void*)(pmg_back+OFF_P2+enemies[0].y),ENEMY_SPR,16);
        if(enemies[2].active){
            /* OR z tym co już jest w buforze – oba wrogi przenikają się */
            byte *dst = (byte*)(pmg_back+OFF_P2+enemies[2].y);
            int  k;
            for(k=0;k<16;k++) dst[k] |= ENEMY_SPR[k];
        }
        if(enemies[1].active) memcpy((void*)(pmg_back+OFF_P3+enemies[1].y),ENEMY_SPR,16);
        if(enemies[3].active){
            byte *dst = (byte*)(pmg_back+OFF_P3+enemies[3].y);
            int  k;
            for(k=0;k<16;k++) dst[k] |= ENEMY_SPR[k];
        }

        for(i=0;i<MAX_BULLETS;i++){
            if(bullets[i].active){
                byte m=(byte)(3<<(i*2));
                POKE(pmg_back+OFF_MISS+bullets[i].y,  m);
                POKE(pmg_back+OFF_MISS+bullets[i].y+1,m);
            }
        }
        for(i=0;i<MAX_EBULLETS;i++){
            if(ebullets[i].active){
                byte prev=PEEK(pmg_back+OFF_MISS+ebullets[i].y);
                POKE(pmg_back+OFF_MISS+ebullets[i].y,  prev|0x03);
                POKE(pmg_back+OFF_MISS+ebullets[i].y+1,prev|0x03);
            }
        }

        while(PEEK(REG_VCOUNT)!=0);

        POKE(REG_HSCROL,(byte)(3-fine_x));
        lms_ptr[0]=(byte)(next_addr&0xFF);
        lms_ptr[1]=(byte)(next_addr>>8);
        POKE(REG_PMBAS,(byte)(pmg_back>>8));

        POKE(HPOSP0,x_to_hpos(planeX));
        POKE(HPOSP1,x_to_hpos(planeX));

        {
            byte hx2=0, hx3=0;
            if(enemies[0].active) hx2=x_to_hpos(enemies[0].x);
            if(enemies[2].active) hx2=x_to_hpos(enemies[2].x);
            if(enemies[1].active) hx3=x_to_hpos(enemies[1].x);
            if(enemies[3].active) hx3=x_to_hpos(enemies[3].x);
            POKE(HPOSP2,hx2);
            POKE(HPOSP3,hx3);
        }

        for(i=0;i<MAX_BULLETS;i++)
            POKE(HPOSM0+i,bullets[i].active?x_to_hpos(bullets[i].x):0);
        for(i=0;i<MAX_EBULLETS;i++)
            if(ebullets[i].active && !bullets[i%MAX_BULLETS].active)
                POKE(HPOSM0+(i%4),x_to_hpos(ebullets[i].x));

        { word tmp=pmg_front; pmg_front=pmg_back; pmg_back=tmp; }
        frame++;
    }
    return 0;
}