#include "SDL_config.h"

/* Dispmanx based SDL video driver implementation.
*  SDL - Simple DirectMedia Layer
*  Copyright (C) 1997-2012 Sam Lantinga
*  
*  SDL dispmanx backend
*  Copyright (C) 2012 Manuel Alfayate
*/

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

//MAC includes
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "bcm_host.h"
//

#ifndef HAVE_GETPAGESIZE
#include <asm/page.h>		/* For definition of PAGE_SIZE */
#endif
//#define ALIGN_UP(x,y)  ((x + (y)-1) & ~((y)-1))
#include <linux/vt.h>

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "SDL_fbvideo.h"
#include "SDL_fbmouse_c.h"
#include "SDL_fbevents_c.h"

#define min(a,b) ((a)<(b)?(a):(b))

/* Initialization/Query functions */
static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void DISPMANX_VideoQuit(_THIS);

/* Hardware surface functions */
static int DISPMANX_InitHWSurfaces(_THIS, SDL_Surface *screen, char *base, int size);
static void DISPMANX_FreeHWSurfaces(_THIS);
static int DISPMANX_AllocHWSurface(_THIS, SDL_Surface *surface);
static int DISPMANX_LockHWSurface(_THIS, SDL_Surface *surface);
static void DISPMANX_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void DISPMANX_FreeHWSurface(_THIS, SDL_Surface *surface);
static void DISPMANX_WaitVBL(_THIS);
static void DISPMANX_WaitIdle(_THIS);
static int DISPMANX_FlipHWSurface(_THIS, SDL_Surface *surface);
static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects);
static void blank_background(void);
//static int enter_bpp (_THIS, int bpp);

//MAC Variables para la inicialización del buffer
int flip_page = 0;

typedef struct {
    //Grupo de variables para dispmanx, o sea, para el tema de elements, resources, rects...
    //Este grupo de variables son para gestionar elementos visuales.

    DISPMANX_DISPLAY_HANDLE_T   display;
    DISPMANX_MODEINFO_T         amode;
    void                       *pixmem;
    void		       *shadowmem;
    DISPMANX_UPDATE_HANDLE_T    update;
    DISPMANX_RESOURCE_HANDLE_T  resources[2];
    DISPMANX_ELEMENT_HANDLE_T   element;
    VC_IMAGE_TYPE_T 		pix_format;
    uint32_t                    vc_image_ptr;
    VC_DISPMANX_ALPHA_T	       *alpha;    
    VC_RECT_T       src_rect;
    VC_RECT_T 	    dst_rect; 
    VC_RECT_T	    bmp_rect;
    int bits_per_pixel;
    int pitch;
    
    //Grupo de variables para el element en negro de fondo
    DISPMANX_RESOURCE_HANDLE_T  b_resource;
    DISPMANX_ELEMENT_HANDLE_T   b_element;
    DISPMANX_UPDATE_HANDLE_T    b_update;	
    
    //Variable para saber si el usuario ha seteado la varable de entorno de ignorar ratio
    int ignore_ratio;

    //Grupo de variables para el vc_tv_service, que se usan para cosas como recuperar el modo gráfico físico,
    //la lista de modos disponibles, establecer nuevo modo físico...No mezclar con dispmanx que es para 
    //elementos visuales. Están en la misma estructura por no definir otra.
    //Este tipo de variables las defino y uso localmente, pero algunas necesito tenerlas 
    //globalmente accesibles, como original_tv_state, que servirá para reestablecer el modo físico original al salir. 
    //int isResChanged;    
    //TV_DISPLAY_STATE_T *original_tv_state;
    //Paleta auxiliar para poder hacer la conversión de 8bpp a 32bpp.Su valor se recoge en SetColors().
    //SDL_Color* shadowpal;
	
} __DISPMAN_VARIABLES_T;


static __DISPMAN_VARIABLES_T _DISPMAN_VARS;
static __DISPMAN_VARIABLES_T *dispvars = &_DISPMAN_VARS;

/* FB driver bootstrap functions */

static int DISPMANX_Available(void)
{
	//MAC Hacemos que retorme siempre true. Buena gana de comprobar esto.
	return (1);
}

static void DISPMANX_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *DISPMANX_CreateDevice(int devindex)
{
	//MAC Esta función no toca nada del framebuffer sino que 
	//sólo inicializa la memoria interna de lo que en SDL es una
	//abstracción del dispositivo.
	#ifdef debug_mode
		printf ("\n[DEBUG][DEBUG] Esperando pulsación de tecla para gdb remoto...");
		getchar();
	#endif


	SDL_VideoDevice *this;

	/* Initialize all variables that we clean on shutdown */
	this = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));
	wait_vbl = DISPMANX_WaitVBL;
	wait_idle = DISPMANX_WaitIdle;
	mouse_fd = -1;
	keyboard_fd = -1;

	/* Set the function pointers */
	this->VideoInit = DISPMANX_VideoInit;
	this->ListModes = DISPMANX_ListModes;
	this->SetVideoMode = DISPMANX_SetVideoMode;
	this->SetColors = DISPMANX_SetColors;
	this->UpdateRects = DISPMANX_DirectUpdate;
	this->VideoQuit = DISPMANX_VideoQuit;
	this->AllocHWSurface = DISPMANX_AllocHWSurface;
	this->CheckHWBlit = NULL;
	this->FillHWRect = NULL;
	this->SetHWColorKey = NULL;
	this->SetHWAlpha = NULL;
	this->LockHWSurface = DISPMANX_LockHWSurface;
	this->UnlockHWSurface = DISPMANX_UnlockHWSurface;
	this->FlipHWSurface = DISPMANX_FlipHWSurface;
	this->FreeHWSurface = DISPMANX_FreeHWSurface;
	this->SetCaption = NULL;
	this->SetIcon = NULL;
	this->IconifyWindow = NULL;
	this->GrabInput = NULL;
	this->GetWMInfo = NULL;
	this->InitOSKeymap = DISPMANX_InitOSKeymap;
	this->PumpEvents = DISPMANX_PumpEvents;
	this->CreateYUVOverlay = NULL;	

	this->free = DISPMANX_DeleteDevice;

	return this;
}

