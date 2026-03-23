/*
 * raycaster.c — Japanese Horror Raycaster for DE1-SoC, RISC-V RV32IM (Nios V/g)
 * Simulate at https://cpulator.01xz.net/?sys=rv32-de1soc
 *
 * Controls via the CPUlator PS/2 keyboard panel:
 *   W / UP arrow   move forward      S / DOWN arrow  move backward
 *   A              strafe left       D               strafe right
 *   LEFT arrow     turn left         RIGHT arrow     turn right
 *   SPACE          open/close doors
 */

/* DE1-SoC memory-mapped addresses */
#define PS2_BASE            0xFF200100
#define PIXEL_BUF_CTRL_BASE 0xFF203020
#define CHAR_BUF_BASE       0x09000000

#define SCREEN_W   320
#define SCREEN_H   240
#define HALF_H_BASE 120

static int HALF_H = 120;

#define MAP_W   16
#define MAP_H   16

#define MOVE_SPEED  0.18f
#define COLL_M      0.25f

#define ROT_C   0.99750208f
#define ROT_S   0.07063998f

#define MM_TILE  2

/* Raycast view (bottom-left top-down overlay) */
#define RV_TILE  4
#define RV_W     (MAP_W * RV_TILE)
#define RV_H     (MAP_H * RV_TILE)
#define RV_X     0
#define RV_Y     0

#define FABS(x)  ((x) < 0.0f ? -(x) : (x))

#define RGB565(r,g,b) \
    ((unsigned short)(((unsigned)(r)<<11)|((unsigned)(g)<<5)|(unsigned)(b)))

/* Fog and lighting constants */
#define FOG_MAX_Q8      2048
#define LIGHT_RADIUS_SQ_FP 2304
#define MAX_LIGHTS      3
#define TILE_DOOR       5
#define TILE_WINDOW     6
#define DOOR_MAX        8
#define DOOR_ANIM_LEN   30

/* Directional brightness: N=moonlit, E=lit, W=dim, S=dark */
static const int FACE_BRIGHT[4] = {256, 160, 224, 192};

/* Textures: 6 textures, each 16x16, RGB565. Generated at init_textures(). */
static unsigned short TEX[6][16][16];
/* 0=dark wood planks, 1=shoji screen, 2=door, 3=stone, 4=tatami floor, 5=ceiling beams */

/* Wall type -> texture index: type 1=darkwood, 2=shoji, 3=stone, 4=darkwood, 5=door, 6=window(unused) */
static const int WALL_TEX[7] = {0, 0, 1, 3, 0, 2, 0};

/* Minimap colours */
#define COL_MM_OPEN  RGB565( 1,  3,  1)
#define COL_MM_WALL  RGB565(12, 24, 12)
#define COL_MM_DOOR  RGB565(20, 40,  5)
#define COL_MM_WIN   RGB565(10, 30, 15)
#define COL_MM_PLAY  RGB565(31, 63,  0)
#define COL_MM_DIR   RGB565(31, 63, 31)
#define COL_MM_BDR   RGB565(20, 40, 20)
#define COL_CROSS    RGB565(31, 63, 31)

/* 16x16 haunted house map
 * 0=open, 1=dark wood, 2=shoji, 3=stone, 4=dark wood alt, 5=door, 6=barred window */
static const int MAP[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
    {1,0,0,0,0,5,0,0,0,0,5,0,0,0,0,1},
    {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
    {1,1,2,6,1,1,5,1,6,5,1,6,1,3,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,6,1,5,1,1,1,0,0,1,1,1,5,1,6,1},
    {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
    {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
    {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
    {1,1,1,6,1,1,0,0,0,0,1,1,6,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

/* Player state: start in entry area facing north */
static float px  = 8.0f;
static float py  = 13.5f;
static float dx  = 0.0f;
static float dy  = -1.0f;
static float plx = 0.577f;
static float ply = 0.0f;

/* Key-held flags */
static int k_w=0, k_s=0, k_a=0, k_d=0;
static int k_up=0, k_dn=0, k_lt=0, k_rt=0;
static int k_space=0;
static int space_prev=0;

/* Door state */
static int door_x[DOOR_MAX], door_y[DOOR_MAX];
static int door_state[DOOR_MAX];  /* 0=closed 1=opening 2=open 3=closing */
static int door_timer[DOOR_MAX];
static int door_map_idx[MAP_H][MAP_W]; /* -1 = no door, 0+ = door index */
static int door_count = 0;

/* Torch lights */
static float light_x[MAX_LIGHTS] = {7.5f, 2.5f, 13.0f};
static float light_y[MAX_LIGHTS] = {5.5f, 2.0f,  9.0f};
static int light_count = 3;

/* Frame counter for torch flicker */
static int frame_count = 0;

/* Sprite system — multi-angle textures
 * Symmetric objects: 1 texture each (lantern=0, candle=1, barrel=2, plant=3)
 * Chair: 3 textures (front=4, side=5, back=6)
 * Table: 2 textures (short=7, long=8)
 * Sprite types: 0=lantern, 1=candle, 2=barrel, 3=plant, 4=chair, 5=table, 6=ghost */
#define MAX_SPRITES      25
#define SPRITE_TEX_COUNT 10
static unsigned short STEX[SPRITE_TEX_COUNT][16][16];

typedef struct { float x, y; int type; int facing; } Sprite;
static Sprite sprites[MAX_SPRITES];
static int sprite_count = 0;
static float z_buf[SCREEN_W];

/* Sprite height scale (numerator, denominator=10) */
static const int SPRITE_SCALE[7] = {6, 7, 5, 6, 5, 4, 8};
/* lantern=0.6, candle=0.7, barrel=0.5, plant=0.6, chair=0.5, table=0.4, ghost=0.8 */

/* Ghost event state */
static int ghost_active = 1;
static int ghost_fade = 0;
static int ghost_sprite_idx;


/* Floor/ceiling row-distance LUT (8.8 fixed-point) */
static int ROW_DIST_FP[120];

/* Ray hit endpoints for raycast view */
static float ray_hx[SCREEN_W], ray_hy[SCREEN_W];

/* Double buffer pointer */
static volatile short *back_buf;
static int buf_index = 0;

/* HUD state */
static int hud_visible = 0;

/*
 * init_textures — procedural 16x16 wall textures (RGB565).
 * TEX[0]=dark wood planks, [1]=shoji screen, [2]=door panels,
 * [3]=stone/plaster, [4]=tatami floor, [5]=ceiling beams.
 */
static void init_textures(void)
{
    int y, x;

    /* TEX[0]: Dark wood vertical planks */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int edge = (x % 4 == 0);
            int grain = ((y + (x / 4) * 3) % 5 == 0);
            if (edge)
                TEX[0][y][x] = RGB565(3, 6, 2);
            else if (grain)
                TEX[0][y][x] = RGB565(6, 13, 4);
            else
                TEX[0][y][x] = RGB565(5, 10, 3);
        }

    /* TEX[1]: Shoji screen */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int frame = (y==0||y==7||y==8||y==15||x==0||x==7||x==8||x==15);
            TEX[1][y][x] = frame ? RGB565(6, 12, 3) : RGB565(26, 52, 22);
        }

    /* TEX[2]: Door */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            unsigned short c = RGB565(6, 12, 4);
            if (y==0||y==15||x==0||x==15)
                c = RGB565(4, 8, 2);
            else if ((y>=2&&y<=6&&x>=2&&x<=6) ||
                     (y>=2&&y<=6&&x>=9&&x<=13) ||
                     (y>=9&&y<=13&&x>=2&&x<=6) ||
                     (y>=9&&y<=13&&x>=9&&x<=13))
                c = RGB565(8, 17, 5);
            else if (y>=7&&y<=8&&x>=7&&x<=8)
                c = RGB565(16, 32, 16);
            TEX[2][y][x] = c;
        }

    /* TEX[3]: Stone/plaster */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            unsigned short c = RGB565(10, 20, 10);
            if (y % 4 == 0)
                c = RGB565(7, 14, 7);
            else if ((y / 4) % 2 == 0 && x == 8)
                c = RGB565(7, 14, 7);
            else if ((y / 4) % 2 == 1 && (x == 0 || x == 15))
                c = RGB565(7, 14, 7);
            TEX[3][y][x] = c;
        }

    /* TEX[4]: Tatami floor */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            if (y==0||y==15||x==0||x==15)
                TEX[4][y][x] = RGB565(5, 10, 2);
            else if (y % 2 == 0)
                TEX[4][y][x] = RGB565(22, 44, 10);
            else
                TEX[4][y][x] = RGB565(20, 40, 9);
        }

    /* TEX[5]: Dark ceiling with beam grid */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            unsigned short c = RGB565(3, 6, 2);
            if (y <= 1 || (y >= 8 && y <= 9))
                c = RGB565(5, 10, 3);
            if (x >= 7 && x <= 8)
                c = RGB565(5, 10, 3);
            TEX[5][y][x] = c;
        }
}

