

#include "SDL_config.h"

#ifndef _SDL_fbvideo_h
#define _SDL_fbvideo_h

#include <sys/types.h>
#include <termios.h>
#include <linux/fb.h>

#include "SDL_mouse.h"
#include "SDL_mutex.h"
#include "../SDL_sysvideo.h"
#if SDL_INPUT_TSLIB
#include "tslib.h"
#endif

//#define debug_mode

#ifdef debug_mode
	FILE *fp;
#endif
//

/* Hidden "this" pointer for the video functions */
#define _THIS	SDL_VideoDevice *this

typedef void DISPMANX_bitBlit(
		Uint8 *src_pos,
		int src_right_delta,	/* pixels, not bytes */
		int src_down_delta,		/* pixels, not bytes */
		Uint8 *dst_pos,
		int dst_linebytes,
		int width,
		int height);

/* This is the structure we use to keep track of video memory */
typedef struct vidmem_bucket {
	struct vidmem_bucket *prev;
	int used;
	int dirty;
	char *base;
	unsigned int size;
	struct vidmem_bucket *next;
} vidmem_bucket;

/* Private display data */
struct SDL_PrivateVideoData {
	int current_vt;
	int saved_vt;
	int keyboard_fd;
	int saved_kbd_mode;
	struct termios saved_kbd_termios;

	int mouse_fd;
#if SDL_INPUT_TSLIB
	struct tsdev *ts_dev;
#endif
	DISPMANX_bitBlit *blitFunc;
	int physlinebytes; /* Length of a line in bytes in physical fb */

#define NUM_MODELISTS   4               /* 8, 16, 24, and 32 bits-per-pixel */
         int SDL_nummodes[NUM_MODELISTS];
         SDL_Rect **SDL_modelist[NUM_MODELISTS];

	vidmem_bucket surfaces;
	int surfaces_memtotal;
	int surfaces_memleft;

	SDL_mutex *hw_lock;
	Uint32 screen_arealen;
	Uint8 *screen_contents;
	__u16  screen_palette[3*256];

	void (*wait_vbl)(_THIS);
	void (*wait_idle)(_THIS);
};
/* Old variable names */
#define console_fd		(this->hidden->console_fd)
#define current_vt		(this->hidden->current_vt)
#define saved_vt		(this->hidden->saved_vt)
#define keyboard_fd		(this->hidden->keyboard_fd)
#define saved_kbd_mode		(this->hidden->saved_kbd_mode)
#define saved_kbd_termios	(this->hidden->saved_kbd_termios)
#define mouse_fd		(this->hidden->mouse_fd)
#if SDL_INPUT_TSLIB
#define ts_dev			(this->hidden->ts_dev)
#endif
#define cache_modinfo		(this->hidden->cache_modinfo)
#define cache_fbinfo            (this->hidden->cache_fbinfo)
#define saved_modinfo		(this->hidden->saved_modinfo)
#define saved_vinfo             (this->hidden->saved_vinfo)
#define cache_vinfo             (this->hidden->cache_vinfo)
#define saved_cmaplen		(this->hidden->saved_cmaplen)
#define saved_cmap		(this->hidden->saved_cmap)
#define blitFunc		(this->hidden->blitFunc)
#define physlinebytes		(this->hidden->physlinebytes)
#define SDL_nummodes		(this->hidden->SDL_nummodes)
#define SDL_modelist		(this->hidden->SDL_modelist)
#define surfaces		(this->hidden->surfaces)
#define surfaces_memtotal	(this->hidden->surfaces_memtotal)
#define surfaces_memleft	(this->hidden->surfaces_memleft)
#define hw_lock			(this->hidden->hw_lock)
#define switched_away		(this->hidden->switched_away)
#define screen_vinfo		(this->hidden->screen_vinfo)
#define screen_arealen		(this->hidden->screen_arealen)
#define screen_contents		(this->hidden->screen_contents)
#define screen_palette		(this->hidden->screen_palette)
#define wait_vbl		(this->hidden->wait_vbl)
#define wait_idle		(this->hidden->wait_idle)

/* These functions are defined in SDL_fbvideo.c */
extern void DISPMANX_SavePaletteTo(_THIS, int palette_len, __u16 *area);
extern void DISPMANX_RestorePaletteFrom(_THIS, int palette_len, __u16 *area);

/* These are utility functions for working with video surfaces */

static __inline__ void DISPMANX_AddBusySurface(SDL_Surface *surface)
{
	((vidmem_bucket *)surface->hwdata)->dirty = 1;
}

static __inline__ int DISPMANX_IsSurfaceBusy(SDL_Surface *surface)
{
	return ((vidmem_bucket *)surface->hwdata)->dirty;
}

static __inline__ void DISPMANX_WaitBusySurfaces(_THIS)
{
	vidmem_bucket *bucket;

	/* Wait for graphic operations to complete */
	wait_idle(this);

	/* Clear all surface dirty bits */
	for ( bucket=&surfaces; bucket; bucket=bucket->next ) {
		bucket->dirty = 0;
	}
}

#endif /* _SDL_fbvideo_h */