VideoBootStrap DISPMANX_bootstrap = {
	"dispmanx", "Dispmanx Raspberry Pi VC",
	DISPMANX_Available, DISPMANX_CreateDevice
};

static int DISPMANX_AddMode(_THIS, int index, unsigned int w, unsigned int h, int check_timings)
{
	SDL_Rect *mode;
	int next_mode;

	// Check to see if we already have this mode
	if ( SDL_nummodes[index] > 0 ) {
		mode = SDL_modelist[index][SDL_nummodes[index]-1];
		if ( (mode->w == w) && (mode->h == h) ) {
			printf("\nWe already have mode %dx%d at %d bytes per pixel\n", w, h, index+1);
			return(0);
		}
	}

	//Set up the new video mode rectangle 
	mode = (SDL_Rect *)SDL_malloc(sizeof *mode);
	if ( mode == NULL ) {
		SDL_OutOfMemory();
		return(-1);
	}
	mode->x = 0;
	mode->y = 0;
	mode->w = w;
	mode->h = h;
	
	// Allocate the new list of modes, and fill in the new mode
	next_mode = SDL_nummodes[index];
	SDL_modelist[index] = (SDL_Rect **)
	       SDL_realloc(SDL_modelist[index], (1+next_mode+1)*sizeof(SDL_Rect *));
	if ( SDL_modelist[index] == NULL ) {
		SDL_OutOfMemory();
		SDL_nummodes[index] = 0;
		SDL_free(mode);
		return(-1);
	}
	SDL_modelist[index][next_mode] = mode;
	SDL_modelist[index][next_mode+1] = NULL;
	SDL_nummodes[index]++;

	return(0);
}

static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
#ifdef debug_mode
 fp = fopen("SDL_log.txt","w");
#endif
	
	int ret = 0;
#if !SDL_THREADS_DISABLED
	/* Create the hardware surface lock mutex */
	hw_lock = SDL_CreateMutex();
	if ( hw_lock == NULL ) {
		SDL_SetError("Unable to create lock mutex");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
#endif
	
	//----------MAC Bloque de inicio del fbdev, que porque luego vamos a cambiar la paleta en modos 8bpp--------
	//----------Supongo que esto se puede condicionar para que sólo se haga para modos de 8bpp------------------
	
	/*
	const char* SDL_fbdev;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;

	
	SDL_fbdev = SDL_getenv("SDL_FBDEV");
	if ( SDL_fbdev == NULL ) {
		SDL_fbdev = "/dev/fb0";
	}
	
	console_fd = open(SDL_fbdev, O_RDWR, 0);
	if ( console_fd < 0 ) {
		DISPMANX_VideoQuit(this);
	}	

	
	// Get the type of video hardware 
	if ( ioctl(console_fd, FBIOGET_FSCREENINFO, &finfo) < 0 ) {
		SDL_SetError("Couldn't get console hardware info");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	switch (finfo.type) {
		case FB_TYPE_PACKED_PIXELS:
		break;
		default:
			SDL_SetError("Unsupported console hardware");
			DISPMANX_VideoQuit(this);
			return(-1);
	}
	switch (finfo.visual) {
		case FB_VISUAL_TRUECOLOR:
		case FB_VISUAL_PSEUDOCOLOR:
		case FB_VISUAL_STATIC_PSEUDOCOLOR:
		case FB_VISUAL_DIRECTCOLOR:
			break;
		default:
			SDL_SetError("Unsupported console hardware");
			DISPMANX_VideoQuit(this);
			return(-1);
	}

	// Determine the current screen depth 
	if ( ioctl(console_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ) {
		SDL_SetError("Couldn't get console pixel format");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	vformat->BitsPerPixel = vinfo.bits_per_pixel;
	if ( vformat->BitsPerPixel < 8 ) {
		// Assuming VGA16, we handle this via a shadow framebuffer 
		vformat->BitsPerPixel = 8;
	}
	for ( i=0; i<vinfo.red.length; ++i ) {
		vformat->Rmask <<= 1;
		vformat->Rmask |= (0x00000001<<vinfo.red.offset);
	}
	for ( i=0; i<vinfo.green.length; ++i ) {
		vformat->Gmask <<= 1;
		vformat->Gmask |= (0x00000001<<vinfo.green.offset);
	}
	for ( i=0; i<vinfo.blue.length; ++i ) {
		vformat->Bmask <<= 1;
		vformat->Bmask |= (0x00000001<<vinfo.blue.offset);
	}
	saved_vinfo = vinfo;

	// Save hardware palette, if needed 
	DISPMANX_SavePalette(this, &finfo, &vinfo);
	
	vinfo.accel_flags = 0;	// Temporarily reserve registers 
	if (ioctl(console_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0) {
		SDL_SetError("No se pudo introducir config de fbdev");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	*/

	//-----------------------------------------------------------------------------------------------



	//MAC Inicializamos el SOC
	bcm_host_init();
		
	//MAC Abrimos el display dispmanx
	uint32_t screen = 0;
	printf("dispmanx: Opening display[%i]...\n", screen );
        dispvars->display = vc_dispmanx_display_open( screen );
	
	//MAC Recuperamos algunos datos de la configuración del buffer actual
	vc_dispmanx_display_get_info( dispvars->display, &(dispvars->amode));
	assert(ret == 0);
	vformat->BitsPerPixel = 16; //Pon lo que quieras.Era para restaurar fb
	
	//MAC Para que las funciones GetVideoInfo() devuelvan un SDL_VideoInfo con contenidos.
	this->info.current_w = dispvars->amode.width;
        this->info.current_h = dispvars->amode.height;
        this->info.wm_available = 0;
        this->info.hw_available = 1;
	this->info.video_mem = 32768 /1024;
		
	printf( "Physical video mode is %d x %d\n", 
	   dispvars->amode.width, dispvars->amode.height );
	
	/* Enable mouse and keyboard support */
	if ( DISPMANX_OpenKeyboard(this) < 0 ) {
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	if ( DISPMANX_OpenMouse(this) < 0 ) {
		const char *sdl_nomouse;
		//MAC Si esto da problemas, es por los premisos de gpm sobre
		//el ratón en /dev/mice. Edita /etc/init.d/gpm y añade
		//en la sección start() la línea chmod 0666{MOUSEDEV}
		sdl_nomouse = SDL_getenv("SDL_NOMOUSE");
		if ( ! sdl_nomouse ) {
			printf("\nERR - Couldn't open mouse. Look for permissions in /etc/init.d/gpm.\n");
			DISPMANX_VideoQuit(this);
			return(-1);
		}
	}

	/* We're done! */
	return(0);
}

static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return(SDL_modelist[((format->BitsPerPixel+7)/8)-1]);
}