/*
 * init_sprite_textures — procedural 16x16 sprite textures (RGB565).
 * STEX[0]=lantern, [1]=candle, [2]=barrel, [3]=plant, [4-6]=chair
 * (front/side/back), [7-8]=table (short/long), [9]=ghost.
 * Transparent pixels are 0x0000 (black).
 */
static void init_sprite_textures(void)
{
    int t, y, x;

    for (t = 0; t < SPRITE_TEX_COUNT; t++)
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                STEX[t][y][x] = 0;

    /* STEX[0]: Stone Lantern — 3D shaded, glowing window */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0, g=0, b=0, s=0;
            if (y >= 13 && x >= 4 && x <= 11) {
                int cx = x - 7; r = 9-(cx>0?cx:-cx)/2; g=r*2; b=r; s=1;
            } else if (y >= 3 && y <= 12 && x >= 6 && x <= 9) {
                int sh = (x==6)?3:(x==9)?-2:(x==7)?1:0;
                r=10+sh; g=20+sh*2; b=10+sh; s=1;
            } else if (y <= 2 && x >= 5 && x <= 10) {
                int cx = x - 7; r=10-(cx>0?cx:-cx)/2; g=r*2; b=r; s=1;
            }
            if (y >= 5 && y <= 8 && x >= 6 && x <= 9) {
                int gw = (x==6||x==9||y==5||y==8)?0:3;
                r=25+gw; g=38+gw; b=3+gw; s=1;
            }
            if (s) {
                if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[0][y][x]=RGB565(r,g,b);
            }
        }

    /* STEX[1]: Candle Stand — pole, candle, flame */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if (y>=14&&x>=5&&x<=10){r=7;g=14;b=7;s=1;}
            if (y>=5&&y<=13&&x>=7&&x<=8){r=(x==7)?6:4;g=r*2;b=r;s=1;}
            if (y>=1&&y<=4&&x>=6&&x<=9){
                int sh=(x==6)?4:(x==9)?-4:(x==7)?2:0;
                r=24+sh;g=48+sh*2;b=22+sh;s=1;
            }
            if (y==0&&x>=7&&x<=8){r=31;g=55;b=4;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[1][y][x]=RGB565(r,g,b);}
        }

    /* STEX[2]: Barrel — cylindrical shading */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            int hw=(y>=6&&y<=11)?5:(y>=4&&y<=13)?4:3;
            int cl=8-hw, cr=7+hw;
            if (y>=3&&y<=14&&x>=cl&&x<=cr) {
                int cx=x-7,sh=hw-(cx>0?cx:-cx);
                if (y==5||y==9||y==13){r=4+sh/2;g=8+sh;b=2+sh/3;}
                else{r=7+sh;g=13+sh*2;b=3+sh/2;}
                if(x==cl||x==cr){r-=2;g-=4;b-=1;} s=1;
            }
            if(y>=1&&y<=2&&x>=5&&x<=10){r=13;g=24;b=6;if(y==1){r+=2;g+=4;b+=2;}s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[2][y][x]=RGB565(r,g,b);}
        }

    /* STEX[3]: Dead Plant — shaded pot, branches, leaves */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if(y>=12&&x>=5&&x<=10){int cx=x-7,sh=3-(cx>0?cx:-cx);
                r=5+sh;g=9+sh;b=3+sh/2;s=1;}
            if(y==11&&x>=4&&x<=11){r=8;g=14;b=5;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[3][y][x]=RGB565(r,g,b);}
        }
    STEX[3][7][7]=RGB565(5,10,3);STEX[3][7][8]=RGB565(5,10,3);
    STEX[3][8][7]=RGB565(5,10,3);STEX[3][9][7]=RGB565(6,11,3);
    STEX[3][9][8]=RGB565(6,11,3);STEX[3][10][7]=RGB565(6,12,3);STEX[3][10][8]=RGB565(6,12,3);
    STEX[3][2][7]=RGB565(4,8,2);STEX[3][3][6]=RGB565(5,9,2);STEX[3][3][8]=RGB565(4,8,2);
    STEX[3][4][5]=RGB565(4,8,2);STEX[3][4][9]=RGB565(4,8,2);STEX[3][5][4]=RGB565(3,7,2);
    STEX[3][5][7]=RGB565(5,10,3);STEX[3][5][10]=RGB565(3,7,2);
    STEX[3][6][6]=RGB565(5,9,2);STEX[3][6][8]=RGB565(4,8,2);
    STEX[3][1][6]=RGB565(3,8,1);STEX[3][2][9]=RGB565(3,8,1);
    STEX[3][3][5]=RGB565(3,7,1);STEX[3][4][10]=RGB565(2,7,1);
    STEX[3][5][3]=RGB565(2,6,1);STEX[3][6][4]=RGB565(2,6,1);

    /* STEX[4]: Chair FRONT — backrest, open seat, legs */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if(y<=6&&x>=5&&x<=10){int sh=(x==5)?2:(x==10)?-2:0;
                r=7+sh;g=14+sh*2;b=3+sh;if(y==0){r+=2;g+=4;}s=1;}
            if(y>=7&&y<=9&&x>=4&&x<=11){int sh=(y==7)?3:(y==9)?-1:1;
                r=6+sh;g=12+sh*2;b=3+sh;if(x==4||x==11){r-=1;g-=2;}s=1;}
            if(y>=10&&y<=15&&(x==4||x==5||x==10||x==11)){
                r=(x==4||x==10)?6:4;g=r*2;b=r/2+1;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[4][y][x]=RGB565(r,g,b);}
        }

    /* STEX[5]: Chair SIDE — thin profile, backrest on left, seat, legs */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if(y<=6&&x>=4&&x<=6){r=7;g=14;b=3;if(x==4){r+=1;g+=2;}s=1;}
            if(y>=7&&y<=9&&x>=4&&x<=11){int sh=(y==7)?2:(y==9)?-1:0;
                r=6+sh;g=12+sh*2;b=3+sh;s=1;}
            if(y>=10&&y<=15&&(x==4||x==5||x==10||x==11)){
                r=5;g=10;b=3;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[5][y][x]=RGB565(r,g,b);}
        }

    /* STEX[6]: Chair BACK — solid backrest panel, legs */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if(y<=8&&x>=4&&x<=11){int sh=(x==4)?1:(x==11)?-1:0;
                r=5+sh;g=10+sh*2;b=3+sh;if(y==0){r+=1;g+=2;}s=1;}
            if(y>=10&&y<=15&&(x==4||x==5||x==10||x==11)){
                r=5;g=10;b=3;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[6][y][x]=RGB565(r,g,b);}
        }

    /* STEX[7]: Table SHORT end — narrow view, legs */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if(y>=5&&y<=8&&x>=5&&x<=10){int sh=(y==5)?3:(y==8)?-2:1;
                r=6+sh;g=12+sh*2;b=3+sh;if(x==5||x==10){r-=1;g-=2;}s=1;}
            if(y>=9&&y<=14&&(x==5||x==6||x==9||x==10)){
                r=5;g=10;b=3;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[7][y][x]=RGB565(r,g,b);}
        }

    /* STEX[8]: Table LONG side — wide view, legs */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int r=0,g=0,b=0,s=0;
            if(y>=5&&y<=8&&x>=2&&x<=13){int sh=(y==5)?3:(y==8)?-2:1;
                r=6+sh;g=12+sh*2;b=3+sh;if(x==2||x==13){r-=1;g-=2;}s=1;}
            if(y>=9&&y<=14&&((x>=3&&x<=4)||(x>=11&&x<=12))){
                r=(x==3||x==11)?6:4;g=r*2;b=r/2+1;s=1;}
            if(s){if(r<0)r=0;if(r>31)r=31;if(g<0)g=0;if(g>63)g=63;if(b<0)b=0;if(b>31)b=31;
                STEX[8][y][x]=RGB565(r,g,b);}
        }

    /* STEX[9]: Ghost — pale face, dark hair, gaping mouth, clawed hands */
    {
        unsigned short hair  = RGB565(1, 2, 1);
        unsigned short hair2 = RGB565(2, 4, 2);
        unsigned short skin  = RGB565(28, 56, 28);
        unsigned short shad  = RGB565(20, 40, 20);
        unsigned short dark  = RGB565(6, 12, 6);
        unsigned short eye   = RGB565(31, 63, 31);  /* white glint */
        unsigned short mouth = RGB565(8, 4, 3);
        unsigned short robe  = RGB565(2, 4, 2);
        unsigned short claw  = RGB565(24, 48, 24);

        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                STEX[9][y][x] = 0;

        /* Row 0: hair crown, wide */
        for (x = 3; x <= 12; x++) STEX[9][0][x] = hair;
        /* Row 1-2: full hair mass */
        for (x = 2; x <= 13; x++) { STEX[9][1][x] = hair; STEX[9][2][x] = hair; }
        STEX[9][1][1] = hair2; STEX[9][2][14] = hair2; /* wispy edges */
        /* Row 3: hair with skin peeking through */
        for (x = 2; x <= 13; x++) STEX[9][3][x] = hair;
        STEX[9][3][7] = shad; STEX[9][3][8] = shad;
        /* Row 4: hair parts, forehead visible */
        for (x = 2; x <= 13; x++) STEX[9][4][x] = hair;
        STEX[9][4][6] = skin; STEX[9][4][7] = skin; STEX[9][4][8] = skin; STEX[9][4][9] = skin;
        /* Row 5: eyes — white glints through hair */
        for (x = 3; x <= 12; x++) STEX[9][5][x] = hair;
        STEX[9][5][5] = skin; STEX[9][5][6] = eye; STEX[9][5][7] = hair;
        STEX[9][5][8] = hair; STEX[9][5][9] = eye; STEX[9][5][10] = skin;
        /* Row 6: under-eye, nose */
        for (x = 3; x <= 12; x++) STEX[9][6][x] = hair;
        STEX[9][6][5] = dark; STEX[9][6][6] = shad; STEX[9][6][7] = skin;
        STEX[9][6][8] = skin; STEX[9][6][9] = shad; STEX[9][6][10] = dark;
        /* Row 7: mouth — gaping dark slit */
        for (x = 3; x <= 12; x++) STEX[9][7][x] = hair;
        STEX[9][7][5] = shad; STEX[9][7][6] = mouth; STEX[9][7][7] = mouth;
        STEX[9][7][8] = mouth; STEX[9][7][9] = mouth; STEX[9][7][10] = shad;
        /* Row 8: chin, hair draping */
        for (x = 2; x <= 13; x++) STEX[9][8][x] = hair;
        STEX[9][8][6] = shad; STEX[9][8][7] = skin; STEX[9][8][8] = skin; STEX[9][8][9] = shad;
        /* Row 9-10: neck/shoulders in dark robe, hair on sides */
        for (x = 3; x <= 12; x++) { STEX[9][9][x] = robe; STEX[9][10][x] = robe; }
        STEX[9][9][2] = hair; STEX[9][9][3] = hair; STEX[9][9][12] = hair; STEX[9][9][13] = hair;
        STEX[9][10][1] = hair2; STEX[9][10][2] = hair; STEX[9][10][13] = hair; STEX[9][10][14] = hair2;
        STEX[9][9][7] = dark; STEX[9][9][8] = dark; /* neck */
        /* Row 11-12: torso in robe, arms reaching out */
        for (x = 4; x <= 11; x++) { STEX[9][11][x] = robe; STEX[9][12][x] = robe; }
        STEX[9][11][2] = robe; STEX[9][11][3] = robe;
        STEX[9][11][12] = robe; STEX[9][11][13] = robe;
        STEX[9][12][1] = robe; STEX[9][12][2] = robe; STEX[9][12][3] = robe;
        STEX[9][12][12] = robe; STEX[9][12][13] = robe; STEX[9][12][14] = robe;
        /* Row 13: forearms reaching forward */
        STEX[9][13][5] = robe; STEX[9][13][6] = robe;
        STEX[9][13][9] = robe; STEX[9][13][10] = robe;
        STEX[9][13][1] = robe; STEX[9][13][2] = claw;
        STEX[9][13][13] = claw; STEX[9][13][14] = robe;
        /* Row 14-15: clawed hands — bony pale fingers spread */
        STEX[9][14][1] = claw; STEX[9][14][2] = claw; STEX[9][14][3] = claw;
        STEX[9][14][5] = robe; STEX[9][14][6] = robe;
        STEX[9][14][9] = robe; STEX[9][14][10] = robe;
        STEX[9][14][12] = claw; STEX[9][14][13] = claw; STEX[9][14][14] = claw;
        STEX[9][15][0] = claw; STEX[9][15][1] = skin; STEX[9][15][2] = claw;
        STEX[9][15][3] = skin;
        STEX[9][15][12] = skin; STEX[9][15][13] = claw; STEX[9][15][14] = skin;
        STEX[9][15][15] = claw;
    }
}

