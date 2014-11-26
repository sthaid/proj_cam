#include <wc.h>
#include <SDL.h>

int main(int argc, char **argv)
{
    SDL_Init( SDL_INIT_VIDEO );
    TTF_Init();

    INFO("HELLO WORLD\n");
    ERROR("HELLO WORLD\n");
    PRINTF("HELLO WORLD\n");

    sleep(5);

    WARN("CALL JPEG_DECODE %d\n", 555);
    uint8_t * out_buf;
    uint32_t width, height;
    jpeg_decode(999,       // uint32_t cxid, 
                0,         // uint32_t jpeg_decode_mode, 
                NULL,      // uint8_t * jpeg, 
                0,         // uint32_t jpeg_size,
                &out_buf,   // uint8_t ** out_buf, 
                &width,     // uint32_t * width, 
                &height);   // uint32_t * height)
    WARN("BACK FROM JPEG_DECODE %d\n", 555);

    sleep(5);

    TTF_Quit();
    SDL_Quit();

    return 0;
}

#if 0
#include "stdbool.h"
#include "unistd.h"
#include "SDL.h"
#include "SDL_ttf.h"

#include "jpeglib.h"

const int WINDOW_WIDTH = 640;
const int WINDOW_HEIGHT = 480;
const char* WINDOW_TITLE = "SDL Start";

int main(int argc, char **argv)
{
   SDL_Init( SDL_INIT_VIDEO );
   TTF_Init();

#if 0
    if (!(screen = SDL_SetVideoMode(WIDTH, HEIGHT, DEPTH, SDL_FULLSCREEN|SDL_HWSURFACE)))
    {
        SDL_Quit();
        return 1;
    }
#else
    // create the window like normal
    SDL_Window * window = SDL_CreateWindow("SDL2 Example",
                               SDL_WINDOWPOS_UNDEFINED,
                               SDL_WINDOWPOS_UNDEFINED, 640, 480,
                               0);

    // but instead of creating a renderer, we can draw directly to the screen
    SDL_Surface * screen = SDL_GetWindowSurface(window);
#endif


   // SDL_WM_SetCaption( WINDOW_TITLE, 0 );

   TTF_Font* font = TTF_OpenFont("/system/fonts/Arial.ttf", 24);

   if (font == NULL) {
      char buf[1000];
      SDL_Log("NO FONT - '%s'", getcwd(buf,1000));
      //printf("NO FONT\n");
      return 1;
   }

   SDL_Color foregroundColor = { 255, 255, 255 }; 
   SDL_Color backgroundColor = { 0, 0, 255 };

   SDL_Surface* textSurface = TTF_RenderText_Shaded(font, "XXXXXXX This is my text.", 
      foregroundColor, backgroundColor);

   // Pass zero for width and height to draw the whole surface 
   SDL_Rect textLocation = { 100, 100, 0, 0 };

   SDL_Event event;
   bool gameRunning = true;

   while (gameRunning)
   {
      if (SDL_PollEvent(&event))
      {
         if (event.type == SDL_QUIT)
         {
            gameRunning = false;
         }
      }

      SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

      SDL_BlitSurface(textSurface, NULL, screen, &textLocation);

#if 0
      SDL_Flip(screen);
#else
      SDL_UpdateWindowSurface(window);
#endif
   }

   SDL_FreeSurface(textSurface);

   TTF_CloseFont(font);

   TTF_Quit();

   SDL_Quit();

   jpeg_start_decompress(NULL);

   return 0;

}

#endif