static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
//MAC Recuerda que aquí, originalmente, nos llegagan las dimensiones de un modo de vídeo
// aproximado en SDL_Video.c de entre los modos de vídeo disponibles. AHORA YA NO.
//Ahora lo que hacemos es que nos lleguen directamente la altura y anchura del modo 
//en el que quiere correr la aplicación, 
//Luego se escala ese modo, de cuanta menos resolución mejor, (ya que hay
//que hacer una escritura de ram a grafica en la función FlipHWScreen), al modo físico, que
//es en realidad el único modo gráfico que existe, el modo en que hemos arrancado.
//Esto explica por qué creamos el plano de overlay a parte, 
//ya que cuando SDL_Video.c llama a SetVideoMode aún no se tienen listos los 
//offsets horizontal y vertical donde empieza el modo de vídeo pequeño 
//(el modo en que corre la app internamente) sobre el grande (el modo físico).
//Otra cosa es la tasa de refresco. Tendrás que usar modos físicos 
//concretos (config.txt) para ajustarte a 50, 60 o 70 Hz.


	//-------Bloque de lista de resoluciones, originalmente en VideoInit--------------
	/* Limpiamos LAS listas de modos disponibles */
	int i;
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		SDL_nummodes[i] = 0;
		SDL_modelist[i] = NULL;
	}	
	
	//Para la aplicación SDL, el modo de vídeo disponible 
	//va a ser siempre el que pida. Se añade el modo que pide la aplicación,
	//que es el tamaño que tendrán los resources, y listo. 

	for (i = 0; i < NUM_MODELISTS; i++){
              //Añado cada modo a la lista 0 (8bpp), lista 1 (16), lista 2(24)..
              //Por eso itero hasta NUM_MODELIST: cada MODELIST es para un bpp.
              DISPMANX_AddMode(this, i, dispvars->amode.width, 
	          dispvars->amode.height, 0);
              printf("Adding video mode: %d x %d - %d bpp\n", width,
                 height, (i+1)*8);
        }
	//---------------------------------------------------------------------------------	
	
	Uint32 Rmask;
	Uint32 Gmask;
	Uint32 Bmask;
	char *surfaces_mem; 
	int surfaces_len;
		
	dispvars->bits_per_pixel = bpp;	
	
	//MAC Establecemos el pitch en función del bpp deseado	
    	//Lo alineamos a 16 porque es el aligment interno de dispmanx(en ejemp)
	dispvars->pitch = ( ALIGN_UP( width, 16 ) * (bpp/8) );
	//Alineamos la atura a 16 por el mismo motivo (ver ejemplo hello_disp)
	height = ALIGN_UP( height, 16);

	switch (bpp){
	   case 8:
		dispvars->pix_format = VC_IMAGE_8BPP;	       
		//dispvars->pix_format = VC_IMAGE_XRGB8888;	       
		//Reservamos 4 bytes por pixel (32 bits por pixel)
		//dispvars->shadowmem = calloc(1, dispvars->pitch * 4 * height);
		break;
	   
	   case 16:
		dispvars->pix_format = VC_IMAGE_RGB565;	       
		break;

	   case 32:
		dispvars->pix_format = VC_IMAGE_XRGB8888;	       
	        break;
           
           default:
	      printf ("\nERR - wrong bpp: %d\n",bpp);
	      return (NULL);
	}	

	
	//-------------Bloque de preparación de buffer fbdev para luego poder cambiar la paleta.
	/*	
	struct fb_fix_screeninfo finfo;
        struct fb_var_screeninfo vinfo;

	int i;
	//MAC Mover esto a condicional de 8bpp
	DISPMANX_RestorePalette(this);
	// Set the video mode and get the final screen format
	if ( ioctl(console_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ) {
		SDL_SetError("Couldn't get console screen info");
		return(NULL);
	}	
	vinfo.activate = FB_ACTIVATE_NOW;
	vinfo.accel_flags = 0;
	vinfo.bits_per_pixel = bpp;
	vinfo.xres = width;
	vinfo.xres_virtual = width;
	vinfo.yres = height;
	vinfo.yres_virtual = height;
	vinfo.xoffset = 0;
	vinfo.yoffset = 0;
	vinfo.red.length = vinfo.red.offset = 0;
	vinfo.green.length = vinfo.green.offset = 0;
	vinfo.blue.length = vinfo.blue.offset = 0;
	//Ponemos la resolución mediante el interface fbcon
	if ( ioctl(console_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0 ) {
			SDL_SetError("Couldn't set console screen info");
			return(NULL);
	}
	vinfo.transp.length = vinfo.transp.offset = 0;
	cache_vinfo = vinfo;
	
	Rmask = 0;
	for ( i=0; i<vinfo.red.length; ++i ) {
		Rmask <<= 1;
		Rmask |= (0x00000001<<vinfo.red.offset);
	}
	Gmask = 0;
	for ( i=0; i<vinfo.green.length; ++i ) {
		Gmask <<= 1;
		Gmask |= (0x00000001<<vinfo.green.offset);
	}
	Bmask = 0;
	for ( i=0; i<vinfo.blue.length; ++i ) {
		Bmask <<= 1;
		Bmask |= (0x00000001<<vinfo.blue.offset);
	}
	if ( ! SDL_ReallocFormat(current, vinfo.bits_per_pixel,
	                                  Rmask, Gmask, Bmask, 0) ) {
		return(NULL);
	}
	//Get the fixed information about the console hardware.
	//This is necessary since finfo.line_length changes.
	 
	if ( ioctl(console_fd, FBIOGET_FSCREENINFO, &finfo) < 0 ) {
		SDL_SetError("Couldn't get console hardware info");
		return(NULL);
	}

	// Save hardware palette, if needed 
	DISPMANX_SavePalette(this, &finfo, &vinfo);

	*/
	
	//--------------------------------------------------------------------------------------

    	
	//MAC blah 
	this->UpdateRects = DISPMANX_DirectUpdate;

	//MAC Establecemos los rects para el escalado
	//this->offset_x = (dispvars->amode.width - width)/2;
	//this->offset_y = (dispvars->amode.height - height)/2;
	
	printf ("\nUsing internal program mode: %d x %d %d bpp", 
		width, height, dispvars->bits_per_pixel);	

	//MAC Por ahora en DISPMANX usamos el mismo modo q ya está establecido
	printf ("\nUsing physical mode: %d x %d %d bpp",
		dispvars->amode.width, dispvars->amode.height,
		dispvars->bits_per_pixel);
	
	//-----------------------------------------------------------------------------
	//Esta parte no es fundamental, sólo sirve para conservar el ratio del juego.
	//Si no se hace y simplemente quitas estas líneas, se estira al modo físico y ya, 
	//quedando la imágen deformada si es de 4:3 en una tele de 16:9, que es lo que pasaba antes.	
	//Simplemente hallamos ese ratio y con él hallamos la nueva anchura, considerando
	//como altura la máxima física que tenemos establecida, o sea, la altura del modo físico establecido. 
	//También se calcula la posición horizontal en que debe empezar el rect de destino (dst_ypos), 
	//para que no quede pegado a la parte izquierda de la pantalla al ser menor que la resolución física, que
	//obviamente no cambia. 
	//Queda obsoleto si cambiamos la resolución a una que tenga el mismo ratio que el modo original del juego.
	float orig_ratio = ((float)width / (float)height); 
	int dst_width = dispvars->amode.height * orig_ratio;	
	int dst_ypos  = (dispvars->amode.width - dst_width) / 2; 
	printf ("\nEl ratio usado va a ser: %d / %d = %f", width, height, orig_ratio);
	printf ("\nEl tamaño del rect de juego, respetando el ratio original, va a ser: %d x %d \n", 
		dst_width, dispvars->amode.height);
	//------------------------------------------------------------------------------
	
	
	//---------------------------Dejamos configurados los rects---------------------
	//Recuerda que los rects NO contienen ninguna información visual, sólo son tamaño, rectángulos
	//descritos para que los entiendan las funciones vc, sólo tamaños de áreas.
	//
	//bmp_rect: se usa sólo para el volcado del buffer en RAM al resource que toque. Define el tamaño
	//del área a copiar de RAM (pixmem) al resource (dispmam->resources[]) usando write_data(), por
	//eso, y para acabarlo de entender del todo, su altura y anchura son las internas del juego, width y height.
	//
	//src_rect y dst_rect: se usan porque un element necesita dos rects definidos: src_rect es el tamaño del área
	//de entrada, o sea, el tamaño con el que clipeamos la imágen de orígen, y dst_rect es el tamaño del área de
	//salida, o sea, el tamaño con que se verá, escalada por hardware, en el element.
	//
	//Por todo esto, src_rect tendrá generalmente la altura y anchura de la imágen original, o dicho de otro
	//modo la altura y anchura que usa el juego internamente (width << 16 y height << 16 por algún rollo de
	//tamaño de variable), y dst_rect tendrá las dimensiones del área de pantalla a la que queremos escalar
	//esa imágen: si le damos las dimensiones físicas totales de la pantalla, escalará sin respetar el ratio.   
	//Así que lo he corregido manteniendo la altura máxima de la pantalla física, y calculando la anchura
	//a partir de dicha altura y el ratio de la imágen (de la resolución del juego) original.
	//
	//Debes pensar siempre de la siguiente manera: un element, que es como un cristal-lupa, un resource 
	//(aunque tengas dos, en un momento dado el element sólo tiene uno) que es como la imágen original,
	//muy pequeñita al fondo, y un "embudo", cuyo tamaño del extremo inferior pegado a la imágen original 
	//es de tamaño src_rect, y cuyo tamaño del extremo superior, pegado al element, es de tamaño dst_rect.
	
	vc_dispmanx_rect_set (&(dispvars->bmp_rect), 0, 0, 
	   width, height);	
	
	vc_dispmanx_rect_set (&(dispvars->src_rect), 0, 0, 
	   width << 16, height << 16);	

	dispvars->ignore_ratio = SDL_getenv("SDL_DISPMANX_IGNORE_RATIO");
	if (dispvars->ignore_ratio)
		vc_dispmanx_rect_set( &(dispvars->dst_rect), 0, 0, 
	   		dispvars->amode.width , dispvars->amode.height );
	else {
		vc_dispmanx_rect_set( &(dispvars->dst_rect), dst_ypos, 0, 
	   		dst_width , dispvars->amode.height );
		//Colocamos fondo negro	
		blank_background();	
	}
	//------------------------------------------------------------------------------
	
	//MAC Establecemos alpha. Para transparencia descomentar flags con or.
	VC_DISPMANX_ALPHA_T layerAlpha;
	/*layerAlpha.flags = (DISPMANX_FLAGS_ALPHA_FROM_SOURCE | 
           DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS);*/
	layerAlpha.flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS;
	layerAlpha.opacity = 255;
	layerAlpha.mask	   = 0;
	dispvars->alpha = &layerAlpha;
	
	//MAC Creo los resources. Me hacen falta dos para el double buffering
	dispvars->resources[0] = vc_dispmanx_resource_create( 
	   dispvars->pix_format, width, height, 
	   &(dispvars->vc_image_ptr) );
    	assert (dispvars->resources[0]);
	
	dispvars->resources[1] = vc_dispmanx_resource_create( 
	   dispvars->pix_format, width, height,
	   &(dispvars->vc_image_ptr) );
    	assert (dispvars->resources[1]);
	
	//Reservo memoria para el array de pixles en RAM 
    	dispvars->pixmem = calloc( 1, dispvars->pitch * height);
    	//dispvars->pixmem=malloc ( dispvars->pitch * dispvars->amode.height );

	//MAC Esto se usa, como mínimo y que yo sepa, para DirectUpdate
	//cache_modinfo = *modinfo;	
	//cache_fbinfo  = *(drmModeGetFB (fd, fb_id));	
	
	//MAC Esta llamada a ReallocFormat es lo que impedía ver algo...
	Rmask = 0;
	Gmask = 0;
	Bmask = 0;
	if ( ! SDL_ReallocFormat(current, bpp, Rmask, Gmask, Bmask, 0) ) {
		return(NULL);
	}
	
	//Preparamos SDL para trabajar sobre el nuevo framebuffer
	shadow_fb = 0;

	//No queremos HWSURFACEs por la manera en que funciona nuestro backend, ya que la app sólo
	//debe conocer el buffer en RAM para que las actualizaciones no sean bloqueantes.
	//TAMPOCO queremos DOUBLEBUFFER: realmente piensa lo que estás haciendo: actualizas la 
	//superficie de vídeo, que está en la RAM, copias a VRAM y, saltándote las normas del API,
	//esperas a evento de vsync para hacer el buffer swapping. Así que la app NO SABE NADA de 
	//double buffering ni debe saberlo. UpdateRect() debe hacer lo que antes hacía FlipHWSurface,
	//ya que de cara a la APP, sólo hay una actualización del buffer de dibujado, NO de pantalla,
	//ya que carecemos de acceso directo a la VRAM.
	//Permitimos HWPALETTEs, cosa que sólo se activa si el juego pide un modo de 8bpp porque,
	//tanto si conseguimos modificar la paleta por hard como si tenemos que indexar los valores
	//como estamos haciendo hasta ahora emulando así la paleta, nos interesa que los juegos
	//entren en SetColors(), y sin paleta por hardware no entran.
	
	current->flags |= SDL_FULLSCREEN;	
	if (flags & SDL_DOUBLEBUF){
	   current->flags &= ~SDL_DOUBLEBUF;	
	}
	if (flags & SDL_HWSURFACE){
	   current->flags &= ~SDL_HWSURFACE;
	   current->flags |= SDL_SWSURFACE;
	}	
	if (flags & SDL_HWPALETTE)
	   current->flags |= SDL_HWPALETTE;	

	current->w = width;
	current->h = height;

	mapped_mem    = dispvars->pixmem;
	mapped_memlen =  (dispvars->pitch * height); 
	current->pitch  = dispvars->pitch;
	current->pixels = mapped_mem;
	
	/* Set up the information for video surface's pixel memory*/
	surfaces_mem = (char *)current->pixels +
		(dispvars->pitch * height);
	surfaces_len = (mapped_memlen-(surfaces_mem-mapped_mem));
		
	DISPMANX_FreeHWSurfaces(this);
	DISPMANX_InitHWSurfaces(this, current, surfaces_mem, surfaces_len);
	
	this->screen = current;
	this->screen = NULL;

	//Añadimos el element.
	dispvars->update = vc_dispmanx_update_start( 0 );
	
	dispvars->element = vc_dispmanx_element_add( dispvars->update, 
	   dispvars->display, 0 /*layer*/, &(dispvars->dst_rect), 	   
	   dispvars->resources[flip_page], &(dispvars->src_rect), 
	   DISPMANX_PROTECTION_NONE, dispvars->alpha, 0 /*clamp*/, 
	   /*VC_IMAGE_ROT0*/ 0 );
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
	

	
	/* We're done */
	//MAC Disable graphics 1
	//Aquí ponemos la terminal en modo gráfico. Ya no se imprimirán más mensajes en la consola a partir de aquí. 
        printf ("\nDISPMANX_SetVideoMode activating keyboard and exiting");
	if ( DISPMANX_EnterGraphicsMode(this) < 0 )
        	return(NULL);

	
	return(current);
}