/* init_sprites — place furniture and ghost on the map. */
static void init_sprites(void)
{
    sprite_count = 0;
    /* Room A (top-left) — facing: 0=N,1=E,2=S,3=W */
    sprites[sprite_count++] = (Sprite){2.5f, 2.5f, 0, 2}; /* lantern facing S */
    sprites[sprite_count++] = (Sprite){1.5f, 1.5f, 4, 2}; /* chair facing S */
    /* Room B (top-center) */
    sprites[sprite_count++] = (Sprite){7.5f, 2.0f, 5, 2}; /* table facing S */
    sprites[sprite_count++] = (Sprite){8.5f, 1.5f, 1, 2}; /* candle stand */
    /* Room C (top-right) */
    sprites[sprite_count++] = (Sprite){12.5f, 1.5f, 2, 0}; /* barrel */
    sprites[sprite_count++] = (Sprite){13.5f, 2.5f, 3, 0}; /* plant */
    /* Corridor */
    sprites[sprite_count++] = (Sprite){3.5f, 5.5f, 2, 0};  /* barrel */
    sprites[sprite_count++] = (Sprite){10.5f, 5.5f, 2, 0}; /* barrel */
    /* Room D (left) */
    sprites[sprite_count++] = (Sprite){2.5f, 9.0f, 5, 1};  /* table facing E */
    sprites[sprite_count++] = (Sprite){1.5f, 8.5f, 4, 1};  /* chair facing E */
    sprites[sprite_count++] = (Sprite){3.5f, 10.5f, 3, 0}; /* plant */
    /* Room E (right) */
    sprites[sprite_count++] = (Sprite){13.5f, 9.0f, 0, 3}; /* lantern facing W */
    sprites[sprite_count++] = (Sprite){12.5f, 8.5f, 4, 3}; /* chair facing W */
    /* Great hall */
    sprites[sprite_count++] = (Sprite){4.5f, 13.0f, 5, 0};  /* table */
    sprites[sprite_count++] = (Sprite){10.5f, 13.0f, 5, 0}; /* table */
    sprites[sprite_count++] = (Sprite){7.5f, 12.5f, 1, 0};  /* candle stand */
    sprites[sprite_count++] = (Sprite){2.5f, 14.0f, 2, 0};  /* barrel */
    sprites[sprite_count++] = (Sprite){13.0f, 14.0f, 3, 0}; /* plant */
    sprites[sprite_count++] = (Sprite){7.5f, 14.0f, 0, 0};  /* lantern */
    sprites[sprite_count++] = (Sprite){3.5f, 13.0f, 4, 1};  /* chair facing E */
    sprites[sprite_count++] = (Sprite){11.5f, 13.0f, 4, 3}; /* chair facing W */
    /* Ghost behind barred window [4][8] */
    ghost_sprite_idx = sprite_count;
    sprites[sprite_count++] = (Sprite){8.5f, 2.5f, 6, 2};  /* ghost facing S */
}

