/*
  Hatari - screen.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  This code converts a 1/2/4 plane ST format screen to either 8, 16 or 32-bit PC
  format. An awful lot of processing is needed to do this conversion - we
  cannot simply change palettes on  interrupts as it is possible with DOS.
  The main code processes the palette/resolution mask tables to find exactly
  which lines need to updating and the conversion routines themselves only
  update 16-pixel blocks which differ from the previous frame - this gives a
  large performance increase.
  Each conversion routine can convert any part of the source ST screen (which
  includes the overscan border, usually set to colour zero) so they can be used
  for both window and full-screen mode.
  Note that in Hi-Resolution we have no overscan and just two colors so we can
  optimise things further.
  In color mode it seems possible to display 47 lines in the bottom border
  with a second 60/50 Hz switch, but most programs consider there are 45
  visible lines in the bottom border only, which gives a total of 274 lines
  for a screen. So not displaying the last two lines fixes garbage that could
  appear in the last two lines when displaying 47 lines (Digiworld 2 by ICE,
  Tyranny by DHS).
*/

const char Screen_fileid[] = "Hatari screen.c : " __DATE__ " " __TIME__;

#include <SDL.h>
#include <SDL_endian.h>
#include <assert.h>

#include "main.h"
#include "configuration.h"
#include "avi_record.h"
#include "log.h"
#include "paths.h"
#include "options.h"
#include "screen.h"
#include "screenConvert.h"
#include "control.h"
#include "convert/routines.h"
#include "resolution.h"
#include "spec512.h"
#include "statusbar.h"
#include "vdi.h"
#include "video.h"
#include "falcon/videl.h"

#ifdef __LIBRETRO__ 
extern uint32_t *videoBuffer;
extern int NEWGAME_FROM_OSD,retrow,retroh;
#endif 

#define DEBUG 0

#if DEBUG
# define DEBUGPRINT(x) printf x
#else
# define DEBUGPRINT(x)
#endif

/* extern for several purposes */
SDL_Surface *sdlscrn = NULL;                /* The SDL screen surface */
int nScreenZoomX, nScreenZoomY;             /* Zooming factors, used for scaling mouse motions */
int nBorderPixelsLeft, nBorderPixelsRight;  /* Pixels in left and right border */
static int nBorderPixelsTop, nBorderPixelsBottom;  /* Lines in top and bottom border */

/* extern for shortcuts etc. */
bool bGrabMouse = false;      /* Grab the mouse cursor in the window */
bool bInFullScreen = false;   /* true if in full screen */

/* extern for spec512.c */
int STScreenLeftSkipBytes;
int STScreenStartHorizLine;   /* Start lines to be converted */
Uint32 STRGBPalette[16];      /* Palette buffer used in conversion routines */
Uint32 ST2RGB[4096];          /* Table to convert ST 0x777 / STe 0xfff palette to PC format RGB551 (2 pixels each entry) */

/* extern for video.c */
Uint8 *pSTScreen;
FRAMEBUFFER *pFrameBuffer;    /* Pointer into current 'FrameBuffer' */

static FRAMEBUFFER FrameBuffers[NUM_FRAMEBUFFERS]; /* Store frame buffer details to tell how to update */
static Uint8 *pSTScreenCopy;                       /* Keep track of current and previous ST screen data */
static Uint8 *pPCScreenDest;                       /* Destination PC buffer */
static int STScreenEndHorizLine;                   /* End lines to be converted */
static int PCScreenBytesPerLine;
static int STScreenWidthBytes;
static int PCScreenOffsetX;                        /* how many pixels to skip from left when drawing */
static int PCScreenOffsetY;                        /* how many pixels to skip from top when drawing */
static SDL_Rect STScreenRect;                      /* screen size without statusbar */

static int STScreenLineOffset[NUM_VISIBLE_LINES];  /* Offsets for ST screen lines eg, 0,160,320... */
static Uint16 HBLPalette[16], PrevHBLPalette[16];  /* Current palette for line, also copy of first line */

static void (*ScreenDrawFunctionsNormal[3])(void); /* Screen draw functions */

static bool bScreenContentsChanged;     /* true if buffer changed and requires blitting */
static bool bScrDoubleY;                /* true if double on Y */
static int ScrUpdateFlag;               /* Bit mask of how to update screen */
static bool bRGBTableInSync;            /* Is RGB table up to date? */

/* These are used for the generic screen conversion functions */
static int genconv_width_req, genconv_height_req, genconv_bpp;
static bool genconv_do_update;          /* HW surface is available -> the SDL need not to update the surface after ->pixel access */


static bool Screen_DrawFrame(bool bForceFlip);

#if WITH_SDL2

SDL_Window *sdlWindow;
static SDL_Renderer *sdlRenderer;
static SDL_Texture *sdlTexture;

void SDL_UpdateRects(SDL_Surface *screen, int numrects, SDL_Rect *rects)
{
	if (ConfigureParams.Screen.bUseSdlRenderer)
	{
		SDL_UpdateTexture(sdlTexture, NULL, screen->pixels, screen->pitch);
		SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
		SDL_RenderPresent(sdlRenderer);
	}
	else
	{
		SDL_UpdateWindowSurfaceRects(sdlWindow, rects, numrects);
	}
}

void SDL_UpdateRect(SDL_Surface *screen, Sint32 x, Sint32 y, Sint32 w, Sint32 h)
{
	SDL_Rect rect;

	if (w == 0 && h == 0) {
		x = y = 0;
		w = screen->w;
		h = screen->h;
	}

	rect.x = x; rect.y = y;
	rect.w = w; rect.h = h;
	SDL_UpdateRects(screen, 1, &rect);
}

#endif

/*-----------------------------------------------------------------------*/
/**
 * Create ST 0x777 / STe 0xfff color format to 16 or 32 bits per pixel
 * conversion table. Called each time when changed resolution or to/from
 * fullscreen mode.
 */
static void Screen_SetupRGBTable(void)
{
	Uint16 STColor;
	Uint32 RGBColor;
	int r, g, b;
	int rr, gg, bb;

	/* Do Red, Green and Blue for all 16*16*16 = 4096 STe colors */
	for (r = 0; r < 16; r++)
	{
		for (g = 0; g < 16; g++)
		{
			for (b = 0; b < 16; b++)
			{
				/* STe 0xfff format */
				STColor = (r<<8) | (g<<4) | (b);
				rr = ((r & 0x7) << 1) | ((r & 0x8) >> 3);
				rr |= rr << 4;
				gg = ((g & 0x7) << 1) | ((g & 0x8) >> 3);
				gg |= gg << 4;
				bb = ((b & 0x7) << 1) | ((b & 0x8) >> 3);
				bb |= bb << 4;
				RGBColor = SDL_MapRGB(sdlscrn->format, rr, gg, bb);
				if (sdlscrn->format->BitsPerPixel <= 16)
				{
					/* As longs, for speed (write two pixels at once) */
					ST2RGB[STColor] = (RGBColor<<16) | RGBColor;
				}
				else
				{
					ST2RGB[STColor] = RGBColor;
				}
			}
		}
	}
}