/*static int enter_bpp (_THIS, int bpp){
	//MAC Esta función es un poco chapuza porque cambia el modo de vídeo a un bpp a través del viejo
	//interface de fbdev. Por eso lo primero es abrir el file descriptor del fb. Si se va a usar, eso
	//se debería pasar a SetVideoMode(), en la parte donde se evalúa el bpp.
	 
	const char* SDL_fbdev;
	struct fb_var_screeninfo vinfo;
	
	SDL_fbdev = SDL_getenv("SDL_FBDEV");
		
	if ( SDL_fbdev == NULL ) {
		SDL_fbdev = "/dev/fb0";
	}
	
	console_fd = open(SDL_fbdev, O_RDWR, 0);
	if ( console_fd < 0 ) {
		exit (-1);
	}

	if ( ioctl(console_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ) {
		return(-1);
	}

	vinfo.bits_per_pixel = bpp;
	if ( ioctl(console_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0 ){
		return (-1);
	}
	
	#ifdef debug_mode
		fprintf (fp, "\n[INFO][INFO] enter_bpp() Función completada con éxito\n");
	#endif
	return 1;
}*/

static void blank_background(void)
{
  //MAC: Función que simplemente pone un element nuevo cuyo resource es de un sólo píxel de color negro,
  //se escala a pantalla completa y listo.
  
  // we create a 1x1 black pixel image that is added to display just behind video

  VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;
  int             ret;
  uint32_t vc_image_ptr;
  uint16_t image = 0x0000; // black

  VC_RECT_T dst_rect, src_rect;

  dispvars->b_resource = vc_dispmanx_resource_create( type, 1 /*width*/, 1 /*height*/, &vc_image_ptr );
  assert( dispvars->b_resource );

  vc_dispmanx_rect_set( &dst_rect, 0, 0, 1, 1);

  ret = vc_dispmanx_resource_write_data( dispvars->b_resource, type, sizeof(image), &image, &dst_rect );
  assert(ret == 0);

  vc_dispmanx_rect_set( &src_rect, 0, 0, 1<<16, 1<<16);
  vc_dispmanx_rect_set( &dst_rect, 0, 0, 0, 0);

  dispvars->b_update = vc_dispmanx_update_start(0);
  assert(dispvars->b_update);

  dispvars->b_element = vc_dispmanx_element_add(dispvars->b_update, dispvars->display, -1 /*layer*/, &dst_rect, 
	dispvars->b_resource, &src_rect, DISPMANX_PROTECTION_NONE, NULL, NULL, (DISPMANX_TRANSFORM_T)0 );
  assert(dispvars->b_element);
  
  ret = vc_dispmanx_update_submit_sync( dispvars->b_update );
  assert( ret == 0 );
}