/* init_row_dist — precompute floor/ceiling distance LUT (Q8 fixed-point).
 * ROW_DIST_FP[d] = (HALF_H * 256) / d, where d = vertical distance from horizon. */
static void init_row_dist(void)
{
    int i;
    ROW_DIST_FP[0] = 0;
    for (i = 1; i < 120; i++)
        ROW_DIST_FP[i] = (120 << 8) / i;
}

/* init_doors — scan MAP for TILE_DOOR, populate door state arrays. */
static void init_doors(void)
{
    int y, x;
    for (y = 0; y < MAP_H; y++)
        for (x = 0; x < MAP_W; x++)
            door_map_idx[y][x] = -1;

    door_count = 0;
    for (y = 0; y < MAP_H && door_count < DOOR_MAX; y++)
        for (x = 0; x < MAP_W && door_count < DOOR_MAX; x++)
            if (MAP[y][x] == TILE_DOOR) {
                door_x[door_count] = x;
                door_y[door_count] = y;
                door_state[door_count] = 0;
                door_timer[door_count] = 0;
                door_map_idx[y][x] = door_count;
                door_count++;
            }
}

/* plot_pixel — write one RGB565 pixel to back buffer.
 * Row stride is 512 shorts (1024 bytes), so y offset = y << 9. */
static void plot_pixel(int x, int y, unsigned short c)
{
    *(back_buf + (y << 9) + x) = c;
}

/* wait_for_vsync — trigger buffer swap via VGA DMA, wait for completion,
 * then update back_buf pointer and ctrl[1] for the next frame. */
static void wait_for_vsync(void)
{
    volatile int *ctrl = (volatile int *) PIXEL_BUF_CTRL_BASE;
    ctrl[0] = 1;
    while (ctrl[3] & 1);
    buf_index ^= 1;
    back_buf = (volatile short *)(unsigned long)(buf_index ? 0x08000000 : 0x01000000);
    ctrl[1] = buf_index ? 0x08000000 : 0x01000000;
}

/* clear_back — zero the entire back buffer (320x240 black). */
static void clear_back(void)
{
    int x, y;
    for (y = 0; y < SCREEN_H; y++)
        for (x = 0; x < SCREEN_W; x++)
            plot_pixel(x, y, 0);
}

/* init_vga — set up double-buffered VGA (front=SRAM, back=SDRAM). */
static void init_vga(void)
{
    volatile int *ctrl = (volatile int *) PIXEL_BUF_CTRL_BASE;
    ctrl[1]  = 0x01000000;
    buf_index = 0;
    back_buf = (volatile short *) 0x01000000;
    clear_back();
    wait_for_vsync();
    clear_back();
}

/* clear_chars — zero the 80x60 character buffer overlay. */
static void clear_chars(void)
{
    volatile char *cb = (volatile char *) CHAR_BUF_BASE;
    int i;
    for (i = 0; i < 80 * 60; i++)
        cb[i] = 0;
}

/* write_string — print ASCII string at (col, row) in the character buffer. */
static void write_string(int col, int row, const char *str)
{
    volatile char *cb = (volatile char *) CHAR_BUF_BASE;
    int i = row * 80 + col;
    while (*str)
        cb[i++] = *str++;
}

/* clear_hud_line — blank one row of the character buffer. */
static void clear_hud_line(int row)
{
    volatile char *cb = (volatile char *) CHAR_BUF_BASE;
    int i, base = row * 80;
    for (i = 0; i < 80; i++)
        cb[base + i] = 0;
}

/* poll_ps2 — drain PS/2 FIFO, update key-held flags.
 * Scan codes use F0 prefix for break (release), E0 for extended keys. */
static void poll_ps2(void)
{
    volatile int *ps2 = (volatile int *) PS2_BASE;
    static int f0 = 0, e0 = 0;
    int data, sc;

    for (;;) {
        data = *ps2;
        if (!(data & 0x8000)) break;
        sc = data & 0xFF;

        if (sc == 0xF0) {
            f0 = 1;
        } else if (sc == 0xE0) {
            e0 = 1;
        } else {
            int p = !f0;
            if (!e0) {
                if      (sc == 0x1D) k_w     = p;
                else if (sc == 0x1B) k_s     = p;
                else if (sc == 0x1C) k_a     = p;
                else if (sc == 0x23) k_d     = p;
                else if (sc == 0x29) k_space = p;
            } else {
                if      (sc == 0x75) k_up = p;
                else if (sc == 0x72) k_dn = p;
                else if (sc == 0x6B) k_lt = p;
                else if (sc == 0x74) k_rt = p;
            }
            f0 = 0;
            e0 = 0;
        }
    }
}

/* find_door — return door index at tile (mx,my), or -1. */
static int find_door(int mx, int my)
{
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return -1;
    return door_map_idx[my][mx];
}

/* is_door_open — true if door at (mx,my) is fully open (state==2). */
static int is_door_open(int mx, int my)
{
    int di = find_door(mx, my);
    return (di >= 0 && door_state[di] == 2);
}

/* update_doors — advance door timers. States: 0=closed, 1=opening, 2=open, 3=closing. */
static void update_doors(void)
{
    int i;
    for (i = 0; i < door_count; i++) {
        if (door_state[i] == 1) {
            if (++door_timer[i] >= DOOR_ANIM_LEN) {
                door_timer[i] = DOOR_ANIM_LEN;
                door_state[i] = 2;
            }
        } else if (door_state[i] == 3) {
            if (--door_timer[i] <= 0) {
                door_timer[i] = 0;
                door_state[i] = 0;
            }
        }
    }
}

/* tile_passable — true if tile at (tx,ty) allows player movement. */
static int tile_passable(int tx, int ty)
{
    int t;
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= MAP_H) return 0;
    t = MAP[ty][tx];
    if (t == 0) return 1;
    if (t == TILE_DOOR) return is_door_open(tx, ty);
    return 0;
}

/* sprite_blocked — true if (x,y) overlaps any sprite (radius 0.3). */
static int sprite_blocked(float x, float y)
{
    int i;
    for (i = 0; i < sprite_count; i++) {
        float sdx = x - sprites[i].x;
        float sdy = y - sprites[i].y;
        if (sdx * sdx + sdy * sdy < 0.09f) return 1;
    }
    return 0;
}

/* walkable — AABB collision (COLL_M margin) against walls + sprite check. */
static int walkable(float x, float y)
{
    int x0 = (int)(x - COLL_M);
    int x1 = (int)(x + COLL_M);
    int y0 = (int)(y - COLL_M);
    int y1 = (int)(y + COLL_M);
    if (!tile_passable(x0, y0) || !tile_passable(x1, y0) ||
        !tile_passable(x0, y1) || !tile_passable(x1, y1))
        return 0;
    return !sprite_blocked(x, y);
}

/* rotate_player — apply 2D rotation matrix to direction and camera plane.
 * [dx']   [cos -sin] [dx]
 * [dy'] = [sin  cos] [dy]   (same for plx/ply) */
static void rotate_player(int right)
{
    float c = ROT_C;
    float s = right ? ROT_S : -ROT_S;
    float t;
    t   = dx;  dx  = dx  * c - dy  * s;  dy  = t  * s + dy  * c;
    t   = plx; plx = plx * c - ply * s;  ply = t  * s + ply * c;
}

/* update_player — process WASD/arrow movement, strafing, door interaction.
 * Movement uses axis-separated collision (try X then Y independently). */
