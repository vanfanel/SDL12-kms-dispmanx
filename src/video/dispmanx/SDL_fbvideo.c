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
#include <bcm_host.h>
//

#ifndef HAVE_GETPAGESIZE
#include <asm/page.h>		/* For definition of PAGE_SIZE */
#endif

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
#define RGB565(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)

/* Initialization/Query functions */
static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int DISPMANX_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void DISPMANX_VideoQuit(_THIS);

/* Hardware surface functions */
static void DISPMANX_WaitVBL(_THIS);
static void DISPMANX_WaitIdle(_THIS);
static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects);
static void DISPMANX_BlankBackground(void);
static int DISPMANX_AddMode(_THIS, unsigned int w, unsigned int h, int index);
static void DISPMANX_FreeResources(void);
static void DISPMANX_FreeBackground (void);

//MAC Variables para la inicializaci√≥n del buffer
int flip_page = 0;

typedef struct {
    //Grupo de variables para dispmanx, o sea, para el tema de elements, resources, rects...
    //Este grupo de variables son para gestionar elementos visuales.


    DISPMANX_DISPLAY_HANDLE_T   display;
    DISPMANX_MODEINFO_T         amode;
    void                       *pixmem;
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
	//MAC Esta funcion no toca nada del framebuffer sino que 
	//solo inicializa la memoria interna de lo que en SDL es una
	//abstraccion del dispositivo.
	#ifdef debug_mode
		printf ("\n[DEBUG] Esperando pulsacion de tecla para gdb remoto...");
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
	this->CheckHWBlit = NULL;
	this->FillHWRect = NULL;
	this->SetHWColorKey = NULL;
	this->SetHWAlpha = NULL;
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

static int DISPMANX_VideoInit(_THIS, SDL_PixelFormat *vformat)
{

#ifdef debug_mode
 fp = fopen("SDL_log.txt","w");
#endif
	
#if !SDL_THREADS_DISABLED
	/* Create the hardware surface lock mutex */
	hw_lock = SDL_CreateMutex();
	if ( hw_lock == NULL ) {
		SDL_SetError("Unable to create lock mutex");
		DISPMANX_VideoQuit(this);
		return(-1);
	}
#endif

	
	/* Enable mouse and keyboard support */
	if ( DISPMANX_OpenKeyboard(this) < 0 ) {
		DISPMANX_VideoQuit(this);
		return(-1);
	}
	if ( DISPMANX_OpenMouse(this) < 0 ) {
		const char *sdl_nomouse;
		//MAC Si esto da problemas, es por los premisos de gpm sobre
		//el raton en /dev/mice. Edita /etc/init.d/gpm y aniade
		//en la seccion start() la linea chmod 0666{MOUSEDEV}
		sdl_nomouse = SDL_getenv("SDL_NOMOUSE");
		if ( ! sdl_nomouse ) {
			printf("\nERR - Couldn't open mouse. Look for permissions in /etc/init.d/gpm.\n");
			DISPMANX_VideoQuit(this);
			return(-1);
		}
	}
	
	//MAC Esto es necesario para que SDL_SetVideoMode de SDL_Video.c (NO DISPMANX_SetVideoMode()) no
	//se piense que tenemos un modo con paleta, porque eso haria que se llamase a DISPMANX_SetColors
	//desde SDL_SetVideoMode(). Esto ocurre porque no es la parte de DISPMANX donde se asigna el 
	//format a mode (que es lo que retorna DISPMANX_SetVideoMode())	sino que nos viene asignado de la 
	//llamada a SDL_CreateRGBSurface() de SDL_VideoInit().
	vformat->BitsPerPixel = 16;
	vformat->Rmask = 0;
	vformat->Gmask = 0;
	vformat->Bmask = 0;
	
	//Pongo pixmem a NULL para saber en DISPMANX_VideoQuit() si hay que liberar las cosas de dispmanx o no.
	//Esto es porque en juegos y emuladores tipo MAME y tal se entra por VideoInit() pero no por SetVideoMode(),
	//donde dispvars->pixmem dejara≠a de ser NULL y entonces si≠tendriamos que liberar cosas.
	dispvars->pixmem = NULL;
		
	/* We're done! */
	return(0);
}

static SDL_Surface *DISPMANX_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
//MAC Recuerda que aqui≠,originalmente, nos llegaban las dimensiones de un modo de video
// aproximado en SDL_Video.c de entre los modos de video disponibles. AHORA YA NO.
//Ahora lo que hacemos es que nos lleguen directamente la altura y anchura del modo 
//en el que quiere correr la aplicacion, 
//Luego se escala ese modo, de cuanta menos resolucion mejor, (ya que hay
//que hacer una escritura de ram a grafica en la funcion FlipHWScreen), al modo fisico, que
//es en realidad el unico modo grafico que existe, el modo en que hemos arrancado.
//Esto explica por que creamos el plano de overlay a parte, 
//ya que cuando SDL_Video.c llama a SetVideoMode aun no se tienen listos los 
//offsets horizontal y vertical donde empieza el modo de video pequenio 
//(el modo en que corre la app internamente) sobre el grande (el modo fisico).
	
	//Si nos pasan width=0 y height=0, interpreto que el programa no quiere video sino
	//que solo necesita entrar en modo grafico, asi que salto alli:
	if ((width == 0) | (height == 0)) goto go_video_console;	

	//MAC Inicializamos el SOC (bcm_host_init) SOLO si no hemos pasado antes por aqui≠. Lo mismo con el fondo.
	//Si ya hemos pasado antes, hacemos limpieza, pero dejamos el fondo sin tocar.
	if (dispvars->pixmem != NULL){
		//Hacemos limpieza de resources, pero dejamos el fondo. No hay problema porque solo lo ponemos
		//si no hemos pasado por aqui antes.
		DISPMANX_FreeResources();	
	}
	else {
    		uint32_t screen = 0;
		
		bcm_host_init();
		
		//MAC Abrimos el display dispmanx
		printf("Dispmanx: Opening display %i\n", screen );
        	dispvars->display = vc_dispmanx_display_open( screen );

		//MAC Recuperamos algunos datos de la configuracion del buffer actual
		vc_dispmanx_display_get_info( dispvars->display, &(dispvars->amode));
		printf( "Dispmanx: Physical video mode is %d x %d\n", 
		dispvars->amode.width, dispvars->amode.height );
		
		//Ponemos el element de fondo negro tanto si se respeta el ratio como si no, 
		//porque si no, se nos vera≠a la consola al cambiar de resolucion durante el programa.
		DISPMANX_BlankBackground();
	}	
	


	//-------Bloque de lista de resoluciones, originalmente en VideoInit--------------
	//Para la aplicacion SDL, el unico modo de video disponible va a ser siempre el que pida. 
	
	DISPMANX_AddMode(this, width, height, (((bpp+7)/8)-1));

	//---------------------------------------------------------------------------------	
	
	Uint32 Rmask;
	Uint32 Gmask;
	Uint32 Bmask;
	
	//dispvars->pitch = width * ((bpp+7) /8);

	//MAC Establecemos el pitch en funcion del bpp deseado
        //Lo alineamos a 16 porque es el aligment interno de dispmanx(en ejemp)
	dispvars->bits_per_pixel = bpp;	
        dispvars->pitch = ( ALIGN_UP( width, 16 ) * (bpp/8) );
        //Alineamos la atura a 16 por el mismo motivo (ver ejemplo hello_disp)
        height = ALIGN_UP( height, 16);

	switch (bpp){
	   case 8:
		dispvars->pix_format = VC_IMAGE_8BPP;	       
		break;
	   
	   case 16:
		dispvars->pix_format = VC_IMAGE_RGB565;	       
		break;

	   case 32:
		dispvars->pix_format = VC_IMAGE_XRGB8888;	       
	        break;
           
           default:
	      printf ("\n[ERROR] - wrong bpp: %d\n",bpp);
	      return (NULL);
	}	
	    	
	//MAC blah 
	this->UpdateRects = DISPMANX_DirectUpdate;

	printf ("\nUsing internal program mode: %d x %d %d bpp", 
		width, height, dispvars->bits_per_pixel);	

	//MAC Por ahora en DISPMANX usamos el mismo modo q ya esta establecido
	printf ("\nUsing physical mode: %d x %d %d bpp",
		dispvars->amode.width, dispvars->amode.height,
		dispvars->bits_per_pixel);
	
	//-----------------------------------------------------------------------------
	//Esta parte no es fundamental, solo sirve para conservar el ratio del juego.
	//Si no se hace y simplemente quitas estas lineas, se estira al modo fisico y ya, 
	//quedando la imagen deformada si es de 4:3 en una tele de 16:9, que es lo que pasaba antes.	
	//Simplemente hallamos ese ratio y con el hallamos la nueva anchura, considerando
	//como altura la maxima fisica que tenemos establecida, o sea, la altura del modo fisico establecido. 
	//Tambien se calcula la posicion horizontal en que debe empezar el rect de destino (dst_ypos), 
	//para que no quede pegado a la parte izquierda de la pantalla al ser menor que la resolucion fisica, que
	//obviamente no cambia. 
	//Queda obsoleto si cambiamos la resolucion a una que tenga el mismo ratio que el modo original del juego.
	
	dispvars->ignore_ratio = (int) SDL_getenv("SDL_DISPMANX_IGNORE_RATIO");

	if (dispvars->ignore_ratio)
		vc_dispmanx_rect_set( &(dispvars->dst_rect), 0, 0, 
	   		dispvars->amode.width , dispvars->amode.height );
	else {
		float orig_ratio = ((float)width / (float)height); 
		int dst_width = dispvars->amode.height * orig_ratio;	
		
		//Si la anchura de la imagen escalada nos sale mayor que el ancho fisico de pantalla,
		//mantenemos el ancho fisico de pantalla como anchura maxima.
		if (dst_width > dispvars->amode.width) dst_width = dispvars->amode.width;

		int dst_ypos  = (dispvars->amode.width - dst_width) / 2; 
		printf ("\nUsing proportion ratio: %d / %d = %f", width, height, orig_ratio);
		printf ("\nProgram rect, respecting original ratio: %d x %d \n", 
		dst_width, dispvars->amode.height);

		vc_dispmanx_rect_set( &(dispvars->dst_rect), dst_ypos, 0, 
	   		dst_width , dispvars->amode.height );
			
	}

	//---------------------------Dejamos configurados los rects---------------------
	//Recuerda que los rects NO contienen ninguna informacion visual, solo son tamanio, rectangulos
	//descritos para que los entiendan las funciones vc, solo tamanios de areas.
	//
	//bmp_rect: se usa solo para el volcado del buffer en RAM al resource que toque. Define el tamanio
	//del area a copiar de RAM (pixmem) al resource (dispmam->resources[]) usando write_data(), por
	//eso, y para acabarlo de entender del todo, su altura y anchura son las internas del juego, width y height.
	//
	//src_rect y dst_rect: se usan porque un element necesita dos rects definidos: src_rect es el tamanio del area
	//de entrada,o sea, el tamanio con el que clipeamos la imagen de origen, y dst_rect es el tamanio del area de
	//salida, o sea, el tamanio con que se vera, escalada por hardware, en el element.
	//
	//Por todo esto, src_rect tendra generalmente la altura y anchura de la imagen original, o dicho de otro
	//modo la altura y anchura que usa el juego internamente (width << 16 y height << 16 por algun rollo de
	//tamanio de variable), y dst_rect tendra las dimensiones del area de pantalla a la que queremos escalar
	//esa imagen: si le damos las dimensiones fisicas totales de la pantalla, escalara sin respetar el ratio.   
	//Asi≠que lo he corregido manteniendo la altura maxima de la pantalla fisica, y calculando la anchura
	//a partir de dicha altura y el ratio de la imagen (de la resolucion del juego) original.
	//
	//Debes pensar siempre de la siguiente manera: un element, que es como un cristal-lupa, un resource 
	//(aunque tengas dos, en un momento dado el element solo tiene uno) que es como la imagen original,
	//muy pequenita al fondo, y un "embudo", cuyo tamanio del extremo inferior pegado a la imagen original 
	//es de tamanio src_rect, y cuyo tamanio del extremo superior, pegado al element, es de tamanio dst_rect.
	
	vc_dispmanx_rect_set (&(dispvars->bmp_rect), 0, 0, 
	   width, height);	
	
	vc_dispmanx_rect_set (&(dispvars->src_rect), 0, 0, 
	   width << 16, height << 16);	

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
	
	dispvars->resources[1] = vc_dispmanx_resource_create( 
	   dispvars->pix_format, width, height,
	   &(dispvars->vc_image_ptr) );
	
	//Reservo memoria para el array de pixles en RAM 
    	dispvars->pixmem = calloc( 1, dispvars->pitch * height);
    	//dispvars->pixmem=malloc ( dispvars->pitch * dispvars->amode.height );

	//MAC Esta llamada a ReallocFormat es lo que impedia≠a ver algo...
	Rmask = 0;
	Gmask = 0;
	Bmask = 0;
	if ( ! SDL_ReallocFormat(current, bpp, Rmask, Gmask, Bmask, 0) ) {
		return(NULL);
	}
	
	//Preparamos SDL para trabajar sobre el nuevo framebuffer

	//No queremos HWSURFACEs por la manera en que funciona nuestro backend, ya que la app solo
	//debe conocer el buffer en RAM para que las actualizaciones no sean bloqueantes.
	//TAMPOCO queremos DOUBLEBUFFER: realmente piensa lo que estas haciendo: actualizas la 
	//superficie de video, que esta en la RAM, copias a VRAM y, saltandote las normas del API,
	//esperas a evento de vsync para hacer el buffer swapping. Asi≠ que la app NO SABE NADA de 
	//double buffering ni debe saberlo. UpdateRect() debe hacer lo que antes hacia≠a FlipHWSurface,
	//ya que de cara a la APP, solo hay una actualizacion del buffer de dibujado, NO de pantalla,
	//ya que carecemos de acceso directo a la VRAM.
	//Permitimos HWPALETTEs, cosa que solo se activa si el juego pide un modo de 8bpp porque,
	//tanto si conseguimos modificar la paleta por hard como si tenemos que indexar los valores
	//como estamos haciendo hasta ahora emulando asi la paleta, nos interesa que los juegos
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

	current->pitch  = dispvars->pitch;
	current->pixels = dispvars->pixmem;
	
	//DISPMANX_FreeHWSurfaces(this);
	//DISPMANX_InitHWSurfaces(this, current, surfaces_mem, surfaces_len);
	
	//this->screen = current;
	//this->screen = NULL;

	//Aniadimos el element.
	dispvars->update = vc_dispmanx_update_start( 0 );
	
	dispvars->element = vc_dispmanx_element_add( dispvars->update, 
	   dispvars->display, 0 /*layer*/, &(dispvars->dst_rect), 	   
	   dispvars->resources[flip_page], &(dispvars->src_rect), 
	   DISPMANX_PROTECTION_NONE, dispvars->alpha, 0 /*clamp*/, 
	   /*VC_IMAGE_ROT0*/ 0 );
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
	
	/* We're done */
	//MAC Disable graphics 1
	//Aqui ponemos la terminal en modo grafico. Ya no se imprimiran mas mensajes en la consola a partir de aqui. 
	go_video_console:
	if ( DISPMANX_EnterGraphicsMode(this) < 0 )
        	return(NULL);

	
	return(current);
}

