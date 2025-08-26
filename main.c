#include "support/gcc8_c_support.h"
#include <exec/types.h>
#include <exec/exec.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <hardware/custom.h>
#include <hardware/intbits.h>
#include <hardware/dmabits.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/dos.h>

// Global variables
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
volatile struct Custom *custom;
 
struct GfxBase *GfxBase = NULL;
struct ViewPort *viewPort = NULL;
struct RasInfo rasInfo;
 
UWORD *copperPtr = NULL;

static UWORD SystemInts;
static UWORD SystemDMA;
static UWORD SystemADKCON;
static volatile APTR VBR = 0;
static APTR SystemIrq;
struct View *ActiView;

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 256
#define BPL_DEPTH 4
#define BPL_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 8) // Bytes per bitplane
#define BYTES_PER_ROW (SCREEN_WIDTH / 8)  // 40 bytes per row
#define NUM_COLORS (1 << BPL_DEPTH)

/* write definitions for dmaconw */
#define DMAF_SETCLR  0x8000
#define DMAF_AUDIO   0x000F   /* 4 bit mask */
#define DMAF_AUD0    0x0001
#define DMAF_AUD1    0x0002
#define DMAF_AUD2    0x0004
#define DMAF_AUD3    0x0008
#define DMAF_DISK    0x0010
#define DMAF_SPRITE  0x0020
#define DMAF_BLITTER 0x0040
#define DMAF_COPPER  0x0080
#define DMAF_RASTER  0x0100
#define DMAF_MASTER  0x0200
#define DMAF_BLITHOG 0x0400
#define DMAF_ALL     0x01FF   /* all dma channels */

// Global variables
UBYTE *bitplanes = NULL;
USHORT *copperList = NULL;
struct Interrupt vblankInt;
BOOL running = TRUE;
 
__attribute__((always_inline)) inline short MouseLeft() { return !((*(volatile UBYTE*)0xbfe001) & 64); }
__attribute__((always_inline)) inline short MouseRight() { return !((*(volatile UWORD*)0xdff016) & (1 << 10)); }


// set up a 320x256 lowres display
__attribute__((always_inline)) inline USHORT* screenScanDefault(USHORT* copListEnd) {
	const USHORT x=129;
	const USHORT width=320;
	const USHORT height=256;
	const USHORT y=44;
	const USHORT RES=8; //8=lowres,4=hires
	USHORT xstop = x+width;
	USHORT ystop = y+height;
	USHORT fw=(x>>1)-RES;

	*copListEnd++ = offsetof(struct Custom, ddfstrt);
	*copListEnd++ = fw;
	*copListEnd++ = offsetof(struct Custom, ddfstop);
	*copListEnd++ = fw+(((width>>4)-1)<<3);
	*copListEnd++ = offsetof(struct Custom, diwstrt);
	*copListEnd++ = x+(y<<8);
	*copListEnd++ = offsetof(struct Custom, diwstop);
	*copListEnd++ = (xstop-256)+((ystop-256)<<8);
	return copListEnd;
}

__attribute__((always_inline)) inline USHORT* copSetPlanes(UBYTE bplPtrStart,USHORT* copListEnd,const UBYTE **planes,int numPlanes) {
	for (USHORT i=0;i<numPlanes;i++) {
		ULONG addr=(ULONG)planes[i];
		*copListEnd++=offsetof(struct Custom, bplpt[0]) + (i + bplPtrStart) * sizeof(APTR);
		*copListEnd++=(UWORD)(addr>>16);
		*copListEnd++=offsetof(struct Custom, bplpt[0]) + (i + bplPtrStart) * sizeof(APTR) + 2;
		*copListEnd++=(UWORD)addr;
	}
	return copListEnd;
}

__attribute__((always_inline)) inline USHORT* copSetColor(USHORT* copListCurrent,USHORT index,USHORT color) {
	*copListCurrent++=offsetof(struct Custom, color) + sizeof(UWORD) * index;
	*copListCurrent++=color;
	return copListCurrent;
}

INCBIN(banner_col, "images/banner.PAL")
INCBIN_CHIP(banner, "images/banner.BPL") // load image into chipmem so we can use it without scopying
 
void WaitLine(USHORT line) 
{
	while (1) 
    {
		volatile ULONG vpos=*(volatile ULONG*)0xDFF004;
		if(((vpos >> 8) & 511) == line)
			break;
	}
}

static APTR GetVBR(void) {
    APTR vbr = 0;
    UWORD getvbr[] = { 0x4e7a, 0x0801, 0x4e73 }; // MOVEC.L VBR,D0 RTE
    if (SysBase->AttnFlags & AFF_68010)
        vbr = (APTR)Supervisor((ULONG (*)())getvbr);
    return vbr;
}

void SetInterruptHandler(APTR interrupt) {
    *(volatile APTR*)(((UBYTE*)VBR) + 0x6c) = interrupt;
}

APTR GetInterruptHandler() {
    return *(volatile APTR*)(((UBYTE*)VBR) + 0x6c);
}

void WaitVbl() {
    while (1) {
        volatile ULONG vpos = *(volatile ULONG*)0xdff004;
        vpos &= 0x1ff00;
        if (vpos != (311 << 8)) break;
    }
    while (1) {
        volatile ULONG vpos = *(volatile ULONG*)0xdff004;
        vpos &= 0x1ff00;
        if (vpos == (311 << 8)) break;
    }
}
 