static void update_player(void)
{
    float nx, ny;
    int i, triggered;

    if (k_w || k_up) {
        nx = px + dx * MOVE_SPEED;
        ny = py + dy * MOVE_SPEED;
        if (walkable(nx, py)) px = nx;
        if (walkable(px, ny)) py = ny;
    }
    if (k_s || k_dn) {
        nx = px - dx * MOVE_SPEED;
        ny = py - dy * MOVE_SPEED;
        if (walkable(nx, py)) px = nx;
        if (walkable(px, ny)) py = ny;
    }
    if (k_a) {
        nx = px +  dy * MOVE_SPEED;
        ny = py + (-dx) * MOVE_SPEED;
        if (walkable(nx, py)) px = nx;
        if (walkable(px, ny)) py = ny;
    }
    if (k_d) {
        nx = px + (-dy) * MOVE_SPEED;
        ny = py +   dx  * MOVE_SPEED;
        if (walkable(nx, py)) px = nx;
        if (walkable(px, ny)) py = ny;
    }
    if (k_lt) rotate_player(0);
    if (k_rt) rotate_player(1);

    /* Door interaction (space rising edge) */
    triggered = (k_space && !space_prev);
    space_prev = k_space;
    if (triggered) {
        for (i = 0; i < door_count; i++) {
            float ddx2 = px - (door_x[i] + 0.5f);
            float ddy2 = py - (door_y[i] + 0.5f);
            if (ddx2 * ddx2 + ddy2 * ddy2 < 2.25f) {
                if (door_state[i] == 0)
                    door_state[i] = 1;
                else if (door_state[i] == 2) {
                    int ptx = (int)px, pty = (int)py;
                    if (ptx != door_x[i] || pty != door_y[i])
                        door_state[i] = 3;
                }
                break;
            }
        }
    }
}

/* update_hud — show/hide "Press [SPACE]" prompt near doors. */
static void update_hud(void)
{
    int i, near = 0;
    for (i = 0; i < door_count; i++) {
        float ddx2 = px - (door_x[i] + 0.5f);
        float ddy2 = py - (door_y[i] + 0.5f);
        if (ddx2 * ddx2 + ddy2 * ddy2 < 2.25f) {
            near = 1;
            break;
        }
    }
    if (near && !hud_visible) {
        write_string(33, 5, "Press [SPACE]");
        hud_visible = 1;
    } else if (!near && hud_visible) {
        clear_hud_line(5);
        hud_visible = 0;
    }
}

/*
 * cast_rays — DDA raycaster rendering walls, textured floor/ceiling, windows.
 *
 * Per column: cast ray via DDA, compute perpendicular distance
 *   perp = (side==0) ? (sdx - ddx) : (sdy - ddy)
 * Wall height: lh = SCREEN_H / perp
 * Fog: brightness = face_bright * (FOG_MAX - perp_q8) >> 11
 * Floor/ceiling at half horizontal resolution using ROW_DIST_FP LUT.
 * Door split animation: gap_half = timer * 8 / DOOR_ANIM_LEN, rays pass
 *   through center tex columns, remaining panels shift outward.
 */