static int DISPMANX_InitHWSurfaces(_THIS, SDL_Surface *screen, char *base, int size)
{
	#ifdef debug_mode
		printf("\n[WARNING WARNING] Estamos en modo DEBUG y se escribirá LOG");
	#endif	

	vidmem_bucket *bucket;

	surfaces_memtotal = size;
	surfaces_memleft = size;

	if ( surfaces_memleft > 0 ) {
		bucket = (vidmem_bucket *)SDL_malloc(sizeof(*bucket));
		if ( bucket == NULL ) {
			SDL_OutOfMemory();
			return(-1);
		}
		bucket->prev = &surfaces;
		bucket->used = 0;
		bucket->dirty = 0;
		bucket->base = base;
		bucket->size = size;
		bucket->next = NULL;
	} else {
		bucket = NULL;
	}

	surfaces.prev = NULL;
	surfaces.used = 1;
	surfaces.dirty = 0;
	surfaces.base = screen->pixels;
	surfaces.size = (unsigned int)((long)base - (long)surfaces.base);
	surfaces.next = bucket;
	screen->hwdata = (struct private_hwdata *)&surfaces;
	return(0);
}
static void DISPMANX_FreeHWSurfaces(_THIS)
{
	vidmem_bucket *bucket, *freeable;

	bucket = surfaces.next;
	while ( bucket ) {
		freeable = bucket;
		bucket = bucket->next;
		SDL_free(freeable);
	}
	surfaces.next = NULL;
}