void TakeSystem() 
{
	Forbid();
	//Save current interrupts and DMA settings so we can restore them upon exit. 
	SystemADKCON=custom->adkconr;
	SystemInts=custom->intenar;
	SystemDMA=custom->dmaconr;
	ActiView=GfxBase->ActiView; //store current view

	LoadView(0);
	WaitTOF();
	WaitTOF();

	WaitVbl();
	WaitVbl();

	OwnBlitter();
	WaitBlit();	
	Disable();
	
	custom->intena=0x7fff;//disable all interrupts
	custom->intreq=0x7fff;//Clear any interrupts that were pending
	
	custom->dmacon=0x7fff;//Clear all DMA channels

	//set all colors black
	for(int a=0;a<32;a++)
		custom->color[a]=0;

	WaitVbl();
	WaitVbl();

	VBR=GetVBR();
	SystemIrq=GetInterruptHandler(); //store interrupt register
}

void FreeSystem()
{ 
	WaitVbl();
	WaitBlit();
    
	custom->intena=0x7fff;//disable all interrupts
	custom->intreq=0x7fff;//Clear any interrupts that were pending
	custom->dmacon=0x7fff;//Clear all DMA channels

	//restore interrupts
	SetInterruptHandler(SystemIrq);

	/*Restore system copper list(s). */
	custom->cop1lc=(ULONG)GfxBase->copinit;
	custom->cop2lc=(ULONG)GfxBase->LOFlist;
	custom->copjmp1=0x7fff; //start coppper

	/*Restore all interrupts and DMA settings. */
	custom->intena=SystemInts|0x8000;
	custom->dmacon=SystemDMA|0x8000;
	custom->adkcon=SystemADKCON|0x8000;

	WaitBlit();	
	DisownBlitter();
	Enable();

	LoadView(ActiView);
	WaitTOF();
	WaitTOF();

	Permit();
} 
 
static __attribute__((interrupt)) void InterruptHandler() 
{
	custom->intreq=(1<<INTB_VERTB);  
}


void SetupCopper()
{

    USHORT* cop = copperList;
    int i;
 
    // Set up display window and bitplane pointers
     const USHORT lineSize=320/8;
    cop = screenScanDefault(cop);
	//enable bitplanes	
	*cop++ = offsetof(struct Custom, bplcon0);
	*cop++ = 0x4200;
	*cop++ = offsetof(struct Custom, bplcon1);	//scrolling
 
	*cop++ = 0;
	*cop++ = offsetof(struct Custom, bplcon2);	//playfied priority
	*cop++ = 1<<6;		 

	//set bitplane modulo
	*cop++=offsetof(struct Custom, bpl1mod); //odd planes   1,3
	*cop++=4*lineSize;
	*cop++=offsetof(struct Custom, bpl2mod); //even  planes 2,4
	*cop++=4*lineSize;
 
	// set bitplane pointers
	const UBYTE* planes[BPL_DEPTH];
	for(int a=0;a<BPL_DEPTH;a++)
		planes[a]=(UBYTE*)banner + lineSize * a;
	cop = copSetPlanes(0, cop, planes, BPL_DEPTH);
 
    // set colors
	for(int a=0; a < NUM_COLORS; a++)
		cop = copSetColor(cop, a, ((USHORT*)banner_col)[a]);
 
    *cop++ = 0xFFFF; // End Copper list
    *cop++ = 0xFFFE;

}
 
// Cleanup
void DisableCopper(void)
{
    if (copperList)
    {
        custom->cop1lc = 0; // Disable Copper
        FreeMem(copperList, 1024);
    }
 
    
}

// Blitter copy for scrolling

void ScrollBitmap(int scroll_x) 
{
    // Configure blitter for each bitplane
    
    WaitBlit();

    custom->bltcon0 = A_TO_D | DEST;
 
    custom->bltadat = 0;
 
    UBYTE *src = (UBYTE*)banner; // Shift source, wrap at 40 bytes
    custom->bltdpt = src; // Source pointer
    custom->bltamod = 0;
    custom->bltdmod = 0;
    custom->bltafwm = custom->bltalwm = 0xffff;
    custom->bltsize = (SCREEN_HEIGHT) | (SCREEN_WIDTH / 16);

	custom->bpl1mod = 4*40; 
	custom->bpl2mod = 4*40; 
 
}

int main(void)
{
    SysBase = *((struct ExecBase**)4UL);
	custom = (struct Custom*)0xdff000;
 
	// We will use the graphics library only to locate and restore the system copper list once we are through.

	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",0);
	if (!GfxBase)
		Exit(0);

    // used for printing
	DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 0);
	if (!DOSBase)
		Exit(0);
 
   
    // Allocate CHIP RAM for Copper list
    copperList = AllocMem(1024, MEMF_CHIP|MEMF_CLEAR);

    debug_register_palette(banner_col, "images/banner.PAL", 16, 0);
    debug_register_bitmap(banner, "images/banner.BPL", 320, 256, 4, debug_resource_bitmap_interleaved);
    debug_register_copperlist(copperList, "copper1", 1024, 0);
    
    Delay(10);

	TakeSystem();

	WaitVbl();

    SetupCopper();
 
    custom->cop1lc = (ULONG)copperList;
   
	custom->dmacon = DMAF_BLITTER;//disable blitter dma for copjmp bug
	custom->copjmp1 = 0x7fff; //start coppper
	custom->dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_RASTER | DMAF_COPPER | DMAF_BLITTER;

     SetInterruptHandler((APTR)InterruptHandler);
 
    custom->intena = INTF_SETCLR | INTF_INTEN | INTF_VERTB;
    custom->intreq=(1<<INTB_VERTB);//reset vbl req

    int scroll_x = 0;

	while(!MouseLeft()) 
    {		 
        WaitLine(0x10);
        ScrollBitmap(scroll_x);
		scroll_x = (scroll_x + 1) % SCREEN_WIDTH; // Wrap at 320 pixels
	}

    DisableCopper();
    FreeSystem();

	CloseLibrary((struct Library*)DOSBase);
	CloseLibrary((struct Library*)GfxBase);
 
    return 0;
}