/*static int enter_bpp (_THIS, int bpp){
	//MAC Esta funcion es un poco chapuza porque cambia el modo de video a un bpp a traves del viejo
	//interface de fbdev. Por eso lo primero es abrir el file descriptor del fb. Si se va a usar, eso
	//se debera≠a pasar a SetVideoMode(), en la parte donde se evalua el bpp.
	 
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
	
	return 1;
}*/

static void DISPMANX_BlankBackground(void)
{
  //MAC: Funcion que simplemente pone un element nuevo cuyo resource es de un solo pixel de color negro,
  //se escala a pantalla completa y listo.
  
  // we create a 1x1 black pixel image that is added to display just behind video

  VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;
  uint32_t vc_image_ptr;
  uint16_t image = 0x0000; // black

  VC_RECT_T dst_rect, src_rect;

  dispvars->b_resource = vc_dispmanx_resource_create( type, 1 /*width*/, 1 /*height*/, &vc_image_ptr );

  vc_dispmanx_rect_set( &dst_rect, 0, 0, 1, 1);

  vc_dispmanx_resource_write_data( dispvars->b_resource, type, sizeof(image), &image, &dst_rect );

  vc_dispmanx_rect_set( &src_rect, 0, 0, 1<<16, 1<<16);
  vc_dispmanx_rect_set( &dst_rect, 0, 0, 0, 0);

  dispvars->b_update = vc_dispmanx_update_start(0);

  dispvars->b_element = vc_dispmanx_element_add(dispvars->b_update, dispvars->display, -1 /*layer*/, &dst_rect, 
	dispvars->b_resource, &src_rect, DISPMANX_PROTECTION_NONE, NULL, NULL, (DISPMANX_TRANSFORM_T)0 );
  
  vc_dispmanx_update_submit_sync( dispvars->b_update );
}