static int DISPMANX_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	vidmem_bucket *bucket;
	int size;
	int extra;

/* Temporarily, we only allow surfaces the same width as display.
   Some blitters require the pitch between two hardware surfaces
   to be the same.  Others have interesting alignment restrictions.
   Until someone who knows these details looks at the code...
*/
if ( surface->pitch > SDL_VideoSurface->pitch ) {
	SDL_SetError("Surface requested wider than screen");
	return(-1);
}
surface->pitch = SDL_VideoSurface->pitch;
	size = surface->h * surface->pitch;
#ifdef FBCON_DEBUG
	fprintf(stderr, "Allocating bucket of %d bytes\n", size);
#endif

	/* Quick check for available mem */
	if ( size > surfaces_memleft ) {
		SDL_SetError("Not enough video memory");
		return(-1);
	}

	/* Search for an empty bucket big enough */
	for ( bucket=&surfaces; bucket; bucket=bucket->next ) {
		if ( ! bucket->used && (size <= bucket->size) ) {
			break;
		}
	}
	if ( bucket == NULL ) {
		SDL_SetError("Video memory too fragmented");
		return(-1);
	}

	/* Create a new bucket for left-over memory */
	extra = (bucket->size - size);
	if ( extra ) {
		vidmem_bucket *newbucket;

#ifdef FBCON_DEBUG
	fprintf(stderr, "Adding new free bucket of %d bytes\n", extra);
#endif
		newbucket = (vidmem_bucket *)SDL_malloc(sizeof(*newbucket));
		if ( newbucket == NULL ) {
			SDL_OutOfMemory();
			return(-1);
		}
		newbucket->prev = bucket;
		newbucket->used = 0;
		newbucket->base = bucket->base+size;
		newbucket->size = extra;
		newbucket->next = bucket->next;
		if ( bucket->next ) {
			bucket->next->prev = newbucket;
		}
		bucket->next = newbucket;
	}

	/* Set the current bucket values and return it! */
	bucket->used = 1;
	bucket->size = size;
	bucket->dirty = 0;
#ifdef FBCON_DEBUG
	fprintf(stderr, "Allocated %d bytes at %p\n", bucket->size, bucket->base);
#endif
	surfaces_memleft -= size;
	surface->pixels = bucket->base;
	surface->hwdata = (struct private_hwdata *)bucket;
	return(0);
}