static void cast_rays(void)
{
    int x, y, i;
    int flicker_bright;
    int px_fp, py_fp;
    int light_x_fp[MAX_LIGHTS], light_y_fp[MAX_LIGHTS];

    flicker_bright = 200 + ((((unsigned int)(frame_count >> 1) * 1103515245u
                              + 12345u) >> 16) & 0x3F);
    if (flicker_bright > 255) flicker_bright = 255;

    px_fp = (int)(px * 256.0f);
    py_fp = (int)(py * 256.0f);

    for (i = 0; i < light_count; i++) {
        light_x_fp[i] = (int)(light_x[i] * 256.0f);
        light_y_fp[i] = (int)(light_y[i] * 256.0f);
    }

    for (x = 0; x < SCREEN_W; x++) {

        float cam, rdx, rdy, ddx_f, ddy_f, sdx, sdy, perp, wall_x;
        int mx, my, sx, sy, side, wtype, tile;
        int lh, wall_t, wall_b, door_idx;
        int tex_x, tex_idx, face, perp_q8, ao, bright;
        int torch_r, torch_g;
        int rdx_fp, rdy_fp;
        int ws, we, vis;
        int hit_x_fp, hit_y_fp;
        int grey;

        /* Window state */
        int win_hit = 0, win_side = 0;
        float win_perp = 0;

        cam  = 2.0f * (float)x / (float)(SCREEN_W - 1) - 1.0f;
        rdx  = dx  + plx * cam;
        rdy  = dy  + ply * cam;

        mx = (int)px;
        my = (int)py;

        ddx_f = (rdx == 0.0f) ? 1e30f : FABS(1.0f / rdx);
        ddy_f = (rdy == 0.0f) ? 1e30f : FABS(1.0f / rdy);

        if (rdx < 0.0f) { sx = -1; sdx = (px - (float)mx)       * ddx_f; }
        else             { sx =  1; sdx = ((float)(mx + 1) - px) * ddx_f; }

        if (rdy < 0.0f) { sy = -1; sdy = (py - (float)my)       * ddy_f; }
        else             { sy =  1; sdy = ((float)(my + 1) - py) * ddy_f; }

        side  = 0;
        wtype = 0;

        /* DDA march */
        while (!wtype) {
            if (sdx < sdy) { sdx += ddx_f; mx += sx; side = 0; }
            else            { sdy += ddy_f; my += sy; side = 1; }

            if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) {
                wtype = 1;
                break;
            }

            tile = MAP[my][mx];
            if (tile == TILE_DOOR && is_door_open(mx, my))
                continue;
            /* Split-open: ray passes through gap columns */
            if (tile == TILE_DOOR) {
                int di = find_door(mx, my);
                if (di >= 0 && door_timer[di] > 0) {
                    float dp = (side == 0) ? (sdx - ddx_f) : (sdy - ddy_f);
                    float wx = (side == 0) ? (py + dp * rdy) : (px + dp * rdx);
                    int dtx, gh;
                    wx -= (int)wx;
                    dtx = (int)(wx * 16.0f) & 0x0F;
                    gh = (door_timer[di] * 8) / DOOR_ANIM_LEN;
                    if (dtx >= (8 - gh) && dtx < (8 + gh))
                        continue;
                }
            }
            if (tile == TILE_WINDOW && !win_hit) {
                win_hit = 1;
                win_side = side;
                win_perp = (side == 0) ? (sdx - ddx_f) : (sdy - ddy_f);
                if (win_perp < 0.001f) win_perp = 0.001f;
                continue;
            }
            if (tile > 0)
                wtype = tile;
        }

        perp = (side == 0) ? (sdx - ddx_f) : (sdy - ddy_f);
        if (perp < 0.001f) perp = 0.001f;
        z_buf[x] = perp;

        /* Ray hit endpoint for minimap overlay */
        if (side == 0) {
            ray_hx[x] = (sx > 0) ? (float)mx : (float)(mx + 1);
            ray_hy[x] = py + perp * rdy;
        } else {
            ray_hx[x] = px + perp * rdx;
            ray_hy[x] = (sy > 0) ? (float)my : (float)(my + 1);
        }

        lh     = (int)((float)SCREEN_H / perp);
        wall_t = HALF_H - lh / 2;
        wall_b = HALF_H + lh / 2 - 1;

        /* Door split animation lookup */
        door_idx = -1;
        if (wtype == TILE_DOOR) {
            door_idx = find_door(mx, my);
        }

        /* Texture U coordinate */
        if (side == 0)
            wall_x = py + perp * rdy;
        else
            wall_x = px + perp * rdx;
        wall_x -= (int)wall_x;
        tex_x = (int)(wall_x * 16.0f) & 0x0F;

        /* Shift tex_x outward for split-open effect */
        if (door_idx >= 0 && door_timer[door_idx] > 0) {
            int gh = (door_timer[door_idx] * 8) / DOOR_ANIM_LEN;
            if (tex_x < 8) tex_x -= gh;
            else           tex_x += gh;
            tex_x &= 0x0F;
        }

        if (wtype >= 1 && wtype <= 6)
            tex_idx = WALL_TEX[wtype];
        else
            tex_idx = 0;

        if (side == 0) face = (sx > 0) ? 3 : 2;
        else           face = (sy > 0) ? 0 : 1;

        perp_q8 = (int)(perp * 256.0f);

        ao = 0;
        if (mx > 0         && MAP[my][mx-1] > 0 && MAP[my][mx-1] != TILE_DOOR) ao++;
        if (mx < MAP_W - 1 && MAP[my][mx+1] > 0 && MAP[my][mx+1] != TILE_DOOR) ao++;
        if (my > 0         && MAP[my-1][mx] > 0 && MAP[my-1][mx] != TILE_DOOR) ao++;
        if (my < MAP_H - 1 && MAP[my+1][mx] > 0 && MAP[my+1][mx] != TILE_DOOR) ao++;

        {
            int fog = FOG_MAX_Q8 - perp_q8;
            if (fog < 0) fog = 0;
            bright = (FACE_BRIGHT[face] * fog) >> 11;
            bright -= ao * 12;
            if (bright < 0) bright = 0;
            if (bright > 255) bright = 255;
        }

        rdx_fp = (int)(rdx * 256.0f);
        rdy_fp = (int)(rdy * 256.0f);

        /* Grey fog amount for this column distance */
        grey = perp_q8 >> 8;
        if (grey > 8) grey = 8;

        /* Torch contribution */
        torch_r = 0;
        torch_g = 0;
        if (side == 0) {
            hit_x_fp = (sx > 0) ? (mx << 8) : ((mx + 1) << 8);
            hit_y_fp = py_fp + ((perp_q8 * rdy_fp) >> 8);
        } else {
            hit_x_fp = px_fp + ((perp_q8 * rdx_fp) >> 8);
            hit_y_fp = (sy > 0) ? (my << 8) : ((my + 1) << 8);
        }
        for (i = 0; i < light_count; i++) {
            int ldx2 = (hit_x_fp - light_x_fp[i]) >> 4;
            int ldy2 = (hit_y_fp - light_y_fp[i]) >> 4;
            int dsq = ldx2 * ldx2 + ldy2 * ldy2;
            if (dsq < LIGHT_RADIUS_SQ_FP) {
                int add = 12 * (LIGHT_RADIUS_SQ_FP - dsq) / LIGHT_RADIUS_SQ_FP;
                add = (add * flicker_bright) >> 8;
                torch_r += add;
                torch_g += add >> 1;
            }
        }
        if (torch_r > 15) torch_r = 15;
        if (torch_g > 15) torch_g = 15;

        /* Ceiling (half-res: 2 pixels per iteration) */
        if ((x & 1) == 0) {
            int ceil_end = (wall_t < 0) ? 0 : wall_t;
            for (y = 0; y < ceil_end; y++) {
                int d = HALF_H - y;
                int rd, fx, fy, tx, ty;
                int cfog, cb, r, g, b, cg;
                unsigned short c, col;
                if (d < 1 || d >= 120) {
                    plot_pixel(x, y, 0);
                    if (x + 1 < SCREEN_W) plot_pixel(x + 1, y, 0);
                    continue;
                }
                rd = ROW_DIST_FP[d];
                if (rd > 1720) {
                    unsigned short fogcol = RGB565(3,6,3);
                    plot_pixel(x, y, fogcol);
                    if (x + 1 < SCREEN_W) plot_pixel(x + 1, y, fogcol);
                    continue;
                }
                fx = px_fp + ((rd * rdx_fp) >> 8);
                fy = py_fp + ((rd * rdy_fp) >> 8);
                tx = (((unsigned)fx) >> 4) & 0x0F;
                ty = (((unsigned)fy) >> 4) & 0x0F;
                c = TEX[5][ty][tx];
                cfog = FOG_MAX_Q8 - rd;
                cb = (cfog * 200) >> 11;
                if (cb > 200) cb = 200;
                r = (((c >> 11) & 0x1F) * cb) >> 8;
                g = (((c >>  5) & 0x3F) * cb) >> 8;
                b = (( c        & 0x1F) * cb) >> 8;
                /* Grey fog */
                cg = rd >> 8; if (cg > 8) cg = 8;
                r += cg; if (r > 31) r = 31;
                g += cg * 2; if (g > 63) g = 63;
                b += cg; if (b > 31) b = 31;
                col = (unsigned short)((r << 11) | (g << 5) | b);
                plot_pixel(x, y, col);
                if (x + 1 < SCREEN_W) plot_pixel(x + 1, y, col);
            }
        }

        /* Wall column */
        ws = (wall_t < 0) ? 0 : wall_t;
        we = (wall_b >= SCREEN_H) ? SCREEN_H - 1 : wall_b;
        if (lh > 0) {
            int tex_step = (16 << 16) / lh;
            int tex_pos = (wall_t < 0) ? (-wall_t) * tex_step : 0;
            for (y = ws; y <= we; y++) {
                int tex_y = (tex_pos >> 16) & 0x0F;
                unsigned short c = TEX[tex_idx][tex_y][tex_x];
                int r = (c >> 11) & 0x1F;
                int g = (c >>  5) & 0x3F;
                int b =  c        & 0x1F;
                r = (r * bright) >> 8;
                g = (g * bright) >> 8;
                b = (b * bright) >> 8;
                r += torch_r; if (r > 31) r = 31;
                g += torch_g; if (g > 63) g = 63;
                /* Grey fog */
                r += grey; if (r > 31) r = 31;
                g += grey * 2; if (g > 63) g = 63;
                b += grey; if (b > 31) b = 31;
                plot_pixel(x, y, (unsigned short)((r << 11) | (g << 5) | b));
                tex_pos += tex_step;
            }
        }

        /* Window bar overlay */
        if (win_hit) {
            int wlh = (int)((float)SCREEN_H / win_perp);
            int wwt = HALF_H - wlh / 2;
            int wwb = HALF_H + wlh / 2 - 1;
            float wwall_x;
            int wtex_x, is_bar;
            if (win_side == 0) wwall_x = py + win_perp * rdy;
            else               wwall_x = px + win_perp * rdx;
            wwall_x -= (int)wwall_x;
            wtex_x = (int)(wwall_x * 16.0f) & 0x0F;
            /* Bars at tex positions 1,2, 5,6, 9,10, 13,14 */
            is_bar = ((wtex_x & 3) == 1 || (wtex_x & 3) == 2);
            if (is_bar) {
                int wws = (wwt < 0) ? 0 : wwt;
                int wwe = (wwb >= SCREEN_H) ? SCREEN_H - 1 : wwb;
                int wfq = (int)(win_perp * 256.0f);
                int wfog = FOG_MAX_Q8 - wfq;
                int wbr, wgr;
                if (wfog < 0) wfog = 0;
                wbr = (wfog * 180) >> 11;
                wgr = wfq >> 8; if (wgr > 8) wgr = 8;
                for (y = wws; y <= wwe; y++) {
                    int r = (8 * wbr) >> 8;
                    int g = (16 * wbr) >> 8;
                    int b = (8 * wbr) >> 8;
                    r += wgr; if (r > 31) r = 31;
                    g += wgr * 2; if (g > 63) g = 63;
                    b += wgr; if (b > 31) b = 31;
                    plot_pixel(x, y, (unsigned short)((r << 11) | (g << 5) | b));
                }
            }
        }

        /* Floor (half-res: 2 pixels per iteration) */
        if ((x & 1) == 0) {
            int floor_start = (wall_b >= SCREEN_H - 1) ? SCREEN_H : wall_b + 1;
            for (y = floor_start; y < SCREEN_H; y++) {
                int d = y - HALF_H;
                int rd, fx, fy, tx, ty;
                int cfog, fb, r, g, b, fg;
                unsigned short c, col;
                if (d < 1 || d >= 120) {
                    plot_pixel(x, y, 0);
                    if (x + 1 < SCREEN_W) plot_pixel(x + 1, y, 0);
                    continue;
                }
                rd = ROW_DIST_FP[d];
                if (rd > 1720) {
                    unsigned short fogcol = RGB565(3,6,3);
                    plot_pixel(x, y, fogcol);
                    if (x + 1 < SCREEN_W) plot_pixel(x + 1, y, fogcol);
                    continue;
                }
                fx = px_fp + ((rd * rdx_fp) >> 8);
                fy = py_fp + ((rd * rdy_fp) >> 8);
                tx = (((unsigned)fx) >> 4) & 0x0F;
                ty = (((unsigned)fy) >> 4) & 0x0F;
                c = TEX[4][ty][tx];
                cfog = FOG_MAX_Q8 - rd;
                fb = (cfog * 220) >> 11;
                if (fb > 220) fb = 220;
                r = (((c >> 11) & 0x1F) * fb) >> 8;
                g = (((c >>  5) & 0x3F) * fb) >> 8;
                b = (( c        & 0x1F) * fb) >> 8;
                /* Grey fog */
                fg = rd >> 8; if (fg > 8) fg = 8;
                r += fg; if (r > 31) r = 31;
                g += fg * 2; if (g > 63) g = 63;
                b += fg; if (b > 31) b = 31;
                col = (unsigned short)((r << 11) | (g << 5) | b);
                plot_pixel(x, y, col);
                if (x + 1 < SCREEN_W) plot_pixel(x + 1, y, col);
            }
        }
    }
}