static void DISPMANX_WaitVBL(_THIS)
{
	return;
}

static void DISPMANX_WaitIdle(_THIS)
{
	return;
}

#define BLOCKSIZE_W 32
#define BLOCKSIZE_H 32

static void DISPMANX_DirectUpdate(_THIS, int numrects, SDL_Rect *rects)
{	
	//En OpenGL tambiÈn va asÌ≠. No deberÌamos esperar para cambiar el buffer, de hecho la aplicaciÛn
	//piensa que no hay doble buffer (hasta hemos desactivado el double buffer), sino que en lugar
	//de esperar, simplemente deberÌamos actualizar la superficie visible directamente... Pero no
	//puedo hacer eso porque no tengo acceso a la superficie visible: tengo la superficie de vÌdeo en RAM
	//y cada vez que se hace un cambio (o sea, cada vez que llego aquÌ≠) copio esa superficie a la VRAM
	//y espero a cambiar los buffers para no tener tearing, a pesar de que esta funciÛn se supone que no
	//hace eso. Pero en OpenGL se hace lo mismo ya que la ˙nica manera de mostrar los cambios es hacer
	//un GL_SWAP_BUFFERS que tambiÈn es bloqueante. 
	//Volcamos desde el ram bitmap buffer al dispmanx resource buffer que toque. cada vez a uno.	
	vc_dispmanx_resource_write_data( dispvars->resources[flip_page], 
	   dispvars->pix_format, dispvars->pitch, dispvars->pixmem, 
	   &(dispvars->bmp_rect) );
	//Empieza actualizaciÛn
	dispvars->update = vc_dispmanx_update_start( 0 );

	vc_dispmanx_element_change_source(dispvars->update, 
	   dispvars->element, dispvars->resources[flip_page]);
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
	//vc_dispmanx_update_submit(dispvars->update, NULL, NULL); 
	//Acaba actualizaciÛn
	flip_page = !flip_page;
	
	return;
}