static void DISPMANX_FreeHWSurface(_THIS, SDL_Surface *surface)
{
	vidmem_bucket *bucket, *freeable;

	/* Look for the bucket in the current list */
	for ( bucket=&surfaces; bucket; bucket=bucket->next ) {
		if ( bucket == (vidmem_bucket *)surface->hwdata ) {
			break;
		}
	}
	if ( bucket && bucket->used ) {
		/* Add the memory back to the total */
#ifdef DGA_DEBUG
	printf("Freeing bucket of %d bytes\n", bucket->size);
#endif
		surfaces_memleft += bucket->size;

		/* Can we merge the space with surrounding buckets? */
		bucket->used = 0;
		if ( bucket->next && ! bucket->next->used ) {
#ifdef DGA_DEBUG
	printf("Merging with next bucket, for %d total bytes\n", bucket->size+bucket->next->size);
#endif
			freeable = bucket->next;
			bucket->size += bucket->next->size;
			bucket->next = bucket->next->next;
			if ( bucket->next ) {
				bucket->next->prev = bucket;
			}
			SDL_free(freeable);
		}
		if ( bucket->prev && ! bucket->prev->used ) {
#ifdef DGA_DEBUG
	printf("Merging with previous bucket, for %d total bytes\n", bucket->prev->size+bucket->size);
#endif
			freeable = bucket;
			bucket->prev->size += bucket->size;
			bucket->prev->next = bucket->next;
			if ( bucket->next ) {
				bucket->next->prev = bucket->prev;
			}
			SDL_free(freeable);
		}
	}
	surface->pixels = NULL;
	surface->hwdata = NULL;
}

static int DISPMANX_LockHWSurface(_THIS, SDL_Surface *surface)
{
	if ( switched_away ) {
		return -2; /* no hardware access */
	}
	if ( surface == this->screen ) {
		SDL_mutexP(hw_lock);
		if ( DISPMANX_IsSurfaceBusy(surface) ) {
			DISPMANX_WaitBusySurfaces(this);
		}
	} else {
		if ( DISPMANX_IsSurfaceBusy(surface) ) {
			DISPMANX_WaitBusySurfaces(this);
		}
	}
	return(0);
}
static void DISPMANX_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
	if ( surface == this->screen ) {
		SDL_mutexV(hw_lock);
	}
}

static void DISPMANX_WaitVBL(_THIS)
{

//MAC Sacado de /usr/include/libdrm/drm.h 
//ioctl(fd, DRM_IOCTL_WAIT_VBLANK, 0);
	return;
}

static void DISPMANX_WaitIdle(_THIS)
{
	return;
}

static int DISPMANX_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	
	//MAC Aquí no hay que entrar para nada porque para la aplicación nosotros no tenemos
	//double buffer, sino que ella actualiza directamente el único buffer que conoce, en RAM
	//usando UpdateRects() que es lo que se llama en SDL_Flip() cuando no hay double buffer y
	//luego nosotros sí que tenemos nuestro doble buffer a base de resources de dispmanx, pero
	//para la app eso es transparente.  
	/*#ifdef debug_mode
		fprintf (fp,"\n[DEBUG] Entrando en FlipHWSurface");
	#endif*/
	
	return (0);
}

#define BLOCKSIZE_W 32
#define BLOCKSIZE_H 32

static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects)
{	
	//En OpenGL también va así. No deberíamos esperar para cambiar el buffer, de hecho la aplicación
	//piensa que no hay doble buffer (hasta hemos desactivado el double buffer), sino que en lugar
	//de esperar, simplemente deberíamos actualizar la superficie visible directamente... Pero no
	//puedo hacer eso porque no tengo acceso a la superficie visible: tengo la superficie de vídeo en RAM
	//y cada vez que se hace un cambio (o sea, cada vez que llego aquí) copio esa superficie a la VRAM
	//y espero a cambiar los buffers para no tener tearing, a pesar de que esta función se supone que no
	//hace eso. Pero en OpenGL se hace lo mismo ya que la única manera de mostrar los cambios es hacer
	//un GL_SWAP_BUFFERS que también es bloqueante. 
	//Volcamos desde el ram bitmap buffer al dispmanx resource buffer que toque. cada vez a uno.	
	vc_dispmanx_resource_write_data( dispvars->resources[flip_page], 
	   dispvars->pix_format, dispvars->pitch, dispvars->pixmem, 
	   &(dispvars->bmp_rect) );
	//**Empieza actualización***
	dispvars->update = vc_dispmanx_update_start( 0 );

	vc_dispmanx_element_change_source(dispvars->update, 
	   dispvars->element, dispvars->resources[flip_page]);
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
	//vc_dispmanx_update_submit(dispvars->update, NULL, NULL); 
	//**Acaba actualización***
	flip_page = !flip_page;
	
	return;
}

/*static void DISPMANX_DirectUpdate8bpp(_THIS, int numrects, SDL_Rect *rects)
{	
	//Versión que incluye conversión de 8bpp paletado a 32bpp directos.
	//NO OLVIDES recoger dispvars->shadowpal en SetColors si usas esto.
	int i,p;
	int32_t red, green, blue;
	int npixels = dispvars->bmp_rect.width * dispvars->bmp_rect.height;

	for (i = 0; i < (npixels); i++){
			//p es simplemente un índice. Nos sirve como entero decimal.
			p = (int) *((char*)dispvars->pixmem+i);
			
			//Los 5 bits de más peso para R	
			red = dispvars->shadowpal[p].r; 			
			red <<= 16 ;
				
			//Los de en medio para G	
			green = dispvars->shadowpal[p].g; 
			green <<= 8;
				
			//Los de menos peso, más a la derecha, para B
			blue = dispvars->shadowpal[p].b; 
			blue <<= 0;				
				
				*((int32_t*)(dispvars->shadowmem)+i) = (0 | red | green | blue); 
	}
	vc_dispmanx_resource_write_data( dispvars->resources[flip_page], 
	   dispvars->pix_format, dispvars->pitch*4, dispvars->shadowmem, 
	   &(dispvars->bmp_rect) );

	dispvars->update = vc_dispmanx_update_start( 0 );

	vc_dispmanx_element_change_source(dispvars->update, 
	   dispvars->element, dispvars->resources[flip_page]);
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
	//vc_dispmanx_update_submit(dispvars->update, NULL, NULL); 
	flip_page = !flip_page;
	
	return;
}*/


