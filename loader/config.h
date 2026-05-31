#ifndef __CONFIG_H__
#define __CONFIG_H__

//#define ENABLE_DEBUG

#define LOAD_ADDRESS 0x98000000

#define SCREEN_W 960
#define SCREEN_H 544

#endif

// UT99 Loader Options
#define USE_SCELIBC_BRIDGE // Use SceLibcBridge for I/O
#define USE_VGL_ALLOCATORS // Use vitaGL memory allocators
#define HAVE_FILEPATH_CACHE // Enable caching for filepaths

#ifndef USE_SCELIBC_BRIDGE
#define sceLibcBridge_fopen fopen
#define sceLibcBridge_fseek fseek
#define sceLibcBridge_ftell ftell
#define sceLibcBridge_fclose fclose
#define sceLibcBridge_fread fread
#define sceLibcBridge_fwrite fwrite
#define sceLibcBridge_feof feof
#define sceLibcBridge_fputc fputc
#define sceLibcBridge_fprintf fprintf
#define sceLibcBridge_ferror ferror
#endif

#ifndef USE_VGL_ALLOCATORS
#define vglMalloc malloc
#define vglFree free
#define vglCalloc calloc
#define vglMemalign memalign
#define vglRealloc realloc
#endif