/*static void DISPMANX_DirectUpdate8bpp(_THIS, int numrects, SDL_Rect *rects)
{	
	//Version que incluye conversion de 8bpp paletado a 32bpp directos.
	//NO OLVIDES recoger dispvars->shadowpal en SetColors si usas esto.
	int i,p;
	int32_t red, green, blue;
	int npixels = dispvars->bmp_rect.width * dispvars->bmp_rect.height;

	for (i = 0; i < (npixels); i++){
			//p es simplemente un indice. Nos sirve como entero decimal.
			p = (int) *((char*)dispvars->pixmem+i);
			
			//Los 5 bits de mas peso para R	
			red = dispvars->shadowpal[p].r; 			
			red <<= 16 ;
				
			//Los de en medio para G	
			green = dispvars->shadowpal[p].g; 
			green <<= 8;
				
			//Los de menos peso, mas a la derecha, para B
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
	int i;
	static unsigned short pal[256];
	
	//Set up the colormap
	for (i = 0; i < ncolors; i++) {
		pal[i] = RGB565 ((colors[i]).r, (colors[i]).g, (colors[i]).b);
	}
	vc_dispmanx_resource_set_palette(  dispvars->resources[flip_page], pal, 0, sizeof pal );
	vc_dispmanx_resource_set_palette(  dispvars->resources[!flip_page], pal, 0, sizeof pal );

	return(1);
}


static int DISPMANX_AddMode(_THIS, unsigned int w, unsigned int h, int index)
{
          int i;

	  //S√≥lo metemos un modo de v√≠deo en cada momento, as√≠ que empezamos con la limpieza.
	  for (i=0; i < NUM_MODELISTS; ++i){
		free(SDL_modelist[i]);
		SDL_modelist[i] = NULL;
		SDL_nummodes[i] = 0;
	  }

	  //NUM_MODELISTS est√° definido como 4, para 8, 16, 24 y 32 bpp. Pero no lo usamos.
	  SDL_Rect *mode;
	  mode = (SDL_Rect *)SDL_malloc(sizeof *mode);
	  mode->x = 0;
       	  mode->y = 0;
          mode->w = w;
          mode->h = h;	  
		
	  SDL_modelist[index] = (SDL_Rect **)
 	  SDL_realloc(SDL_modelist[index], sizeof(SDL_Rect *));

	  SDL_nummodes[index] = 1;
	  SDL_modelist[index][0] = mode;
	  SDL_modelist[index][1] = NULL;
	  //Ponemos el miembro siguiente a NULL porque as√≠ es como se cierra la lista y los programas esperan eso.
 
	  return (0); 
}

static SDL_Rect **DISPMANX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return(SDL_modelist[((format->BitsPerPixel+7)/8)-1]);
}

static void DISPMANX_FreeResources(void){
	free (dispvars->pixmem);
	      
	//MAC liberamos lo relacionado con dispmanx
	dispvars->update = vc_dispmanx_update_start( 0 );
    	
    	vc_dispmanx_resource_delete( dispvars->resources[0] );
    	vc_dispmanx_resource_delete( dispvars->resources[1] );
	vc_dispmanx_element_remove(dispvars->update, dispvars->element);
	
	vc_dispmanx_update_submit_sync( dispvars->update );		
}

static void DISPMANX_FreeBackground (void) {
	dispvars->b_update = vc_dispmanx_update_start( 0 );
    	
	vc_dispmanx_resource_delete( dispvars->b_resource );
	vc_dispmanx_element_remove ( dispvars->b_update, dispvars->b_element);
	
	vc_dispmanx_update_submit_sync( dispvars->b_update );
}

static void DISPMANX_VideoQuit(_THIS)
{
	/* Clear the lock mutex */
	if ( hw_lock ) {
		SDL_DestroyMutex(hw_lock);
		hw_lock = NULL;
	}

	if (dispvars->pixmem != NULL){ 
		int i;
		for (i=0; i < NUM_MODELISTS; ++i){
			free(SDL_modelist[i]);
			SDL_modelist[i] = NULL;
			SDL_nummodes[i] = 0;
	  	}
		DISPMANX_FreeResources();
		DISPMANX_FreeBackground();
		vc_dispmanx_display_close( dispvars->display );
		bcm_host_deinit();
	}

	DISPMANX_CloseMouse(this);
	DISPMANX_CloseKeyboard(this);
	
	//MAC Set custom video mode block 2
	//Reestablecemos el modo de video original
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