/* Note:  If we are terminated, this could be called in the middle of
   another SDL video routine -- notably UpdateRects.
*/

static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	//Esta función está deshabilitada porque de momento no hemos logrado cambiar la paleta física de los
	//resources de dispmanx. Si optas por soportar conversión de 8bpp a 32bpp (en DirectUpdate8bpp)
	//deberás activar al menos la asignación de shadowpal a colors, para tener la paleta que establezca el juego.
	//dispvars->shadowpal = colors;
	return (1);	
/*
	int i;
	__u16 r[256];
	__u16 g[256];
	__u16 b[256];
	struct fb_cmap cmap;
	
	//Set up the colormap
	for (i = 0; i < ncolors; i++) {
		r[i] = colors[i].r << 8;
		g[i] = colors[i].g << 8;
		b[i] = colors[i].b << 8;
	}
	cmap.start = firstcolor;
	cmap.len = ncolors;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	cmap.transp = NULL;
	int ret = ioctl(console_fd, FBIOPUTCMAP, &cmap);
	if( (ret < 0) | !(this->screen->flags & SDL_HWPALETTE) ) {
	        colors = this->screen->format->palette->colors;
		ncolors = this->screen->format->palette->ncolors;
		cmap.start = 0;
		cmap.len = ncolors;
		SDL_memset(r, 0, sizeof(r));
		SDL_memset(g, 0, sizeof(g));
		SDL_memset(b, 0, sizeof(b));
		if ( ioctl(console_fd, FBIOGETCMAP, &cmap) == 0 ) {
			for ( i=ncolors-1; i>=0; --i ) {
				colors[i].r = (r[i]>>8);
				colors[i].g = (g[i]>>8);
				colors[i].b = (b[i]>>8);
			}
		}
		return(0);
	}
	
	#ifdef debug_mode
		fprintf (fp, "\n[INFO][INFO] SetColors() Función completada con éxito!!\n");
	#endif
	return(1);
	*/
}

static void DISPMANX_VideoQuit(_THIS)
{
	int i,j;
		
	if ( this->screen ) {
	   /* Clear screen and tell SDL not to free the pixels */
	   const char *dontClearPixels = SDL_getenv("SDL_FBCON_DONT_CLEAR");
	      //En este caso sí tenemos que limpiar el framebuffer	
	      if ( !dontClearPixels && this->screen->pixels 
	      && DISPMANX_InGraphicsMode(this) ) {
#if defined(__powerpc__) || defined(__ia64__)	
	         /* SIGBUS when using SDL_memset() ?? */
		 Uint8 *rowp = (Uint8 *)this->screen->pixels;
		 int left = this->screen->pitch*this->screen->h;
		 while ( left-- ) { *rowp++ = 0; }
#else
		 SDL_memset(this->screen->pixels,0,
		 this->screen->h*this->screen->pitch);
#endif
	      }
		
	      if ( ((char *)this->screen->pixels >= mapped_mem) &&
	         ( (char *)this->screen->pixels < (mapped_mem+mapped_memlen)) ) 
		    this->screen->pixels = NULL;
		 
	}
	
	/* Clear the lock mutex */
	if ( hw_lock ) {
		SDL_DestroyMutex(hw_lock);
		hw_lock = NULL;
	}

	/* Clean up defined video modes */
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		if ( SDL_modelist[i] != NULL ) {
			for ( j=0; SDL_modelist[i][j]; ++j ) {
				SDL_free(SDL_modelist[i][j]);
			}
			SDL_free(SDL_modelist[i]);
			SDL_modelist[i] = NULL;
		}
	}

	/* Clean up the memory bucket list */
	DISPMANX_FreeHWSurfaces(this);

	/* Unmap the video framebuffer and I/O registers */
	if ( mapped_mem ) {
		munmap(mapped_mem, mapped_memlen);
		mapped_mem = NULL;
	}
	if ( mapped_io ) {
		munmap(mapped_io, mapped_iolen);
		mapped_io = NULL;
	}
		
	//MAC liberamos lo relacionado con dispmanx
	printf ("\nDeleting dispmanx elements;\n");
	dispvars->update = vc_dispmanx_update_start( 0 );
    	assert( dispvars->update );
    	
    	vc_dispmanx_resource_delete( dispvars->resources[0] );
    	vc_dispmanx_resource_delete( dispvars->resources[1] );
	vc_dispmanx_element_remove(dispvars->update, dispvars->element);
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
	
	//----------Quitamos el element y su resource que se usan para el fondo negro.
	if (!dispvars->ignore_ratio){
		dispvars->b_update = vc_dispmanx_update_start( 0 );
    		assert( dispvars->b_update );
    	
		vc_dispmanx_resource_delete( dispvars->b_resource );
		vc_dispmanx_element_remove ( dispvars->b_update, dispvars->b_element);
	
		vc_dispmanx_update_submit_sync( dispvars->b_update );
	}		
	//----------------------------------------------------------------------------
	
	vc_dispmanx_display_close( dispvars->display );
	bcm_host_deinit();

	DISPMANX_CloseMouse(this);
	DISPMANX_CloseKeyboard(this);
	
	//Si hemos cambiado el bpp del framebuffer usando el viejo interface fbdev, lo restauramos a los 16bpp
	/*if (dispvars->pix_format == VC_IMAGE_8BPP)
		enter_bpp (this, 16);
	*/
	//MAC Set custom video mode block 2
	//Reestablecemos el modo de vídeo original
	/*if (dispvars->isResChanged){
		int ret = vc_tv_hdmi_power_on_explicit_new(HDMI_MODE_HDMI, 
		dispvars->original_tv_state->display.hdmi.group,
	 	dispvars->original_tv_state->display.hdmi.mode);
		printf ("\nRestaurando modo original...%s\n", ret == 0 ? "OK " : "FALLO");
	}*/
	
	#ifdef debug_mode
		fclose (fp);
	#endif
	
	exit (0);
}