/* sprite_tex_for_view — map sprite type + relative view angle to STEX index.
 * Symmetric types always return one texture; chair/table select by angle. */
static int sprite_tex_for_view(int type, int view)
{
    switch (type) {
        case 0: return 0;  /* lantern — symmetric */
        case 1: return 1;  /* candle — symmetric */
        case 2: return 2;  /* barrel — symmetric */
        case 3: return 3;  /* plant — symmetric */
        case 4:            /* chair: front=4, side=5, back=6 */
            if (view == 0) return 4;
            if (view == 2) return 6;
            return 5;
        case 5:            /* table: short=7, long=8 */
            return (view & 1) ? 8 : 7;
        case 6: return 9;  /* ghost — symmetric */
    }
    return 0;
}

/*
 * draw_sprites — billboarded sprite renderer, drawn far-to-near.
 * Camera transform: inv_det = 1 / (plx*dy - dx*ply)
 *   tr_x = inv_det * (dy*spx - dx*spy)
 *   tr_y = inv_det * (-ply*spx + plx*spy)
 * Screen X: (W/2) * (1 + tr_x/tr_y). Z-buffer occlusion per column.
 * Ghost uses dithered fade (noise threshold increases over 10 frames).
 */
static void draw_sprites(void)
{
    int i, j, order[MAX_SPRITES];
    float dist[MAX_SPRITES];
    float inv_det;

    for (i = 0; i < sprite_count; i++) {
        float sx2 = sprites[i].x - px;
        float sy2 = sprites[i].y - py;
        dist[i] = sx2 * sx2 + sy2 * sy2;
        order[i] = i;
    }

    /* Insertion sort far-to-near */
    for (i = 1; i < sprite_count; i++) {
        int key = order[i];
        float kd = dist[key];
        j = i - 1;
        while (j >= 0 && dist[order[j]] < kd) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    inv_det = 1.0f / (plx * dy - dx * ply);

    for (i = 0; i < sprite_count; i++) {
        int idx = order[i];
        if (idx == ghost_sprite_idx && !ghost_active) continue;
        float spx_f = sprites[idx].x - px;
        float spy_f = sprites[idx].y - py;
        float tr_x, tr_y;
        int scr_x, full_sh, sh, sw;
        int draw_sx, draw_ex, draw_sy, draw_ey;
        int tex_id, stripe, type, fog_q8, fog_b, sp_grey;
        int view_abs, view_rel, mirror;

        tr_x = inv_det * (dy * spx_f - dx * spy_f);
        tr_y = inv_det * (-ply * spx_f + plx * spy_f);

        if (tr_y <= 0.1f) continue;

        fog_q8 = (int)(tr_y * 256.0f);
        if (fog_q8 > 1720) continue;
        {
            int f = FOG_MAX_Q8 - fog_q8;
            fog_b = (f * 220) >> 11;
            if (fog_b < 0) fog_b = 0;
            if (fog_b > 220) fog_b = 220;
        }
        sp_grey = fog_q8 >> 8;
        if (sp_grey > 8) sp_grey = 8;

        type = sprites[idx].type;

        /* Determine which face the player sees */
        {
            float ax = (spx_f > 0) ? spx_f : -spx_f;
            float ay = (spy_f > 0) ? spy_f : -spy_f;
            if (ay >= ax)
                view_abs = (spy_f < 0) ? 2 : 0;
            else
                view_abs = (spx_f > 0) ? 3 : 1;
        }
        view_rel = (view_abs - sprites[idx].facing + 4) & 3;

        /* Mirror for left-side views */
        mirror = 0;
        if (type == 4 && view_rel == 3) { view_rel = 1; mirror = 1; }
        if (type == 5 && view_rel == 2) { mirror = 1; view_rel = 0; }
        if (type == 5 && view_rel == 3) { mirror = 1; view_rel = 1; }

        tex_id = sprite_tex_for_view(type, view_rel);

        scr_x = (int)((float)(SCREEN_W / 2) * (1.0f + tr_x / tr_y));
        full_sh = (int)((float)SCREEN_H / tr_y);
        sh = full_sh * SPRITE_SCALE[type] / 10;
        if (sh > SCREEN_H * 2) sh = SCREEN_H * 2;
        if (sh < 1) continue;
        sw = sh;

        /* Align bottom with floor */
        draw_ey = HALF_H + full_sh / 2 - 1;
        draw_sy = draw_ey - sh + 1;
        draw_sx = scr_x - sw / 2;
        draw_ex = draw_sx + sw - 1;

        {
            int tex_step_v = (16 << 16) / sh;

            for (stripe = draw_sx; stripe <= draw_ex; stripe++) {
                int tex_u, tex_pos_v, py2;
                int ys, ye;

                if (stripe < 0 || stripe >= SCREEN_W) continue;
                if (tr_y >= z_buf[stripe]) continue;

                tex_u = ((stripe - draw_sx) * 16) / sw;
                if (tex_u < 0) tex_u = 0;
                if (tex_u > 15) tex_u = 15;
                if (mirror) tex_u = 15 - tex_u;

                ys = (draw_sy < 0) ? 0 : draw_sy;
                ye = (draw_ey >= SCREEN_H) ? SCREEN_H - 1 : draw_ey;
                tex_pos_v = (draw_sy < 0) ? (-draw_sy) * tex_step_v : 0;

                for (py2 = ys; py2 <= ye; py2++) {
                    int tex_v = (tex_pos_v >> 16) & 0x0F;
                    unsigned short c = STEX[tex_id][tex_v][tex_u];
                    if (c != 0) {
                        /* Ghost dithered fade */
                        if (idx == ghost_sprite_idx && ghost_fade > 0) {
                            int thr = ghost_fade * 25;
                            int noise = ((stripe * 7 + py2 * 13 + frame_count) & 0xFF);
                            if (noise < thr) { tex_pos_v += tex_step_v; continue; }
                        }
                        {
                        int r = ((c >> 11) & 0x1F) * fog_b >> 8;
                        int g = ((c >>  5) & 0x3F) * fog_b >> 8;
                        int b = ( c        & 0x1F) * fog_b >> 8;
                        r += sp_grey; if (r > 31) r = 31;
                        g += sp_grey * 2; if (g > 63) g = 63;
                        b += sp_grey; if (b > 31) b = 31;
                        plot_pixel(stripe, py2,
                            (unsigned short)((r << 11) | (g << 5) | b));
                        }
                    }
                    tex_pos_v += tex_step_v;
                }
            }
        }
    }
}

/* draw_torch_hand — first-person torch overlay: handle, flame, hand, forearm.
 * Flame height flickers with frame_count. Hand bobs when moving. */
static void draw_torch_hand(void)
{
    int moving = k_w || k_s || k_a || k_d || k_up || k_dn;
    int bob = 0, bx = 265, by = 178;
    int tx, ty;

    if (moving) {
        int p = (frame_count >> 1) & 7;
        bob = (p < 4) ? p - 2 : 6 - p;
    }
    by += bob;

    /* Handle: dark brown vertical bar */
    for (ty = by; ty < by + 45 && ty < SCREEN_H; ty++)
        for (tx = bx; tx < bx + 5 && tx < SCREEN_W; tx++)
            if (ty >= 0 && tx >= 0)
                plot_pixel(tx, ty, RGB565(7, 12, 3));

    /* Wrapping bands */
    for (tx = bx - 1; tx < bx + 6 && tx < SCREEN_W; tx++) {
        if (tx < 0) continue;
        if (by + 2 >= 0 && by + 2 < SCREEN_H) plot_pixel(tx, by + 2, RGB565(10, 18, 5));
        if (by + 4 >= 0 && by + 4 < SCREEN_H) plot_pixel(tx, by + 4, RGB565(10, 18, 5));
    }

    /* Flame — flickers with frame_count */
    {
        int fh = 9 + (((frame_count >> 1) * 3) & 3);
        for (ty = by - fh; ty < by; ty++) {
            int fy, fw;
            if (ty < 0 || ty >= SCREEN_H) continue;
            fy = by - ty;
            fw = (fy > 7) ? 1 : (fy > 4) ? 2 : 3;
            for (tx = bx + 2 - fw; tx <= bx + 2 + fw; tx++) {
                unsigned short fc;
                if (tx < 0 || tx >= SCREEN_W) continue;
                if (fy > 6) fc = RGB565(31, 63, 12);
                else if (fy > 3) fc = RGB565(31, 55, 5);
                else fc = RGB565(28, 42, 3);
                plot_pixel(tx, ty, fc);
            }
        }
    }

    /* Forearm — extends from handle down to bottom-right corner */
    for (ty = by + 30; ty < SCREEN_H; ty++) {
        int row = ty - (by + 30);
        int arm_l = bx - 3 + row / 3;
        int arm_r = bx + 8 + row;
        if (arm_r >= SCREEN_W) arm_r = SCREEN_W - 1;
        for (tx = arm_l; tx <= arm_r; tx++)
            if (tx >= 0)
                plot_pixel(tx, ty, RGB565(11, 18, 6));
    }

    /* Wrist shading — darker strip along top of forearm */
    for (tx = bx - 2; tx < bx + 12 && tx < SCREEN_W; tx++) {
        if (tx < 0) continue;
        if (by + 30 >= 0 && by + 30 < SCREEN_H) plot_pixel(tx, by + 30, RGB565(8, 14, 4));
        if (by + 31 >= 0 && by + 31 < SCREEN_H) plot_pixel(tx, by + 31, RGB565(9, 15, 5));
    }

    /* Hand — wrapping around handle */
    for (ty = by + 24; ty < by + 36 && ty < SCREEN_H; ty++) {
        if (ty < 0) continue;
        /* Left fingers (wrap in front of handle) */
        for (tx = bx - 5; tx < bx && tx < SCREEN_W; tx++)
            if (tx >= 0)
                plot_pixel(tx, ty, RGB565(12, 20, 7));
        /* Right side (thumb area) */
        for (tx = bx + 5; tx < bx + 10 && tx < SCREEN_W; tx++)
            if (tx >= 0)
                plot_pixel(tx, ty, RGB565(11, 18, 6));
    }

    /* Finger segments — knuckle lines across the grip */
    {
        int seg;
        for (seg = 0; seg < 3; seg++) {
            int fy = by + 25 + seg * 4;
            if (fy >= 0 && fy < SCREEN_H)
                for (tx = bx - 5; tx < bx + 1 && tx < SCREEN_W; tx++)
                    if (tx >= 0)
                        plot_pixel(tx, fy, RGB565(8, 14, 4));
        }
    }

    /* Thumb highlight */
    for (ty = by + 26; ty < by + 33 && ty < SCREEN_H; ty++) {
        if (ty < 0) continue;
        if (bx + 6 >= 0 && bx + 6 < SCREEN_W)
            plot_pixel(bx + 6, ty, RGB565(13, 22, 8));
    }
}

/* draw_crosshair — 5-pixel plus sign at screen center. */
static void draw_crosshair(void)
{
    int cx = SCREEN_W / 2, cy = HALF_H;
    plot_pixel(cx,   cy,   COL_CROSS);
    plot_pixel(cx-2, cy,   COL_CROSS);
    plot_pixel(cx+2, cy,   COL_CROSS);
    plot_pixel(cx,   cy-2, COL_CROSS);
    plot_pixel(cx,   cy+2, COL_CROSS);
}

/* draw_raycast_view — top-down minimap with Bresenham ray lines.
 * Draws every 8th ray column from player to hit endpoint. */
static void draw_raycast_view(void)
{
    int mx, my, tx, ty, i;
    int px_rv, py_rv;
    unsigned short c;
    int t;

    /* Map background */
    for (my = 0; my < MAP_H; my++)
        for (mx = 0; mx < MAP_W; mx++) {
            t = MAP[my][mx];
            if (t == 0)               c = RGB565(1, 3, 1);
            else if (t == TILE_DOOR)  c = RGB565(8, 16, 4);
            else if (t == TILE_WINDOW) c = RGB565(4, 10, 5);
            else                      c = RGB565(5, 10, 4);
            for (ty = 0; ty < RV_TILE; ty++)
                for (tx = 0; tx < RV_TILE; tx++)
                    plot_pixel(RV_X + mx * RV_TILE + tx,
                               RV_Y + my * RV_TILE + ty, c);
        }

    /* Draw rays — every 8th column, Bresenham lines */
    px_rv = (int)(px * (float)RV_TILE);
    py_rv = (int)(py * (float)RV_TILE);

    for (i = 0; i < SCREEN_W; i += 8) {
        int x0 = px_rv, y0 = py_rv;
        int x1 = (int)(ray_hx[i] * (float)RV_TILE);
        int y1 = (int)(ray_hy[i] * (float)RV_TILE);
        int sdx = (x1 > x0) ? 1 : -1;
        int sdy = (y1 > y0) ? 1 : -1;
        int adx = (x1 - x0) * sdx;
        int ady = (y1 - y0) * sdy;
        int err = adx - ady;
        int cx2 = x0, cy2 = y0;
        int steps = 0;

        while (steps < 200) {
            if (cx2 >= 0 && cx2 < RV_W && cy2 >= 0 && cy2 < RV_H)
                plot_pixel(RV_X + cx2, RV_Y + cy2, RGB565(18, 36, 8));
            if (cx2 == x1 && cy2 == y1) break;
            {
                int e2 = err * 2;
                if (e2 > -ady) { err -= ady; cx2 += sdx; }
                if (e2 <  adx) { err += adx; cy2 += sdy; }
            }
            steps++;
        }
    }

    /* Player dot */
    if (px_rv >= 0 && px_rv < RV_W && py_rv >= 0 && py_rv < RV_H)
        plot_pixel(RV_X + px_rv, RV_Y + py_rv, RGB565(31, 63, 0));

    /* Border */
    for (i = 0; i < RV_W; i++) {
        plot_pixel(RV_X + i, RV_Y, COL_MM_BDR);
        plot_pixel(RV_X + i, RV_Y + RV_H - 1, COL_MM_BDR);
    }
    for (i = 0; i < RV_H; i++) {
        plot_pixel(RV_X, RV_Y + i, COL_MM_BDR);
        plot_pixel(RV_X + RV_W - 1, RV_Y + i, COL_MM_BDR);
    }
}

/* main — init peripherals, run game loop: input, physics, render, vsync. */
int main(void)
{
    init_vga();
    init_textures();
    init_sprite_textures();
    init_sprites();
    init_row_dist();
    init_doors();
    clear_chars();

    for (;;) {
        frame_count++;
        poll_ps2();
        update_player();
        update_doors();
        update_hud();

        /* Camera bob when moving */
        {
            int moving = k_w || k_s || k_a || k_d || k_up || k_dn;
            if (moving) {
                int p = (frame_count >> 1) & 7;
                int bob = (p < 4) ? (p - 2) * 2 : (6 - p) * 2; /* -4 to +4 */
                HALF_H = HALF_H_BASE + bob;
            } else {
                HALF_H = HALF_H_BASE;
            }
        }

        /* Ghost fade near barred window */
        if (ghost_active) {
            if (ghost_fade == 0) {
                float wdx = px - 8.5f;
                float wdy = py - 4.5f;
                if (wdx * wdx + wdy * wdy < 2.5f)
                    ghost_fade = 1;
            }
            if (ghost_fade > 0 && ghost_fade <= 10) ghost_fade++;
            if (ghost_fade > 10) {
                ghost_active = 0;
                sprites[ghost_sprite_idx].x = -10.0f;
                sprites[ghost_sprite_idx].y = -10.0f;
            }
        }

        cast_rays();
        draw_sprites();
        draw_raycast_view();
        draw_crosshair();
        draw_torch_hand();
        wait_for_vsync();
    }

    return 0;
}