/**
 * Convert 640x400 monochrome screen
 */
static void Screen_ConvertHighRes(void)
{
	int linewidth = 640 / 16;

	Screen_GenConvert(pSTScreen, 640, 400, 1, linewidth, 0, 0, 0, 0, 0);
	bScreenContentsChanged = true;
}

/**
 * Set screen draw functions.
 */
static void Screen_SetDrawFunctions(int nBitCount, bool bDoubleLowRes)
{
	if (nBitCount <= 16)
	{
		/* High color */
		if (bDoubleLowRes)
			ScreenDrawFunctionsNormal[ST_LOW_RES] = ConvertLowRes_640x16Bit;
		else
			ScreenDrawFunctionsNormal[ST_LOW_RES] = ConvertLowRes_320x16Bit;
		ScreenDrawFunctionsNormal[ST_MEDIUM_RES] = ConvertMediumRes_640x16Bit;
	}
	else /* Assume 32 bit drawing functions */
	{
		/* True color */
		if (bDoubleLowRes)
			ScreenDrawFunctionsNormal[ST_LOW_RES] = ConvertLowRes_640x32Bit;
		else
			ScreenDrawFunctionsNormal[ST_LOW_RES] = ConvertLowRes_320x32Bit;
		ScreenDrawFunctionsNormal[ST_MEDIUM_RES] = ConvertMediumRes_640x32Bit;
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Set amount of border pixels
 */
static void Screen_SetBorderPixels(int leftX, int leftY)
{
	/* All screen widths need to be aligned to 16-bits */
	nBorderPixelsLeft = Opt_ValueAlignMinMax(leftX/2, 16, 0, 48);
	nBorderPixelsRight = nBorderPixelsLeft;

	/* assertain assumption of code below */
	assert(OVERSCAN_TOP < MAX_OVERSCAN_BOTTOM);
	
	if (leftY > 2*OVERSCAN_TOP)
	{
		nBorderPixelsTop = OVERSCAN_TOP;
		if (leftY >= OVERSCAN_TOP + MAX_OVERSCAN_BOTTOM)
			nBorderPixelsBottom = MAX_OVERSCAN_BOTTOM;
		else
			nBorderPixelsBottom = leftY - OVERSCAN_TOP;
	}
	else
	{
		if (leftY > 0)
			nBorderPixelsTop = nBorderPixelsBottom = leftY/2;
		else
			nBorderPixelsTop = nBorderPixelsBottom = 0;
	}
}

/*-----------------------------------------------------------------------*/
/**
 * store Y offset for each horizontal line in our source ST screen for
 * reference in the convert functions.
 */
static void Screen_SetSTScreenOffsets(void)
{
	int i;

	/* Store offset to each horizontal line, uses
	 * nBorderPixels* variables.
	 */
	for (i = 0; i < NUM_VISIBLE_LINES; i++)
	{
		STScreenLineOffset[i] = i * SCREENBYTES_LINE;
	}
}

/**
 * Return true if Falcon/TT/VDI generic screen convert functions
 * need to be used instead of the ST/STE functions.
 */
static bool Screen_UseGenConvScreen(void)
{
	return Config_IsMachineFalcon() || Config_IsMachineTT()
		|| bUseHighRes || bUseVDIRes;
}

static bool Screen_WantToKeepResolution(void)
{
#if WITH_SDL2
	return ConfigureParams.Screen.bKeepResolution;
#else
	if (Screen_UseGenConvScreen())
		return ConfigureParams.Screen.bKeepResolution;
	else
		return ConfigureParams.Screen.bKeepResolutionST;
#endif
}

#if WITH_SDL2
static void Screen_FreeSDL2Resources(void)
{
	if (sdlTexture)
	{
		SDL_DestroyTexture(sdlTexture);
		sdlTexture = NULL;
	}
	if (sdlscrn)
	{
		if (ConfigureParams.Screen.bUseSdlRenderer)
			SDL_FreeSurface(sdlscrn);
		sdlscrn = NULL;
	}
	if (sdlRenderer)
	{
		SDL_DestroyRenderer(sdlRenderer);
		sdlRenderer = NULL;
	}
}
#endif

/**
 * Change the SDL video mode.
 * @return true if mode has been changed, false if change was not necessary
 */
bool Screen_SetSDLVideoSize(int width, int height, int bitdepth, bool bForceChange)
{
	Uint32 sdlVideoFlags;
#if WITH_SDL2
	static int nPrevRenderScaleQuality = 0;
	static bool bPrevUseVsync = false;
	static bool bPrevInFullScreen;
	int win_width, win_height;

	if (bitdepth == 0 || bitdepth == 24)
		bitdepth = 32;
#endif
	/* Check if we really have to change the video mode: */
	if (sdlscrn != NULL && sdlscrn->w == width && sdlscrn->h == height
	    && sdlscrn->format->BitsPerPixel == bitdepth && !bForceChange)
		return false;

#ifdef _MUDFLAP
	if (sdlscrn)
	{
		__mf_unregister(sdlscrn->pixels, sdlscrn->pitch*sdlscrn->h, __MF_TYPE_GUESS);
	}
#endif
	if (bInFullScreen)
	{
		/* unhide the Hatari WM window for fullscreen */
		Control_ReparentWindow(width, height, bInFullScreen);
	}

#if WITH_SDL2

	/* SDL Video attributes: */
	win_width = width;
	win_height = height;
	if (bInFullScreen)
	{
		sdlVideoFlags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_INPUT_GRABBED;
		if (ConfigureParams.Screen.bKeepResolution)
			sdlVideoFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		else
			sdlVideoFlags |= SDL_WINDOW_FULLSCREEN;
	}
	else
	{
		int deskw, deskh;
		if (getenv("PARENT_WIN_ID") != NULL)	/* Embedded window? */
			sdlVideoFlags = SDL_WINDOW_BORDERLESS;
		else if (ConfigureParams.Screen.bResizable
		         && ConfigureParams.Screen.bUseSdlRenderer)
			sdlVideoFlags = SDL_WINDOW_RESIZABLE;
		else
			sdlVideoFlags = 0;
		/* Make sure that window is not bigger than current desktop */
		Resolution_GetDesktopSize(&deskw, &deskh);
		if (win_width > deskw)
			win_width = deskw;
		if (win_height > deskh)
			win_height = deskh;
	}

	Screen_FreeSDL2Resources();
	if (sdlWindow &&
	    ((bInFullScreen && !ConfigureParams.Screen.bKeepResolution) ||
	     (bPrevInFullScreen != bInFullScreen) ||
	     bForceChange
	    ))
	{
		SDL_DestroyWindow(sdlWindow);
		sdlWindow = NULL;
	}
	bPrevInFullScreen = bInFullScreen;

	/* Set SDL2 video hints */
	if (nPrevRenderScaleQuality != ConfigureParams.Screen.nRenderScaleQuality)
	{
		char hint[2] = { '0' + ConfigureParams.Screen.nRenderScaleQuality, 0 };
		SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, hint, SDL_HINT_OVERRIDE);
		nPrevRenderScaleQuality = ConfigureParams.Screen.nRenderScaleQuality;
	}
	if (bPrevUseVsync != ConfigureParams.Screen.bUseVsync)
	{
		char hint[2] = { '0' + ConfigureParams.Screen.bUseVsync, 0 };
		SDL_SetHintWithPriority(SDL_HINT_RENDER_VSYNC, hint, SDL_HINT_OVERRIDE);
		bPrevUseVsync = ConfigureParams.Screen.bUseVsync;
	}

#ifdef SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4		/* requires sdl >= 2.0.4 */
	/* Disable closing Hatari with alt+F4 under Windows as alt+F4 can be used by some emulated programs */
	SDL_SetHintWithPriority(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1", SDL_HINT_OVERRIDE);
#endif

	/* Set new video mode */
	DEBUGPRINT(("SDL screen request: %d x %d @ %d (%s)\n", width, height,
	        bitdepth, bInFullScreen?"fullscreen":"windowed"));

	if (sdlWindow)
	{
		if ((SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_MAXIMIZED) == 0)
			SDL_SetWindowSize(sdlWindow, win_width, win_height);
	}
	else
	{
		sdlWindow = SDL_CreateWindow("Hatari", SDL_WINDOWPOS_UNDEFINED,
		                             SDL_WINDOWPOS_UNDEFINED,
		                             win_width, win_height, sdlVideoFlags);
	}
	if (!sdlWindow)
	{
		fprintf(stderr, "Failed to create %dx%d window!\n",
		        win_width, win_height);
		exit(-1);
	}
	if (ConfigureParams.Screen.bUseSdlRenderer)
	{
		int rm, bm, gm, pfmt;

		sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
		if (!sdlRenderer)
		{
			fprintf(stderr,"Failed to create %dx%d renderer!\n",
			        win_width, win_height);
			exit(1);
		}

		if (bInFullScreen)
			SDL_RenderSetLogicalSize(sdlRenderer, width, height);

		if (bitdepth == 16)
		{
			rm = 0xF800;
			gm = 0x07E0;
			bm = 0x001F;
			pfmt = SDL_PIXELFORMAT_RGB565;
		}
		else
		{
			rm = 0x00FF0000;
			gm = 0x0000FF00;
			bm = 0x000000FF;
			pfmt = SDL_PIXELFORMAT_RGB888;
		}
		sdlscrn = SDL_CreateRGBSurface(0, width, height, bitdepth,
		                               rm, gm, bm, 0);
		sdlTexture = SDL_CreateTexture(sdlRenderer, pfmt,
		                               SDL_TEXTUREACCESS_STREAMING,
		                               width, height);
		if (!sdlTexture)
		{
			fprintf(stderr,"Failed to create %dx%d@%d texture!\n",
			       width, height, bitdepth);
			exit(-3);
		}
	}
	else
	{
		sdlscrn = SDL_GetWindowSurface(sdlWindow);
	}

#else	/* WITH_SDL2 */

	/* SDL Video attributes: */
	if (bInFullScreen)
	{
		sdlVideoFlags  = SDL_HWSURFACE|SDL_FULLSCREEN/*|SDL_DOUBLEBUF*/;
		/* SDL_DOUBLEBUF helps avoiding tearing and can be faster on suitable HW,
		 * but it doesn't work with partial screen updates done by the ST screen
		 * update code or the Hatari GUI, so double buffering is disabled. */
	}
	else
	{
		sdlVideoFlags  = SDL_SWSURFACE;
	}

	/* Set new video mode */
	DEBUGPRINT(("SDL screen request: %d x %d @ %d (%s)\n", width, height, bitdepth,
	            bInFullScreen?"fullscreen":"windowed"));
#ifndef __LIBRETRO__ 
	sdlscrn = SDL_SetVideoMode(width, height, bitdepth, sdlVideoFlags);
#else
	/* FORCE 32 BITDEPTH */
	sdlscrn = SDL_SetVideoMode(width, height, 32/*bitdepth*/, sdlVideoFlags);

	videoBuffer=(unsigned int *)sdlscrn->pixels;
	printf("SDL screen obtain: %d x %d @ %d (%s)\n", sdlscrn->w, sdlscrn->h, sdlscrn->format->BitsPerPixel, bInFullScreen?"fullscreen":"windowed");
	retrow=sdlscrn->w;
	retroh=sdlscrn->h;
	NEWGAME_FROM_OSD=1;
#endif 

	/* By default ConfigureParams.Screen.nForceBpp and therefore
	 * BitCount is zero which means "SDL color depth autodetection".
	 * In this case the SDL_SetVideoMode() call might return
	 * a 24 bpp resolution
	 */
	if (sdlscrn && sdlscrn->format->BitsPerPixel == 24)
	{
		fprintf(stderr, "Unsupported color depth 24, trying 32 bpp instead...\n");
		sdlscrn = SDL_SetVideoMode(width, height, 32, sdlVideoFlags);
	}

#endif	/* WITH_SDL2 */

	/* Exit if we can not open a screen */
	if (!sdlscrn)
	{
		fprintf(stderr, "Could not set video mode:\n %s\n", SDL_GetError() );
		SDL_Quit();
		exit(-2);
	}

	DEBUGPRINT(("SDL screen granted: %d x %d @ %d\n", sdlscrn->w, sdlscrn->h,
	            sdlscrn->format->BitsPerPixel));

#ifdef _MUDFLAP
	__mf_register(sdlscrn->pixels, sdlscrn->pitch*sdlscrn->h, __MF_TYPE_GUESS, "SDL pixels");
#endif

	if (!bInFullScreen)
	{
		/* re-embed the new Hatari SDL window */
		Control_ReparentWindow(width, height, bInFullScreen);
	}

	Avi_SetSurface(sdlscrn);

	bRGBTableInSync = false;

	return true;
}


/**
 * Initialize ST/STE screen resolution.
 */
static void Screen_SetSTResolution(bool bForceChange)
{
	int Width, Height, nZoom, SBarHeight, BitCount, maxW, maxH;
	bool bDoubleLowRes = false;

	/* Bits per pixel */
	BitCount = ConfigureParams.Screen.nForceBpp;

	nBorderPixelsTop = nBorderPixelsBottom = 0;
	nBorderPixelsLeft = nBorderPixelsRight = 0;

	nScreenZoomX = 1;
	nScreenZoomY = 1;

	if (STRes == ST_LOW_RES)
	{
		Width = 320;
		Height = 200;
		nZoom = 1;
	}
	else    /* else use 640x400, also for med-rez */
	{
		Width = 640;
		Height = 400;
		nZoom = 2;
	}

	/* Statusbar height for doubled screen size */
	SBarHeight = Statusbar_GetHeightForSize(640, 400);

	Resolution_GetLimits(&maxW, &maxH, &BitCount, Screen_WantToKeepResolution());

	/* Zoom if necessary, factors used for scaling mouse motions */
	if (STRes == ST_LOW_RES &&
	    2*Width <= maxW && 2*Height+SBarHeight <= maxH)
	{
		nZoom = 2;
		Width *= 2;
		Height *= 2;
		nScreenZoomX = 2;
		nScreenZoomY = 2;
		bDoubleLowRes = true;
	}
	else if (STRes == ST_MEDIUM_RES)
	{
		/* med-rez conversion functions want always
		 * to double vertically, they don't support
		 * skipping that (only leaving doubled lines
		 * black for the TV mode).
		 */
		nScreenZoomX = 1;
		nScreenZoomY = 2;
	}

	/* Adjust width/height for overscan borders, if mono or VDI we have no overscan */
	if (ConfigureParams.Screen.bAllowOverscan && !bUseHighRes)
	{
		int leftX = maxW - Width;
		int leftY = maxH - (Height + Statusbar_GetHeightForSize(Width, Height));

		Screen_SetBorderPixels(leftX/nZoom, leftY/nZoom);
		DEBUGPRINT(("resolution limit:\n\t%d x %d\nlimited resolution:\n\t", maxW, maxH));
		DEBUGPRINT(("%d * (%d + %d + %d) x (%d + %d + %d)\n", nZoom,
			    nBorderPixelsLeft, Width/nZoom, nBorderPixelsRight,
			    nBorderPixelsTop, Height/nZoom, nBorderPixelsBottom));
		Width += (nBorderPixelsRight + nBorderPixelsLeft)*nZoom;
		Height += (nBorderPixelsTop + nBorderPixelsBottom)*nZoom;
		DEBUGPRINT(("\t= %d x %d (+ statusbar)\n", Width, Height));
	}

	Screen_SetSTScreenOffsets();  
	Height += Statusbar_SetHeight(Width, Height);

	PCScreenOffsetX = PCScreenOffsetY = 0;

	/* Video attributes: */
#if !WITH_SDL2
	if (bInFullScreen && ConfigureParams.Screen.bKeepResolutionST)
	{
		/* use desktop resolution */
		Resolution_GetDesktopSize(&maxW, &maxH);
		SBarHeight = Statusbar_GetHeightForSize(maxW, maxH);
		/* re-calculate statusbar height for this resolution */
		Statusbar_SetHeight(maxW, maxH-SBarHeight);
		/* center Atari screen to resolution */
		PCScreenOffsetY = (maxH - Height)/2;
		PCScreenOffsetX = (maxW - Width)/2;
		/* and select desktop resolution */
		Height = maxH;
		Width = maxW;
	}
#endif

	if (Screen_SetSDLVideoSize(Width, Height, BitCount, bForceChange))
	{
		Statusbar_Init(sdlscrn);

		/* screen area without the statusbar */
		STScreenRect.x = 0;
		STScreenRect.y = 0;
		STScreenRect.w = sdlscrn->w;
		STScreenRect.h = sdlscrn->h - Statusbar_GetHeight();
	}

	if (!bRGBTableInSync)
	{
		Screen_SetupRGBTable();   /* Create color conversion table */
		bRGBTableInSync = true;
	}

	/* Set drawing functions */
	Screen_SetDrawFunctions(sdlscrn->format->BitsPerPixel, bDoubleLowRes);

	Screen_SetFullUpdate();           /* Cause full update of screen */
}


/**
 * Change resolution, according to the machine and display type
 * that we're currently emulating.
 */
static void Screen_ChangeResolution(bool bForceChange)
{
	int hbpp = ConfigureParams.Screen.nForceBpp;

	if (bUseVDIRes)
	{
		Screen_SetGenConvSize(VDIWidth, VDIHeight, hbpp, bForceChange);
	}
	else if (Config_IsMachineFalcon())
	{
		Videl_ScreenModeChanged(bForceChange);
	}
	else if (Config_IsMachineTT())
	{
		int width, height, bpp;
		Video_GetTTRes(&width, &height, &bpp);
		Screen_SetGenConvSize(width, height, hbpp, bForceChange);
	}
	else if (bUseHighRes)
	{
		Screen_SetGenConvSize(640, 400, hbpp, bForceChange);
	}
	else
	{
		Screen_SetSTResolution(bForceChange);
	}

	if (bInFullScreen || bGrabMouse)
		SDL_WM_GrabInput(SDL_GRAB_ON);
	else
		SDL_WM_GrabInput(SDL_GRAB_OFF);
}


/**
 * Change the resolution - but only if it was already initialized before
 */
void Screen_ModeChanged(bool bForceChange)
{
	if (sdlscrn)	/* Do it only if we're already up and running */
	{
		Screen_ChangeResolution(bForceChange);
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Init Screen bitmap and buffers/tables needed for ST to PC screen conversion
 */
void Screen_Init(void)
{
	int i;
	SDL_Surface *pIconSurf;
	char sIconFileName[FILENAME_MAX];

	/* Clear frame buffer structures and set current pointer */
	memset(FrameBuffers, 0, NUM_FRAMEBUFFERS * sizeof(FRAMEBUFFER));

	/* Allocate previous screen check workspace. We are going to double-buffer a double-buffered screen. Oh. */
	for (i = 0; i < NUM_FRAMEBUFFERS; i++)
	{
		FrameBuffers[i].pSTScreen = malloc(MAX_VDI_BYTES);
		FrameBuffers[i].pSTScreenCopy = malloc(MAX_VDI_BYTES);
		if (!FrameBuffers[i].pSTScreen || !FrameBuffers[i].pSTScreenCopy)
		{
			fprintf(stderr, "Failed to allocate frame buffer memory.\n");
			exit(-1);
		}
	}
	pFrameBuffer = &FrameBuffers[0];

	/* Set initial window resolution */
	bInFullScreen = ConfigureParams.Screen.bFullScreen;
	Screen_ChangeResolution(false);
	ScreenDrawFunctionsNormal[ST_HIGH_RES] = Screen_ConvertHighRes;

	Video_SetScreenRasters();                       /* Set rasters ready for first screen */

	/* Load and set icon */
	snprintf(sIconFileName, sizeof(sIconFileName), "%s%chatari-icon.bmp",
	         Paths_GetDataDir(), PATHSEP);
	pIconSurf = SDL_LoadBMP(sIconFileName);
	if (pIconSurf)
	{
#if WITH_SDL2
		SDL_SetColorKey(pIconSurf, SDL_TRUE, SDL_MapRGB(pIconSurf->format, 255, 255, 255));
		SDL_SetWindowIcon(sdlWindow, pIconSurf);
#else
		SDL_SetColorKey(pIconSurf, SDL_SRCCOLORKEY, SDL_MapRGB(pIconSurf->format, 255, 255, 255));
		SDL_WM_SetIcon(pIconSurf, NULL);
#endif
		SDL_FreeSurface(pIconSurf);
	}

	/* Configure some SDL stuff: */
	SDL_ShowCursor(SDL_DISABLE);
}


/*-----------------------------------------------------------------------*/
/**
 * Free screen bitmap and allocated resources
 */
void Screen_UnInit(void)
{
	int i;

	/* Free memory used for copies */
	for (i = 0; i < NUM_FRAMEBUFFERS; i++)
	{
		free(FrameBuffers[i].pSTScreen);
		free(FrameBuffers[i].pSTScreenCopy);
	}

#if WITH_SDL2
	Screen_FreeSDL2Resources();
	if (sdlWindow)
	{
		SDL_DestroyWindow(sdlWindow);
		sdlWindow = NULL;
	}
#endif
}


/*-----------------------------------------------------------------------*/
/**
 * Reset screen
 */
void Screen_Reset(void)
{
	/* On re-boot, always correct ST resolution for monitor, eg Colour/Mono */
	if (bUseVDIRes)
	{
		STRes = VDIRes;
	}
	else
	{
		if (bUseHighRes)
		{
			STRes = ST_HIGH_RES;
			TTRes = TT_HIGH_RES;
		}
		else
		{
			STRes = ST_LOW_RES;
			TTRes = TT_MEDIUM_RES;
		}
	}
	/* Cause full update */
	Screen_ModeChanged(false);
}


/*-----------------------------------------------------------------------*/
/**
 * Set flags so screen will be TOTALLY re-drawn (clears whole of full-screen)
 * next time around
 */
void Screen_SetFullUpdate(void)
{
	int i;

	/* Update frame buffers */
	for (i = 0; i < NUM_FRAMEBUFFERS; i++)
		FrameBuffers[i].bFullUpdate = true;
}


/*-----------------------------------------------------------------------*/
/**
 * Clear Window display memory
 */
static void Screen_ClearScreen(void)
{
	SDL_FillRect(sdlscrn, &STScreenRect, SDL_MapRGB(sdlscrn->format, 0, 0, 0));
}


/*-----------------------------------------------------------------------*/
/**
 * Force screen redraw.  Does the right thing regardless of whether
 * we're in ST/STe, Falcon or TT mode.  Needed when switching modes
 * while emulation is paused.
 */
static void Screen_Refresh(void)
{
	if (bUseVDIRes)
	{
		Screen_GenDraw(VideoBase, VDIWidth, VDIHeight, VDIPlanes,
		               VDIWidth * VDIPlanes / 16, 0, 0, 0, 0);
	}
	else if (Config_IsMachineFalcon())
	{
		VIDEL_renderScreen();
	}
	else if (Config_IsMachineTT())
	{
		Video_RenderTTScreen();
	}
	else
	{
		Screen_DrawFrame(true);
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Enter Full screen mode
 */
void Screen_EnterFullScreen(void)
{
	bool bWasRunning;

	if (!bInFullScreen)
	{
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = true;

		if (Screen_UseGenConvScreen())
		{
			Screen_SetGenConvSize(genconv_width_req, genconv_height_req,
			                      genconv_bpp, true);
			/* force screen redraw */
			Screen_GenConvUpdate(NULL, true);
		}
		else
		{
			Screen_SetSTResolution(true);
			Screen_ClearScreen();       /* Black out screen bitmap as will be invalid when return */
		}

		if (!Screen_WantToKeepResolution())
		{
			/* Give monitor time to change to new resolution */
			SDL_Delay(20);
		}

		if (bWasRunning)
		{
			/* And off we go... */
			Main_UnPauseEmulation();
		}
		else
		{
			Screen_Refresh();
		}
		SDL_WM_GrabInput(SDL_GRAB_ON);  /* Grab mouse pointer in fullscreen */
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Return from Full screen mode back to a window
 */
void Screen_ReturnFromFullScreen(void)
{
	bool bWasRunning;

	if (bInFullScreen)
	{
		/* Hold things... */
		bWasRunning = Main_PauseEmulation(false);
		bInFullScreen = false;

		if (Screen_UseGenConvScreen())
		{
			Screen_SetGenConvSize(genconv_width_req, genconv_height_req,
			                      genconv_bpp, true);
			/* force screen redraw */
			Screen_GenConvUpdate(NULL, true);
		}
		else
		{
			Screen_SetSTResolution(true);
		}

		if (!Screen_WantToKeepResolution())
		{
			/* Give monitor time to switch resolution */
			SDL_Delay(20);
		}

		if (bWasRunning)
		{
			/* And off we go... */
			Main_UnPauseEmulation();
		}
		else
		{
			Screen_Refresh();
		}

		if (!bGrabMouse)
		{
			/* Un-grab mouse pointer in windowed mode */
			SDL_WM_GrabInput(SDL_GRAB_OFF);
		}
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Have we changed between low/med/high res?
 */
static void Screen_DidResolutionChange(int new_res)
{
	if (new_res != STRes)
	{
		STRes = new_res;
		Screen_ModeChanged(false);
	}
	else
	{
		/* Did change overscan mode? Causes full update */
		if (pFrameBuffer->VerticalOverscanCopy != VerticalOverscan)
			pFrameBuffer->bFullUpdate = true;
	}
}


/**
 * Compare current resolution on line with previous, and set 'UpdateLine' accordingly
 * Return if swap between low/medium resolution
 */
static bool Screen_CompareResolution(int y, int *pUpdateLine, int oldres)
{
	/* Check if wrote to resolution register */
	if (HBLPaletteMasks[y]&PALETTEMASK_RESOLUTION)  /* See 'Intercept_ShifterMode_WriteByte' */
	{
		int newres = (HBLPaletteMasks[y]>>16)&ST_MEDIUM_RES_BIT;
		/* Did resolution change? */
		if (newres != (int)((pFrameBuffer->HBLPaletteMasks[y]>>16)&ST_MEDIUM_RES_BIT))
			*pUpdateLine |= PALETTEMASK_UPDATERES;
		else
			*pUpdateLine &= ~PALETTEMASK_UPDATERES;
		/* Have used any low/medium res mix? */
		return (newres != (oldres&ST_MEDIUM_RES_BIT));
	}
	return false;
}


/*-----------------------------------------------------------------------*/
/**
 * Check to see if palette changes cause screen update and keep 'HBLPalette[]' up-to-date
 */
static void Screen_ComparePalette(int y, int *pUpdateLine)
{
	bool bPaletteChanged = false;
	int i;

	/* Did write to palette in this or previous frame? */
	if (((HBLPaletteMasks[y]|pFrameBuffer->HBLPaletteMasks[y])&PALETTEMASK_PALETTE)!=0)
	{
		/* Check and update ones which changed */
		for (i = 0; i < 16; i++)
		{
			if (HBLPaletteMasks[y]&(1<<i))        /* Update changes in ST palette */
				HBLPalette[i] = HBLPalettes[(y*16)+i];
		}
		/* Now check with same palette from previous frame for any differences(may be changing palette back) */
		for (i = 0; (i < 16) && (!bPaletteChanged); i++)
		{
			if (HBLPalette[i]!=pFrameBuffer->HBLPalettes[(y*16)+i])
				bPaletteChanged = true;
		}
		if (bPaletteChanged)
			*pUpdateLine |= PALETTEMASK_UPDATEPAL;
		else
			*pUpdateLine &= ~PALETTEMASK_UPDATEPAL;
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Check for differences in Palette and Resolution from Mask table and update
 * and store off which lines need updating and create full-screen palette.
 * (It is very important for these routines to check for colour changes with
 * the previous screen so only the very minimum parts are updated).
 * Return new STRes value.
 */
static int Screen_ComparePaletteMask(int res)
{
	bool bLowMedMix = false;
	int LineUpdate = 0;
	int y;

	/* Set for monochrome? */
	if (bUseHighRes)
	{
		VerticalOverscan = V_OVERSCAN_NONE;

		/* Just copy mono colors */
		if (HBLPalettes[0] & 0x777)
		{
			HBLPalettes[0] = 0x777;
			HBLPalettes[1] = 0x000;
		}
		else
		{
			HBLPalettes[0] = 0x000;
			HBLPalettes[1] = 0x777;
		}

		/* Colors changed? */
		if (HBLPalettes[0] != PrevHBLPalette[0])
			pFrameBuffer->bFullUpdate = true;

		/* Set bit to flag 'full update' */
		if (pFrameBuffer->bFullUpdate)
			ScrUpdateFlag = PALETTEMASK_UPDATEFULL;
		else
			ScrUpdateFlag = 0x00000000;

		/* Force to standard hi-resolution screen, without overscan */
		res = ST_HIGH_RES;
	}
	else    /* Full colour */
	{
		/* Get resolution */
		//res = (HBLPaletteMasks[0]>>16)&ST_RES_MASK;
		/* [NP] keep only low/med bit (could be hires in case of overscan on the 1st line) */
		res = (HBLPaletteMasks[0]>>16)&ST_MEDIUM_RES_BIT;

		/* Do all lines - first is tagged as full-update */
		for (y = 0; y < NUM_VISIBLE_LINES; y++)
		{
			/* Find any resolution/palette change and update palette/mask buffer */
			/* ( LineUpdate has top two bits set to say if line needs updating due to palette or resolution change ) */
			bLowMedMix |= Screen_CompareResolution(y, &LineUpdate, res);
			Screen_ComparePalette(y,&LineUpdate);
			HBLPaletteMasks[y] = (HBLPaletteMasks[y]&(~PALETTEMASK_UPDATEMASK)) | LineUpdate;
			/* Copy palette and mask for next frame */
			memcpy(&pFrameBuffer->HBLPalettes[y*16],HBLPalette,sizeof(short int)*16);
			pFrameBuffer->HBLPaletteMasks[y] = HBLPaletteMasks[y];
		}
		/* Did mix/have medium resolution? */
		if (bLowMedMix || (res & ST_MEDIUM_RES_BIT))
			res = ST_MEDIUM_RES;
	}

	/* Copy old palette for compare */
	memcpy(PrevHBLPalette, HBLPalettes, sizeof(Uint16)*16);

	return res;
}


/*-----------------------------------------------------------------------*/
/**
 * Update Palette Mask to show 'full-update' required. This is usually done after a resolution change
 * or when going between a Window and full-screen display
 */
static void Screen_SetFullUpdateMask(void)
{
	int y;

	for (y = 0; y < NUM_VISIBLE_LINES; y++)
		HBLPaletteMasks[y] |= PALETTEMASK_UPDATEFULL;
}


/*-----------------------------------------------------------------------*/
/**
 * Set details for ST screen conversion.
 */
static void Screen_SetConvertDetails(void)
{
	pSTScreen = pFrameBuffer->pSTScreen;          /* Source in ST memory */
	pSTScreenCopy = pFrameBuffer->pSTScreenCopy;  /* Previous ST screen */
	pPCScreenDest = sdlscrn->pixels;              /* Destination PC screen */

	PCScreenBytesPerLine = sdlscrn->pitch;        /* Bytes per line */

	/* Center to available framebuffer */
	pPCScreenDest += PCScreenOffsetY * PCScreenBytesPerLine + PCScreenOffsetX * (sdlscrn->format->BitsPerPixel/8);

	pHBLPalettes = pFrameBuffer->HBLPalettes;     /* HBL palettes pointer */
	/* Not in TV-Mode? Then double up on Y: */
	bScrDoubleY = !(ConfigureParams.Screen.nMonitorType == MONITOR_TYPE_TV);

	if (ConfigureParams.Screen.bAllowOverscan)  /* Use borders? */
	{
		/* Always draw to WHOLE screen including ALL borders */
		STScreenLeftSkipBytes = 0;              /* Number of bytes to skip on ST screen for left (border) */

		if (bUseHighRes)
		{
				pFrameBuffer->VerticalOverscanCopy = VerticalOverscan = V_OVERSCAN_NONE;
			STScreenStartHorizLine = 0;
			STScreenEndHorizLine = 400;
		}
		else
		{
			STScreenWidthBytes = SCREENBYTES_LINE;  /* Number of horizontal bytes in our ST screen */
			STScreenStartHorizLine = OVERSCAN_TOP - nBorderPixelsTop;
			STScreenEndHorizLine = OVERSCAN_TOP + 200 + nBorderPixelsBottom;
		}
	}
	else
	{
		/* Only draw main area and centre on Y */
		STScreenLeftSkipBytes = SCREENBYTES_LEFT;
		STScreenWidthBytes = SCREENBYTES_MIDDLE;
		STScreenStartHorizLine = OVERSCAN_TOP;
		STScreenEndHorizLine = OVERSCAN_TOP + (bUseHighRes ? 400 : 200);
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Lock full-screen for drawing
 */
bool Screen_Lock(void)
{
	if (SDL_MUSTLOCK(sdlscrn))
	{
		if (SDL_LockSurface(sdlscrn))
		{
			Screen_ReturnFromFullScreen();   /* All OK? If not need to jump back to a window */
			return false;
		}
	}

	return true;
}

/*-----------------------------------------------------------------------*/
/**
 * UnLock full-screen
 */
void Screen_UnLock(void)
{
	if ( SDL_MUSTLOCK(sdlscrn) )
		SDL_UnlockSurface(sdlscrn);
}


/*-----------------------------------------------------------------------*/
/**
 * Blit our converted ST screen to window/full-screen
 */
static void Screen_Blit(SDL_Rect *sbar_rect)
{
	unsigned char *pTmpScreen;

	/* Don't update anything on screen if video output is disabled */
	if ( ConfigureParams.Screen.DisableVideo )
		return;

#if 0	/* double buffering cannot be used with partial screen updates */
# if NUM_FRAMEBUFFERS > 1
	if (bInFullScreen && (sdlscrn->flags & SDL_DOUBLEBUF))
	{
		/* Swap screen */
		if (pFrameBuffer==&FrameBuffers[0])
			pFrameBuffer = &FrameBuffers[1];
		else
			pFrameBuffer = &FrameBuffers[0];
		SDL_Flip(sdlscrn);
	}
	else
# endif
#endif
#ifndef __LIBRETRO__
	{
		int count = 1;
		SDL_Rect rects[2];
		rects[0] = STScreenRect;
		if (sbar_rect)
		{
			rects[1] = *sbar_rect;
			count = 2;
		}
		SDL_UpdateRects(sdlscrn, count, rects);
	}
#endif
	/* Swap copy/raster buffers in screen. */
	pTmpScreen = pFrameBuffer->pSTScreenCopy;
	pFrameBuffer->pSTScreenCopy = pFrameBuffer->pSTScreen;
	pFrameBuffer->pSTScreen = pTmpScreen;
}


/*-----------------------------------------------------------------------*/
/**
 * Draw ST screen to window/full-screen framebuffer
 * @param  bForceFlip  Force screen update, even if contents did not change
 * @return  true if screen contents changed
 */
static bool Screen_DrawFrame(bool bForceFlip)
{
	int new_res;
	void (*pDrawFunction)(void);
	static bool bPrevFrameWasSpec512 = false;
	SDL_Rect *sbar_rect;

	assert(!bUseVDIRes);

	/* Scan palette/resolution masks for each line and build up palette/difference tables */
	new_res = Screen_ComparePaletteMask(STRes);
	/* Did we change resolution this frame - allocate new screen if did so */
	Screen_DidResolutionChange(new_res);
	/* Is need full-update, tag as such */
	if (pFrameBuffer->bFullUpdate)
		Screen_SetFullUpdateMask();

	/* restore area potentially left under overlay led
	 * and saved by Statusbar_OverlayBackup()
	 */
	Statusbar_OverlayRestore(sdlscrn);

	/* Lock screen for direct screen surface format writes */
	if (Screen_Lock())
	{
		bScreenContentsChanged = false;      /* Did change (ie needs blit?) */

		/* Set details */
		Screen_SetConvertDetails();

		/* Clear screen on full update to clear out borders and also interleaved lines */
		if (pFrameBuffer->bFullUpdate)
			Screen_ClearScreen();
		
		/* Call drawing for full-screen */
		pDrawFunction = ScreenDrawFunctionsNormal[STRes];
		/* Check if is Spec512 image */
		if (Spec512_IsImage())
		{
			bPrevFrameWasSpec512 = true;
			/* What mode were we in? Keep to 320xH or 640xH */
			if (pDrawFunction==ConvertLowRes_320x16Bit)
				pDrawFunction = ConvertLowRes_320x16Bit_Spec;
			else if (pDrawFunction==ConvertLowRes_640x16Bit)
				pDrawFunction = ConvertLowRes_640x16Bit_Spec;
			else if (pDrawFunction==ConvertLowRes_320x32Bit)
				pDrawFunction = ConvertLowRes_320x32Bit_Spec;
			else if (pDrawFunction==ConvertLowRes_640x32Bit)
				pDrawFunction = ConvertLowRes_640x32Bit_Spec;
			else if (pDrawFunction==ConvertMediumRes_640x32Bit)
				pDrawFunction = ConvertMediumRes_640x32Bit_Spec;
			else if (pDrawFunction==ConvertMediumRes_640x16Bit)
				pDrawFunction = ConvertMediumRes_640x16Bit_Spec;
		}
		else if (bPrevFrameWasSpec512)
		{
			/* If we switch back from Spec512 mode to normal
			 * screen rendering, we have to make sure to do
			 * a full update of the screen. */
			Screen_SetFullUpdateMask();
			bPrevFrameWasSpec512 = false;
		}

		if (pDrawFunction)
			CALL_VAR(pDrawFunction);

		/* Unlock screen */
		Screen_UnLock();

		/* draw overlay led(s) or statusbar after unlock */
		Statusbar_OverlayBackup(sdlscrn);
		sbar_rect = Statusbar_Update(sdlscrn, false);
		
		/* Clear flags, remember type of overscan as if change need screen full update */
		pFrameBuffer->bFullUpdate = false;
		pFrameBuffer->VerticalOverscanCopy = VerticalOverscan;

		/* And show to user */
		if (bScreenContentsChanged || bForceFlip || sbar_rect)
		{
			Screen_Blit(sbar_rect);
		}

		return bScreenContentsChanged;
	}

	return false;
}


/*-----------------------------------------------------------------------*/
/**
 * Draw ST screen to window/full-screen
 */
bool Screen_Draw(void)
{
	if (bQuitProgram)
	{
		return false;
	}

	/* And draw (if screen contents changed) */
	return Screen_DrawFrame(false);
}

/**
 * This is used to set the size of the SDL screen
 * when we're using the generic conversion functions.
 */
void Screen_SetGenConvSize(int width, int height, int bpp, bool bForceChange)
{
	const bool keep = ConfigureParams.Screen.bKeepResolution;
	int screenwidth, screenheight, maxw, maxh;
	int scalex, scaley, sbarheight;

	if (bpp == 24)
		bpp = 32;

	/* constrain size request to user's desktop size */
	Resolution_GetDesktopSize(&maxw, &maxh);
#if !WITH_SDL2
	scalex = scaley = 1;
	while (width > maxw*scalex) {
		scalex *= 2;
	}
	while (height > maxh*scaley) {
		scaley *= 2;
	}
	if (scalex * scaley > 1) {
		fprintf(stderr, "WARNING: too large screen size %dx%d -> divided by %dx%d!\n",
			width, height, scalex, scaley);
		width /= scalex;
		height /= scaley;
	}
#endif

	Resolution_GetLimits(&maxw, &maxh, &bpp, keep);
	nScreenZoomX = nScreenZoomY = 1;

	if (ConfigureParams.Screen.bAspectCorrect) {
		/* Falcon (and TT) pixel scaling factors seem to 2^x
		 * (quarter/half pixel, interlace/double line), so
		 * do aspect correction as 2's exponent.
		 */
		while (nScreenZoomX*width < height &&
		       2*nScreenZoomX*width < maxw) {
			nScreenZoomX *= 2;
		}
		while (2*nScreenZoomY*height < width &&
		       2*nScreenZoomY*height < maxh) {
			nScreenZoomY *= 2;
		}
		if (nScreenZoomX*nScreenZoomY > 2) {
			fprintf(stderr, "WARNING: strange screen size %dx%d -> aspect corrected by %dx%d!\n",
				width, height, nScreenZoomX, nScreenZoomY);
		}
	}

	/* then select scale as close to target size as possible
	 * without having larger size than it
	 */
	scalex = maxw/(nScreenZoomX*width);
	scaley = maxh/(nScreenZoomY*height);
	if (scalex > 1 && scaley > 1) {
		/* keep aspect ratio */
		if (scalex < scaley) {
			nScreenZoomX *= scalex;
			nScreenZoomY *= scalex;
		} else {
			nScreenZoomX *= scaley;
			nScreenZoomY *= scaley;
		}
	}

	genconv_width_req = width;
	genconv_height_req = height;
	width *= nScreenZoomX;
	height *= nScreenZoomY;

	/* get statusbar size for this screen size */
	sbarheight = Statusbar_GetHeightForSize(width, height);
	screenheight = height + sbarheight;
	screenwidth = width;

#if !WITH_SDL2
	/* get resolution corresponding to these */
	Resolution_Search(&screenwidth, &screenheight, &bpp, keep);
#endif
	/* re-calculate statusbar height for this resolution */
	sbarheight = Statusbar_SetHeight(screenwidth, screenheight-sbarheight);

	genconv_bpp = bpp;
	/* screen area without the statusbar */
	STScreenRect.x = STScreenRect.y = 0;
	STScreenRect.w = screenwidth;
	STScreenRect.h = screenheight - sbarheight;

	if (!Screen_SetSDLVideoSize(screenwidth, screenheight, bpp, bForceChange))
	{
		/* same host screen size despite Atari resolution change,
		 * -> no time consuming host video mode change needed
		 */
		if (screenwidth > width || screenheight > height+sbarheight) {
			/* Atari screen smaller than host -> clear screen */
			Screen_ClearScreen();
			/* re-calculate variables in case height + statusbar height
			 * don't anymore match SDL surface size (there's an assert
			 * for that)
			 */
			Statusbar_Init(sdlscrn);
		}
#if WITH_SDL2
		genconv_do_update = true;
#else
		genconv_do_update = ( sdlscrn->flags & SDL_HWSURFACE ) == 0;
#endif
		return;
	}

	// In case surface format changed, remap the native palette
	Screen_RemapPalette();

	// redraw statusbar
	Statusbar_Init(sdlscrn);

	DEBUGPRINT(("Surface Pitch = %d, width = %d, height = %d\n", sdlscrn->pitch, sdlscrn->w, sdlscrn->h));
	DEBUGPRINT(("Must Lock? %s\n", SDL_MUSTLOCK(sdlscrn) ? "YES" : "NO"));

#if WITH_SDL2
	genconv_do_update = true;
#else
	// is the SDL_update needed?
	genconv_do_update = ( sdlscrn->flags & SDL_HWSURFACE ) == 0;
#endif

	DEBUGPRINT(("Pixel format:bitspp=%d, tmasks r=%04x g=%04x b=%04x"
			", tshifts r=%d g=%d b=%d"
			", tlosses r=%d g=%d b=%d\n",
			sdlscrn->format->BitsPerPixel,
			sdlscrn->format->Rmask, sdlscrn->format->Gmask, sdlscrn->format->Bmask,
			sdlscrn->format->Rshift, sdlscrn->format->Gshift, sdlscrn->format->Bshift,
			sdlscrn->format->Rloss, sdlscrn->format->Gloss, sdlscrn->format->Bloss));

	Main_WarpMouse(sdlscrn->w/2,sdlscrn->h/2, false);
}

void Screen_GenConvUpdate(SDL_Rect *extra, bool forced)
{
	SDL_Rect rects[2];
	int count = 1;

	/* Don't update anything on screen if video output is disabled */
	if ( ConfigureParams.Screen.DisableVideo )
		return;

	if (!forced && !genconv_do_update) // the HW surface is available
		return;
#ifndef __LIBRETRO__
	rects[0] = STScreenRect;
	if (extra) {
		rects[1] = *extra;
		count = 2;
	}
	SDL_UpdateRects(sdlscrn, count, rects);
#endif
}

Uint32 Screen_GetGenConvWidth(void)
{
	return STScreenRect.w;
}

Uint32 Screen_GetGenConvHeight(void)
{
	return STScreenRect.h;
}


/* -------------- screen conversion routines --------------------------------
  Screen conversion routines. We have a number of routines to convert ST screen
  to PC format. We split these into Low, Medium and High each with 8/16-bit
  versions. To gain extra speed, as almost half of the processing time can be
  spent in these routines, we check for any changes from the previously
  displayed frame. AdjustLinePaletteRemap() sets a flag to tell the routines
  if we need to totally update a line (ie full update, or palette/res change)
  or if we just can do a difference check.
  We convert each screen 16 pixels at a time by use of a couple of look-up
  tables. These tables convert from 2-plane format to bbp and then we can add
  two of these together to get 4-planes. This keeps the tables small and thus
  improves speed. We then look these bbp values up as an RGB/Index value to
  copy to the screen.
*/


/*-----------------------------------------------------------------------*/
/**
 * Update the STRGBPalette[] array with current colours for this raster line.
 *
 * Return 'ScrUpdateFlag', 0x80000000=Full update, 0x40000000=Update
 * as palette changed
 */
static int AdjustLinePaletteRemap(int y)
{
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	static const int endiantable[16] = {0,2,1,3,8,10,9,11,4,6,5,7,12,14,13,15};
#endif
	Uint16 *actHBLPal;
	int i;

	/* Copy palette and convert to RGB in display format */
	actHBLPal = pHBLPalettes + (y<<4);    /* offset in palette */
	for (i=0; i<16; i++)
	{
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		STRGBPalette[endiantable[i]] = ST2RGB[*actHBLPal++];
#else
		STRGBPalette[i] = ST2RGB[*actHBLPal++];
#endif
	}
	ScrUpdateFlag = HBLPaletteMasks[y];
	return ScrUpdateFlag;
}


/*-----------------------------------------------------------------------*/
/**
 * Run updates to palette(STRGBPalette[]) until get to screen line
 * we are to convert from
 */
static void Convert_StartFrame(void)
{
	int y = 0;
	/* Get #lines before conversion starts */
	int lines = STScreenStartHorizLine;
	while (lines--)
		AdjustLinePaletteRemap(y++);     /* Update palette */
}

/* lookup tables and conversion macros */
#include "convert/macros.h"

/* Conversion routines */

#include "convert/low320x16.c"		/* LowRes To 320xH x 16-bit color */
#include "convert/low640x16.c"		/* LowRes To 640xH x 16-bit color */
#include "convert/med640x16.c"		/* MediumRes To 640xH x 16-bit color */
#include "convert/low320x16_spec.c"	/* LowRes Spectrum 512 To 320xH x 16-bit color */
#include "convert/low640x16_spec.c"	/* LowRes Spectrum 512 To 640xH x 16-bit color */
#include "convert/med640x16_spec.c"	/* MediumRes Spectrum 512 To 640xH x 16-bit color */

#include "convert/low320x32.c"		/* LowRes To 320xH x 32-bit color */
#include "convert/low640x32.c"		/* LowRes To 640xH x 32-bit color */
#include "convert/med640x32.c"		/* MediumRes To 640xH x 32-bit color */
#include "convert/low320x32_spec.c"	/* LowRes Spectrum 512 To 320xH x 32-bit color */
#include "convert/low640x32_spec.c"	/* LowRes Spectrum 512 To 640xH x 32-bit color */
#include "convert/med640x32_spec.c"	/* MediumRes Spectrum 512 To 640xH x 32-bit color */
