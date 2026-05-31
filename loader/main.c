/* main.c -- Unreal Tournament .so loader
 *
 * Copyright (C) 2025 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_net.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <setjmp.h>
#include <xmp.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"

#include "vorbis/vorbisfile.h"
#ifdef USE_SCELIBC_BRIDGE
#include "libc_bridge.h"
#endif

#ifdef HAVE_FILEPATH_CACHE
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#define XXH_NAMESPACE UT99_
#define XXH_memcpy sceClibMemcpy
#define XXH_memset sceClibMemset
#include "xxhash_utils.h"
#endif

#ifdef ENABLE_DEBUG
#define dlog sceClibPrintf
#else
#define dlog
#endif

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static char data_path[256];

static char fake_vm[0x1000];
static char fake_env[0x1000];

enum {
	SYSTEM_DIR,
	MAPS_DIR,
	TEXTURES_DIR,
	SOUNDS_DIR,
	MUSIC_DIR,
	DIRS_NUM
};

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
#ifdef HAVE_FILEPATH_CACHE
	char fullpath[256];
#endif
};

#ifndef HAVE_FILEPATH_CACHE
typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;
android_DIR _dirp;
#else
struct android_dirent cached_dirs[DIRS_NUM][512];
size_t cached_entries[DIRS_NUM] = {};
size_t cached_lists[DIRS_NUM];
#endif

int framecap = 0;

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = 330 * 1024 * 1024;
int sceLibcHeapSize = 2 * 1024 * 1024;
so_module main_mod, bridge_mod;

int ret4() { return 4; }

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, data_path);
	return buf;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef ENABLE_DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

#define  MUTEX_TYPE_NORMAL	 0x0000
#define  MUTEX_TYPE_RECURSIVE  0x4000
#define  MUTEX_TYPE_ERRORCHECK 0x8000

static void init_static_mutex(pthread_mutex_t **mutex)
{
	pthread_mutex_t *mtxMem = NULL;

	switch ((int)*mutex) {
	case MUTEX_TYPE_NORMAL: {
		pthread_mutex_t initTmpNormal = PTHREAD_MUTEX_INITIALIZER;
		mtxMem = vglCalloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpNormal, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_RECURSIVE: {
		pthread_mutex_t initTmpRec = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
		mtxMem = vglCalloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpRec, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_ERRORCHECK: {
		pthread_mutex_t initTmpErr = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
		mtxMem = vglCalloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpErr, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	default:
		break;
	}
}

static void init_static_cond(pthread_cond_t **cond)
{
	if (*cond == NULL) {
		pthread_cond_t initTmp = PTHREAD_COND_INITIALIZER;
		pthread_cond_t *condMem = vglCalloc(1, sizeof(pthread_cond_t));
		sceClibMemcpy(condMem, &initTmp, sizeof(pthread_cond_t));
		*cond = condMem;
	}
}

int pthread_attr_destroy_soloader(pthread_attr_t **attr)
{
	int ret = pthread_attr_destroy(*attr);
	vglFree(*attr);
	return ret;
}

int pthread_attr_getstack_soloader(const pthread_attr_t **attr,
				   void **stackaddr,
				   size_t *stacksize)
{
	return pthread_attr_getstack(*attr, stackaddr, stacksize);
}

__attribute__((unused)) int pthread_condattr_init_soloader(pthread_condattr_t **attr)
{
	*attr = vglCalloc(1, sizeof(pthread_condattr_t));

	return pthread_condattr_init(*attr);
}

__attribute__((unused)) int pthread_condattr_destroy_soloader(pthread_condattr_t **attr)
{
	int ret = pthread_condattr_destroy(*attr);
	vglFree(*attr);
	return ret;
}

int pthread_cond_init_soloader(pthread_cond_t **cond,
				   const pthread_condattr_t **attr)
{
	*cond = vglCalloc(1, sizeof(pthread_cond_t));

	if (attr != NULL)
		return pthread_cond_init(*cond, *attr);
	else
		return pthread_cond_init(*cond, NULL);
}

int pthread_cond_destroy_soloader(pthread_cond_t **cond)
{
	int ret = pthread_cond_destroy(*cond);
	vglFree(*cond);
	return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t **cond)
{
	init_static_cond(cond);
	return pthread_cond_signal(*cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t **cond,
					pthread_mutex_t **mutex,
					struct timespec *abstime)
{
	init_static_cond(cond);
	init_static_mutex(mutex);
	return pthread_cond_timedwait(*cond, *mutex, abstime);
}

int pthread_create_soloader(pthread_t **thread,
				const pthread_attr_t **attr,
				void *(*start)(void *),
				void *param)
{
	*thread = vglCalloc(1, sizeof(pthread_t));

	if (attr != NULL) {
		pthread_attr_setstacksize(*attr, 512 * 1024);
		return pthread_create(*thread, *attr, start, param);
	} else {
		pthread_attr_t attrr;
		pthread_attr_init(&attrr);
		pthread_attr_setstacksize(&attrr, 512 * 1024);
		return pthread_create(*thread, &attrr, start, param);
	}

}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t **attr)
{
	*attr = vglCalloc(1, sizeof(pthread_mutexattr_t));

	return pthread_mutexattr_init(*attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t **attr, int type)
{
	return pthread_mutexattr_settype(*attr, type);
}

int pthread_mutexattr_setpshared_soloader(pthread_mutexattr_t **attr, int pshared)
{
	return pthread_mutexattr_setpshared(*attr, pshared);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t **attr)
{
	int ret = pthread_mutexattr_destroy(*attr);
	vglFree(*attr);
	return ret;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t **mutex)
{
	int ret = pthread_mutex_destroy(*mutex);
	vglFree(*mutex);
	return ret;
}

int pthread_mutex_init_soloader(pthread_mutex_t **mutex,
				const pthread_mutexattr_t **attr)
{
	*mutex = vglCalloc(1, sizeof(pthread_mutex_t));

	if (attr != NULL)
		return pthread_mutex_init(*mutex, *attr);
	else
		return pthread_mutex_init(*mutex, NULL);
}

int pthread_mutex_lock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_lock(*mutex);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_trylock(*mutex);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t **mutex)
{
	return pthread_mutex_unlock(*mutex);
}

int pthread_join_soloader(const pthread_t *thread, void **value_ptr)
{
	return pthread_join(*thread, value_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t **cond, pthread_mutex_t **mutex)
{
	return pthread_cond_wait(*cond, *mutex);
}

int pthread_cond_broadcast_soloader(pthread_cond_t **cond)
{
	return pthread_cond_broadcast(*cond);
}

int pthread_attr_init_soloader(pthread_attr_t **attr)
{
	*attr = vglCalloc(1, sizeof(pthread_attr_t));

	return pthread_attr_init(*attr);
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t **attr, int state)
{
	return pthread_attr_setdetachstate(*attr, !state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t **attr, size_t stacksize)
{
	return pthread_attr_setstacksize(*attr, stacksize);
}

int pthread_attr_getstacksize_soloader(pthread_attr_t **attr, size_t *stacksize)
{
	return pthread_attr_getstacksize(*attr, stacksize);
}

int pthread_attr_setschedparam_soloader(pthread_attr_t **attr,
					const struct sched_param *param)
{
	return pthread_attr_setschedparam(*attr, param);
}

int pthread_attr_setstack_soloader(pthread_attr_t **attr,
				   void *stackaddr,
				   size_t stacksize)
{
	return pthread_attr_setstack(*attr, stackaddr, stacksize);
}

int pthread_setschedparam_soloader(const pthread_t *thread, int policy,
				   const struct sched_param *param)
{
	return pthread_setschedparam(*thread, policy, param);
}

int pthread_getschedparam_soloader(const pthread_t *thread, int *policy,
				   struct sched_param *param)
{
	return pthread_getschedparam(*thread, policy, param);
}

int pthread_detach_soloader(const pthread_t *thread)
{
	return pthread_detach(*thread);
}

int pthread_getattr_np_soloader(pthread_t* thread, pthread_attr_t *attr) {
	fprintf(stderr, "[WARNING!] Not implemented: pthread_getattr_np\n");
	return 0;
}

int pthread_equal_soloader(const pthread_t *t1, const pthread_t *t2)
{
	if (t1 == t2)
		return 1;
	if (!t1 || !t2)
		return 0;
	return pthread_equal(*t1, *t2);
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(const pthread_t *thread, const char* thread_name) {
	if (thread == 0 || thread_name == NULL) {
		return EINVAL;
	}
	size_t thread_name_len = strlen(thread_name);
	if (thread_name_len >= MAX_TASK_COMM_LEN) {
		return ERANGE;
	}

	// TODO: Implement the actual name setting if possible
	fprintf(stderr, "PTHR: pthread_setname_np with name %s\n", thread_name);

	return 0;
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}


int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_uldivmod;
extern void *__aeabi_ldiv0;
extern void *__aeabi_ul2f;
extern void *__aeabi_l2f;
extern void *__aeabi_d2lz;
extern void *__aeabi_l2d;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b) {
	dlog("throwing %s\n", *str);
}

typedef struct {
	uint32_t magic;
	uint8_t *buf;
	size_t sz;
	size_t offs;
} fake_fd;

#define FDS_ARRAY_SIZE (1024)
uint32_t grab_idx = 1;
uint32_t release_idx = 0;
fake_fd fake_fds[FDS_ARRAY_SIZE];
fake_fd *fake_fds_pool[FDS_ARRAY_SIZE];

//#define DEBUG_FOPEN

FILE *cache_lookup(uint64_t hash);
void cache_insert(uint64_t hash, const char *fname);

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f = NULL;
#ifdef HAVE_FILEPATH_CACHE
	if (mode[0] == 'r') {
		uint64_t hash = XXH3_64bits(fname, strlen(fname));
		f = cache_lookup(hash);
		if (f) {
			goto FOUND_FILE;
		}
	}
#endif
	char real_fname[256];
	dlog("fopen(%s,%s)\n", fname, mode);
	if (!strncmp(fname, "../Textures/Package", 19)) {
		sprintf(real_fname, "ux0:data/ut99/Textures/Package0.utx");
		dlog("fopen(%s,%s) ON PACKAGE!!!!\n", real_fname, mode);
		f = sceLibcBridge_fopen(real_fname, mode);
	} else if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "ux0:data/ut99/System/%s", fname);
		dlog("fopen(%s,%s) patched\n", real_fname, mode);
		f = sceLibcBridge_fopen(real_fname, mode);
	} else {
		f = sceLibcBridge_fopen(fname, mode);
	}
	if (mode[0] == 'r' && f) {
FOUND_FILE: {
		fake_fd *fd = fake_fds_pool[grab_idx];
		grab_idx = (grab_idx + 1) % FDS_ARRAY_SIZE;
		fd->magic = 0xBADDBADD;
#if 1
		fd->buf = (uint8_t *)f;
		fd->offs = 0xFFFFFFFF;
#else
		sceLibcBridge_fseek(f, 0, SEEK_END);
		fd->sz = sceLibcBridge_ftell(f);
		fd->buf = vglMalloc(fd->sz);
		fd->offs = 0;
		sceLibcBridge_fseek(f, 0, SEEK_SET);
		sceLibcBridge_fread(fd->buf, 1, fd->sz, f);
		sceLibcBridge_fclose(f);
#endif
		return (FILE *)fd;
}
	}
	return f;
}

int fseek_hook(FILE *fp, long offs, int whence) {
	fake_fd *fd = (fake_fd *)fp;
	if (fd->magic == 0xBADDBADD) {
		switch (whence) {
		case SEEK_SET:
			fd->offs = offs;
			return 0;
		case SEEK_END:
#if 1
			if (fd->offs == 0xFFFFFFFF) {
				fp = (FILE *)fd->buf;
				sceLibcBridge_fseek(fp, 0, SEEK_END);
				fd->sz = sceLibcBridge_ftell(fp);
				fd->buf = vglMalloc(fd->sz);
#ifdef DEBUG_FOPEN
				if (!fd->buf) {
					sceClibPrintf("Failed to alloc %d bytes for fread.\n", fd->sz);
				}
#endif
				fd->offs = 0;
				sceLibcBridge_fseek(fp, 0, SEEK_SET);
				sceLibcBridge_fread(fd->buf, 1, fd->sz, fp);
				sceLibcBridge_fclose(fp);
			}
#endif
			fd->offs = fd->sz + offs;
			return 0;
		default:
			fd->offs += offs;
			return 0;
		}
	}
	return sceLibcBridge_fseek(fp, offs, whence);
}

size_t fread_hook(void *buffer, size_t size, size_t count, fake_fd *fd) {
	size_t ret = fd->offs + (size * count) > fd->sz ? (fd->sz - fd->offs) : (size * count);
	sceClibMemcpy(buffer, &fd->buf[fd->offs], ret);
	fd->offs += ret;
	return ret / size;
}

long ftell_hook(FILE *fp) {
	fake_fd *fd = (fake_fd *)fp;
	if (fd->magic == 0xBADDBADD) {
		return fd->offs;
	}
	return sceLibcBridge_ftell(fp);
}

int fclose_hook(FILE *fp) {
	fake_fd *fd = (fake_fd *)fp;
	if (fd->magic == 0xBADDBADD) {
#if 1
		if (fd->offs == 0xFFFFFFFF) {
			sceLibcBridge_fclose((FILE*)fd->buf);
		} else {
			vglFree(fd->buf);
		}
#else
		vglFree(fd->buf);
#endif
		fake_fds_pool[release_idx] = fd;
		release_idx = (release_idx + 1) % FDS_ARRAY_SIZE;
		return 0;
	}
	return sceLibcBridge_fclose(fp);
	
}

extern void *__aeabi_atexit;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

typedef struct __attribute__((__packed__)) stat64_bionic {
	unsigned long long st_dev;
	unsigned char __pad0[4];
	unsigned long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	unsigned long st_uid;
	unsigned long st_gid;
	unsigned long long st_rdev;
	unsigned char __pad3[4];
	unsigned long st_size;
	unsigned long st_blksize;
	unsigned long st_blocks;
	unsigned long st_atime;
	unsigned long st_atime_nsec;
	unsigned long st_mtime;
	unsigned long st_mtime_nsec;
	unsigned long st_ctime;
	unsigned long st_ctime_nsec;
	unsigned long long __pad4;
} stat64_bionic;

int lstat_hook(const char *pathname, stat64_bionic *statbuf) {
	dlog("lstat(%s)\n", pathname);
	int res;
	struct stat st;
	if (strncmp(pathname, "ux0:", 4)) {
		char fname[256];
		sprintf(fname, "ux0:data/ut99/System/%s", pathname);
		dlog("lstat(%s) fixed\n", fname);
		res = stat(fname, &st);
	} else {
		res = stat(pathname, &st);
	}
	if (res == 0) {
		if (!statbuf) {
			statbuf = vglMalloc(sizeof(stat64_bionic));
		}
		statbuf->st_dev = st.st_dev;
		statbuf->st_ino = st.st_ino;
		statbuf->st_mode = st.st_mode;
		statbuf->st_nlink = st.st_nlink;
		statbuf->st_uid = st.st_uid;
		statbuf->st_gid = st.st_gid;
		statbuf->st_rdev = st.st_rdev;
		statbuf->st_size = st.st_size;
		statbuf->st_blksize = st.st_blksize;
		statbuf->st_blocks = st.st_blocks;
		statbuf->st_atime = st.st_atime;
		statbuf->st_atime_nsec = 0;
		statbuf->st_mtime = st.st_mtime;
		statbuf->st_mtime_nsec = 0;
		statbuf->st_ctime = st.st_ctime;
		statbuf->st_ctime_nsec = 0;
	}
	return res;
}

int stat_hook(const char *pathname, stat64_bionic *statbuf) {
	dlog("stat(%s)\n", pathname);
	int res;
	struct stat st;
	if (strncmp(pathname, "ux0:", 4)) {
		char fname[256];
		sprintf(fname, "ux0:data/ut99/System/%s", pathname);
		dlog("stat(%s) fixed\n", fname);
		res = stat(fname, &st);
	} else {
		res = stat(pathname, &st);
	}
	if (res == 0) {
		if (!statbuf) {
			statbuf = vglMalloc(sizeof(stat64_bionic));
		}
		statbuf->st_dev = st.st_dev;
		statbuf->st_ino = st.st_ino;
		statbuf->st_mode = st.st_mode;
		statbuf->st_nlink = st.st_nlink;
		statbuf->st_uid = st.st_uid;
		statbuf->st_gid = st.st_gid;
		statbuf->st_rdev = st.st_rdev;
		statbuf->st_size = st.st_size;
		statbuf->st_blksize = st.st_blksize;
		statbuf->st_blocks = st.st_blocks;
		statbuf->st_atime = st.st_atime;
		statbuf->st_atime_nsec = 0;
		statbuf->st_mtime = st.st_mtime;
		statbuf->st_mtime_nsec = 0;
		statbuf->st_ctime = st.st_ctime;
		statbuf->st_ctime_nsec = 0;
	}
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void *sceClibMemclr(void *dst, SceSize len) {
	if (!dst) {
		printf("memclr on NULL\n");
		return NULL;
	}
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

char *SDL_AndroidGetExternalStoragePath() {
	return "ux0:data/ut99";
}

char *SDL_AndroidGetInternalStoragePath() {
	return "ux0:data/ut99";
}

char *SDL_GetPrefPath_hook(const char *org, const char *app) {
	char *r = SDL_GetPrefPath(org, app);
	sceClibPrintf("Pref Path: %s\n", r);
	r[strlen(r) - 1] = 0;
	return r;
}

int g_SDL_BufferGeometry_w;
int g_SDL_BufferGeometry_h;

void abort_hook() {
	dlog("abort called from %p\n", __builtin_return_address(0));
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99() {
	return 99;
}

static so_default_dynlib gl_hook[] = {
	{"glPixelStorei", (uintptr_t)&ret0},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

void *SDL_GL_GetProcAddress_fake(const char *symbol) {
	dlog("looking for symbol %s\n", symbol);
	for (size_t i = 0; i < gl_numhook; ++i) {
		if (!strcmp(symbol, gl_hook[i].symbol)) {
			return (void *)gl_hook[i].func;
		}
	}
	void *r = vglGetProcAddress(symbol);
	if (!r) {
		dlog("Cannot find symbol %s\n", symbol);
	}
	return r;
}

uint32_t opendir_fake(const char *dirname) {
#ifdef HAVE_FILEPATH_CACHE
	if (dirname[3] == 'C') // Cache
		return 0;
	uint32_t ret;
	if (dirname[1] == '/') {
		ret = SYSTEM_DIR;
	} else {
		uint32_t i = dirname[0] == 'u' ? 24 : 3;
		switch (dirname[i]) {
		case 'T':
			ret = TEXTURES_DIR;
			break;
		case 'M':
			ret = dirname[i + 1] == 'a' ? MAPS_DIR : MUSIC_DIR;
			break;
		default:
			ret = dirname[i + 1] == 'y' ? SYSTEM_DIR : SOUNDS_DIR;
			break;
		}
	}
	dlog("opendir(%s) -> %d\n", dirname, ret);
	cached_lists[ret] = 0;
	return ret + 1;
#else
	dlog("opendir(%s)\n", dirname);
	SceUID uid;
	if (strncmp(dirname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/ut99/System/%s", dirname);
		uid = sceIoDopen(real_fname);
	} else {
		uid = sceIoDopen(dirname);
	}
	
	if (uid < 0) {
		errno = uid & 0xFF;
		return 0;
	}

	android_DIR *dirp = &_dirp;
	dirp->uid = uid;

	errno = 0;
	return (uint32_t)dirp;
#endif
}

struct android_dirent *readdir_fake(uint32_t dirp) {
#ifdef HAVE_FILEPATH_CACHE
	dirp--;
	if (cached_lists[dirp] < cached_entries[dirp]) {
		return &cached_dirs[dirp][cached_lists[dirp]++];
	}
	return NULL;
#else
	android_DIR *__dirp = (android_DIR *)dirp;
	if (!__dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(__dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & 0xFF;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	__dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(__dirp->dir.d_name, sce_dir.d_name);
	return &__dirp->dir;
#endif
}

#ifndef HAVE_FILEPATH_CACHE
int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	if (res < 0) {
		errno = res & 0xFF;
		return -1;
	}

	errno = 0;
	return 0;
}
#endif

SDL_Surface *IMG_Load_hook(const char *file) {
	char real_fname[256];
	dlog("IMG_Load %s\n", file);
	if (strncmp(file, "ux0:", 4)) {
		sprintf(real_fname, "ux0:data/ut99/System/%s", file);
		return IMG_Load(real_fname);
	}
	return IMG_Load(file);
}

SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode) {
	SDL_RWops *f;
	char real_fname[256];
	dlog("SDL_RWFromFile(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "ux0:data/ut99/System/%s", fname);
		printf("SDL_RWFromFile patched to %s\n", real_fname);
		f = SDL_RWFromFile(real_fname, mode);
	} else {
		f = SDL_RWFromFile(fname, mode);
	}
	return f;
}

Mix_Music *Mix_LoadMUS_hook(const char *fname) {
	Mix_Music *f;
	char real_fname[256];
	dlog("Mix_LoadMUS(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "ux0:data/ut99/System/%s", fname);
		f = Mix_LoadMUS(real_fname);
	} else {
		f = Mix_LoadMUS(fname);
	}
	return f;
}

int Mix_OpenAudio_hook(int frequency, Uint16 format, int channels, int chunksize) {
	return Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024);
}

extern void SDL_ResetKeyboard(void);

size_t __strlen_chk(const char *s, size_t s_len) {
	return strlen(s);
}

SDL_Window *sdl_wnd;
SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y, int w, int h, Uint32 flags) {
	sdl_wnd = SDL_CreateWindow("UT99", 0, 0, SCREEN_W, SCREEN_H, flags);
	return sdl_wnd;
}

uint64_t lseek64(int fd, uint64_t offset, int whence) {
	return lseek(fd, offset, whence);
}

char *SDL_GetBasePath_hook() {
	void *ret = vglMalloc(256);
	sprintf(ret, "%s/", data_path);
	dlog("SDL_GetBasePath\n");
	return ret;
}

void SDL_GetVersion_fake(SDL_version *ver){
	ver->major = 2;
	ver->minor = 0;
	ver->patch = 10;
}

const char *SDL_JoystickName_fake(SDL_Joystick *joystick) {
	return "Totally PS4 Controller ( ͡° ͜ʖ ͡°)";
}

int SDL_OpenAudio_fake(SDL_AudioSpec * desired, SDL_AudioSpec * obtained) {
	desired->freq = 44100;
	return SDL_OpenAudio(desired, obtained);
}

void __assert2(const char *file, int line, const char *func, const char *expr) {
	dlog("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
	sceKernelExitProcess(0);
}

void *dlsym_hook( void *handle, const char *symbol) {
	//dlog("dlsym %s\n", symbol);
	return vglGetProcAddress(symbol);
}

int strerror_r_hook(int errnum, char *buf, size_t buflen) {
	strerror_r(errnum, buf, buflen);
	dlog("Error %d: %s\n",errnum, buf);
	return 0;
}

extern void *__aeabi_ul2d;
extern void *__aeabi_d2ulz;

uint32_t fake_stdout;

int access_hook(const char *pathname, int mode) {
	dlog("access(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/ut99/System/%s", pathname);
		return access(real_fname, mode);
	}
	
	return access(pathname, mode);
}

int mkdir_hook(const char *pathname, int mode) {
	dlog("mkdir(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/ut99/System/%s", pathname);
		return mkdir(real_fname, mode);
	}
	
	return mkdir(pathname, mode);
}

static uint32_t last_fbo_w, last_fbo_h;
void glFramebufferTexture2D_hook(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
	if (attachment == GL_COLOR_ATTACHMENT0) {
		glFramebufferTexture2D(target, attachment, textarget, texture, level);
		glBindTexture(GL_TEXTURE_2D, texture);
		SceGxmTexture *t = vglGetGxmTexture(GL_TEXTURE_2D);
		last_fbo_w = sceGxmTextureGetWidth(t);
		last_fbo_h = sceGxmTextureGetHeight(t);
	} else if (attachment == GL_STENCIL_ATTACHMENT) {
		// Game always attached stencil attachment right after depth one, so we use a combined renderbuffer for it
		GLuint rbo;
		glGenRenderbuffers(1, &rbo);
		glNamedRenderbufferStorage(rbo, GL_DEPTH24_STENCIL8, last_fbo_w, last_fbo_h);
		glFramebufferRenderbuffer(target, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
	}
}

int rmdir_hook(const char *pathname) {
	dlog("rmdir(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/ut99/System/%s", pathname);
		return rmdir(real_fname);
	}
	
	return rmdir(pathname);
}

int unlink_hook(const char *pathname) {
	dlog("unlink(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/ut99/System/%s", pathname);
		return sceIoRemove(real_fname);
	}
	
	return sceIoRemove(pathname);
}

int remove_hook(const char *pathname) {
	dlog("unlink(%s)\n", pathname);
	if (strncmp(pathname, "ux0:", 4)) {
		char real_fname[256];
		sprintf(real_fname, "ux0:data/ut99/System/%s", pathname);
		return sceIoRemove(real_fname);
	}
	
	return sceIoRemove(pathname);
}

int rename_hook(const char *old_filename, const char *new_filename) {
	dlog("rename %s -> %s\n", old_filename, new_filename);
	char real_old[256], real_new[256];
	if (strncmp(old_filename, "ux0:", 4)) {
		sprintf(real_old, "ux0:data/ut99/System/%s", old_filename);
	} else {
		strcpy(real_old, old_filename);
	}
	if (strncmp(new_filename, "ux0:", 4)) {
		sprintf(real_new, "ux0:data/ut99/System/%s", new_filename);
	} else {
		strcpy(real_new, new_filename);
	}
	return sceIoRename(real_old, real_new);
}

int SDL_Init_hook(uint32_t flags) {
	return SDL_Init(flags | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
}

int nanosleep_hook(const struct timespec *req, struct timespec *rem) {
	const uint32_t usec = req->tv_sec * 1000 * 1000 + req->tv_nsec / 1000;
	return sceKernelDelayThreadCB(usec);
}

static so_default_dynlib default_dynlib[] = {
	{ "xmp_end_player", (uintptr_t)&xmp_end_player},
	{ "xmp_release_module", (uintptr_t)&xmp_release_module},
	{ "xmp_free_context", (uintptr_t)&xmp_free_context},
	{ "xmp_create_context", (uintptr_t)&xmp_create_context},
	{ "xmp_load_module_from_memory", (uintptr_t)&xmp_load_module_from_memory},
	{ "xmp_start_player", (uintptr_t)&xmp_start_player},
	{ "xmp_set_position", (uintptr_t)&xmp_set_position},
	{ "xmp_play_buffer", (uintptr_t)&xmp_play_buffer},
	{ "rename", (uintptr_t)&rename_hook},
	{ "glGetError", (uintptr_t)&ret0},
	{ "strtoll_l", (uintptr_t)&strtoll_l},
	{ "strtoull_l", (uintptr_t)&strtoull_l},
	{ "strtold_l", (uintptr_t)&strtold_l},
	{ "wcstoul", (uintptr_t)&wcstoul},
	{ "wcstoll", (uintptr_t)&wcstoll},
	{ "wcstoull", (uintptr_t)&wcstoull},
	{ "wcstof", (uintptr_t)&wcstof},
	{ "wcstod", (uintptr_t)&wcstod},
	{ "wcsnrtombs", (uintptr_t)&wcsnrtombs},
	{ "mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
	{ "mbtowc", (uintptr_t)&mbtowc},
	{ "mbrlen", (uintptr_t)&mbrlen},
	{ "select", (uintptr_t)&select},
	{ "isblank", (uintptr_t)&isblank},
	{ "SDL_QuitSubSystem", (uintptr_t)&SDL_QuitSubSystem},
	{ "SDL_QueueAudio", (uintptr_t)&SDL_QueueAudio},
	{ "SDL_ClearQueuedAudio", (uintptr_t)&SDL_ClearQueuedAudio},
	{ "SDL_GetCPUCount", (uintptr_t)&SDL_GetCPUCount},
	{ "SDL_GetQueuedAudioSize", (uintptr_t)&SDL_GetQueuedAudioSize},
	{ "SDL_VideoQuit", (uintptr_t)&SDL_VideoQuit},
	{ "SDL_GameControllerName", (uintptr_t)&SDL_GameControllerName},
	{ "SDL_ClearError", (uintptr_t)&SDL_ClearError},
	{ "rmdir", (uintptr_t)&rmdir_hook},
	{ "rmdir", (uintptr_t)&rmdir_hook},
	{ "_setjmp", (uintptr_t)&setjmp},
	{ "_longjmp", (uintptr_t)&longjmp},
	{ "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D_hook},
	{ "stdout", (uintptr_t)&fake_stdout },
	{ "stdin", (uintptr_t)&fake_stdout },
	{ "stderr", (uintptr_t)&fake_stdout },
	{ "newlocale", (uintptr_t)&newlocale },
	{ "uselocale", (uintptr_t)&uselocale },
	{ "ov_read", (uintptr_t)&ov_read },
	{ "ov_raw_seek", (uintptr_t)&ov_raw_seek },
	{ "ov_open_callbacks", (uintptr_t)&ov_open_callbacks },
	{ "ov_pcm_total", (uintptr_t)&ov_pcm_total },
	{ "ov_clear", (uintptr_t)&ov_clear },
	{ "SDL_GetRelativeMouseState", (uintptr_t)&SDL_GetRelativeMouseState },
	{ "SDL_OpenAudioDevice", (uintptr_t)&SDL_OpenAudioDevice },
	{ "SDL_PauseAudioDevice", (uintptr_t)&SDL_PauseAudioDevice },
	{ "SDL_CloseAudioDevice", (uintptr_t)&SDL_CloseAudioDevice },
	{ "SDL_LockAudioDevice", (uintptr_t)&SDL_LockAudioDevice },
	{ "SDL_memset", (uintptr_t)&SDL_memset },
	{ "SDL_UnlockAudioDevice", (uintptr_t)&SDL_UnlockAudioDevice },
	{ "exp2f", (uintptr_t)&exp2f },
	{ "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
	{ "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
	{ "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
	{ "__pthread_cleanup_push", (uintptr_t)&ret0 },
	{ "__pthread_cleanup_pop", (uintptr_t)&ret0 },
	//{ "SDL_SendMouseMotion", (uintptr_t)&SDL_SendMouseMotion },
	//{ "SDL_SendMouseButton", (uintptr_t)&SDL_SendMouseButton },
	{ "SDL_SetTextInputRect", (uintptr_t)&SDL_SetTextInputRect },
	{ "SDL_GameControllerAddMapping", (uintptr_t)&SDL_GameControllerAddMapping },
	{ "sincos", (uintptr_t)&sincos },
	{ "SDL_GetWindowDisplayMode", (uintptr_t)&SDL_GetWindowDisplayMode },
	{ "SDL_SetWindowDisplayMode", (uintptr_t)&SDL_SetWindowDisplayMode },
	{ "SDL_CreateSoftwareRenderer", (uintptr_t)&SDL_CreateSoftwareRenderer },
	{ "SDL_RenderCopyEx", (uintptr_t)&SDL_RenderCopyEx },
	{ "SDL_SetWindowGammaRamp", (uintptr_t)&SDL_SetWindowGammaRamp },
	{ "SDL_GetWindowGammaRamp", (uintptr_t)&SDL_GetWindowGammaRamp },
	{ "SDL_SetRelativeMouseMode", (uintptr_t)&SDL_SetRelativeMouseMode },
	{ "SDL_WasInit", (uintptr_t)&SDL_WasInit },
	{ "__assert2", (uintptr_t)&__assert2 },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri},
	{ "glReadPixels", (uintptr_t)&glReadPixels},
	{ "glShaderSource", (uintptr_t)&glShaderSource},
	{ "SDL_GetPlatform", (uintptr_t)&SDL_GetPlatform},
	{ "sincosf", (uintptr_t)&sincosf },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
#ifdef HAVE_FILEPATH_CACHE
	{ "closedir", (uintptr_t)&ret0 },
#else
	{ "closedir", (uintptr_t)&closedir_fake },	
#endif
	{ "g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w },
	{ "g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h },
	{ "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
	{ "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
	{ "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
	{ "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&memmove },
	{ "__aeabi_memmove8", (uintptr_t)&memmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&memmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
	{ "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
	{ "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
	{ "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno },
	{ "__strlen_chk", (uintptr_t)&__strlen_chk },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
	{ "dl_unwind_find_exidx", (uintptr_t)&ret0 },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access_hook },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	// { "bind", (uintptr_t)&bind },
	{ "bsd_signal", (uintptr_t)&ret0 },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&vglCalloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&ret0 },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlopen", (uintptr_t)&ret0 },
	{ "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose_hook },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "feof", (uintptr_t)&sceLibcBridge_feof },
	{ "ferror", (uintptr_t)&sceLibcBridge_ferror },
	{ "fflush", (uintptr_t)&ret0 },
	{ "floor", (uintptr_t)&floor },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
	{ "fputc", (uintptr_t)&sceLibcBridge_fputc },
	{ "fread", (uintptr_t)&fread_hook },
	{ "free", (uintptr_t)&vglFree },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	{ "fseek", (uintptr_t)&fseek_hook },
	{ "ftell", (uintptr_t)&ftell_hook },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "gettid", (uintptr_t)&ret0 },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&getenv },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "isascii", (uintptr_t)&isascii },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "lseek64", (uintptr_t)&lseek64 },
	{ "malloc", (uintptr_t)&vglMalloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&vglMemalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&memmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir_hook },
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	// { "poll", (uintptr_t)&poll },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&sceClibPrintf },
	{ "pthread_join", (uintptr_t)&pthread_join_soloader },
	{ "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
	{ "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_soloader },
	{ "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
	{ "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
	{ "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_soloader },
	{ "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_soloader },
	{ "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
	{ "pthread_attr_getstacksize", (uintptr_t) &pthread_attr_getstacksize_soloader },
	{ "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
	{ "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
	{ "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
	{ "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
	{ "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
	{ "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
	{ "pthread_create", (uintptr_t) &pthread_create_soloader },
	{ "pthread_detach", (uintptr_t) &pthread_detach_soloader },
	{ "pthread_equal", (uintptr_t) &pthread_equal_soloader },
	{ "pthread_exit", (uintptr_t) &pthread_exit },
	{ "pthread_getattr_np", (uintptr_t) &pthread_getattr_np_soloader },
	{ "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
	{ "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
	{ "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
	{ "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader},
	{ "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
	{ "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader},
	{ "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader},
	{ "pthread_mutexattr_setpshared", (uintptr_t) &pthread_mutexattr_setpshared_soloader},
	{ "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader},
	{ "pthread_once", (uintptr_t)&pthread_once },
	{ "pthread_self", (uintptr_t) &pthread_self },
	{ "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "read", (uintptr_t)&read },
	{ "realloc", (uintptr_t)&vglRealloc },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&setenv },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	//{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "chmod", (uintptr_t)&ret0 },
	{ "stat", (uintptr_t)&stat_hook },
	{ "lstat", (uintptr_t)&lstat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strerror", (uintptr_t)&strerror },
	{ "strerror_r", (uintptr_t)&strerror_r_hook },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strtok", (uintptr_t)&strtok },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "gmtime", (uintptr_t)&gmtime },
	{ "mktime", (uintptr_t)&mktime },
	//{ "utime", (uintptr_t)&utime },
	{ "difftime", (uintptr_t)&difftime },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&ret0 },
	{ "vasprintf", (uintptr_t)&vasprintf },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsscanf", (uintptr_t)&vsscanf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	{ "sigaction", (uintptr_t)&ret0 },
	{ "zlibVersion", (uintptr_t)&zlibVersion },
	// { "writev", (uintptr_t)&writev },
	{ "unlink", (uintptr_t)&unlink_hook },
	{ "SDL_SetWindowGrab", (uintptr_t)&SDL_SetWindowGrab },
	{ "SDL_SetWindowIcon", (uintptr_t)&ret0 },
	{ "SDL_AndroidGetActivityClass", (uintptr_t)&ret0 },
	{ "SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive },
	{ "SDL_GameControllerEventState", (uintptr_t)&SDL_GameControllerEventState },
	{ "SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow },
	{ "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
	{ "SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath },
	{ "SDL_Android_Init", (uintptr_t)&ret1 },
	{ "SDL_ShowWindow", (uintptr_t)&SDL_ShowWindow },
	{ "SDL_AddTimer", (uintptr_t)&SDL_AddTimer },
	{ "SDL_CondSignal", (uintptr_t)&SDL_CondSignal },
	{ "SDL_CondWait", (uintptr_t)&SDL_CondWait },
	{ "SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat },
	{ "SDL_CreateCond", (uintptr_t)&SDL_CreateCond },
	{ "SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex },
	{ "SDL_CreateRenderer", (uintptr_t)&SDL_CreateRenderer },
	{ "SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface },
	{ "SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture },
	{ "SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface },
	{ "SDL_CreateThread", (uintptr_t)&SDL_CreateThread },
	{ "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow_hook },
	{ "SDL_Delay", (uintptr_t)&SDL_Delay },
	{ "SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex },
	{ "SDL_DestroyRenderer", (uintptr_t)&SDL_DestroyRenderer },
	{ "SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture },
	{ "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
	{ "SDL_FillRect", (uintptr_t)&SDL_FillRect },
	{ "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
	{ "SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode },
	{ "SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode },
	{ "SDL_GetError", (uintptr_t)&SDL_GetError },
	{ "SDL_GetModState", (uintptr_t)&SDL_GetModState },
	{ "SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState },
	{ "SDL_GetRGBA", (uintptr_t)&SDL_GetRGBA },
	{ "SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW },
	{ "SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes },
	{ "SDL_GetRendererInfo", (uintptr_t)&SDL_GetRendererInfo },
	{ "SDL_GetTextureBlendMode", (uintptr_t)&SDL_GetTextureBlendMode },
	{ "SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath },
	{ "SDL_GetTextureColorMod", (uintptr_t)&SDL_GetTextureColorMod },
	{ "SDL_GetTicks", (uintptr_t)&SDL_GetTicks },
	{ "SDL_GetVersion", (uintptr_t)&SDL_GetVersion_fake },
	{ "SDL_GL_BindTexture", (uintptr_t)&SDL_GL_BindTexture },
	{ "SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext },
	{ "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
	{ "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
	{ "SDL_Init", (uintptr_t)&SDL_Init_hook },
	{ "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
	{ "SDL_IntersectRect", (uintptr_t)&SDL_IntersectRect },
	{ "SDL_LockMutex", (uintptr_t)&SDL_LockMutex },
	{ "SDL_LockSurface", (uintptr_t)&SDL_LockSurface },
	{ "SDL_Log", (uintptr_t)&ret0 },
	{ "SDL_LogError", (uintptr_t)&ret0 },
	{ "SDL_LogSetPriority", (uintptr_t)&ret0 },
	{ "SDL_MapRGB", (uintptr_t)&SDL_MapRGB },
	{ "SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID },
	{ "SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis },
	{ "SDL_MinimizeWindow", (uintptr_t)&SDL_MinimizeWindow },
	{ "SDL_PeepEvents", (uintptr_t)&SDL_PeepEvents },
	{ "SDL_PumpEvents", (uintptr_t)&SDL_PumpEvents },
	{ "SDL_PushEvent", (uintptr_t)&SDL_PushEvent },
	{ "SDL_PollEvent", (uintptr_t)&SDL_PollEvent },
	{ "SDL_QueryTexture", (uintptr_t)&SDL_QueryTexture },
	{ "SDL_Quit", (uintptr_t)&SDL_Quit },
	{ "SDL_RemoveTimer", (uintptr_t)&SDL_RemoveTimer },
	{ "SDL_RenderClear", (uintptr_t)&SDL_RenderClear },
	{ "SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy },
	{ "SDL_RenderFillRect", (uintptr_t)&SDL_RenderFillRect },
	{ "SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent },
	{ "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_hook },
	{ "SDL_RWread", (uintptr_t)&SDL_RWread },
	{ "SDL_RWwrite", (uintptr_t)&SDL_RWwrite },
	{ "SDL_RWclose", (uintptr_t)&SDL_RWclose },
	{ "SDL_RWsize", (uintptr_t)&SDL_RWsize },
	{ "SDL_RWFromMem", (uintptr_t)&SDL_RWFromMem },
	{ "SDL_SetColorKey", (uintptr_t)&SDL_SetColorKey },
	{ "SDL_SetEventFilter", (uintptr_t)&SDL_SetEventFilter },
	{ "SDL_SetHint", (uintptr_t)&SDL_SetHint },
	{ "SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_SetRenderDrawBlendMode", (uintptr_t)&SDL_SetRenderDrawBlendMode },
	{ "SDL_SetRenderDrawColor", (uintptr_t)&SDL_SetRenderDrawColor },
	{ "SDL_SetRenderTarget", (uintptr_t)&SDL_SetRenderTarget },
	{ "SDL_SetTextureBlendMode", (uintptr_t)&SDL_SetTextureBlendMode },
	{ "SDL_SetTextureColorMod", (uintptr_t)&SDL_SetTextureColorMod },
	{ "SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor },
	{ "SDL_ShowSimpleMessageBox", (uintptr_t)&ret0 },
	{ "SDL_StartTextInput", (uintptr_t)&ret0 },
	{ "SDL_StopTextInput", (uintptr_t)&ret0 },
	{ "SDL_strdup", (uintptr_t)&SDL_strdup },
	{ "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
	{ "SDL_UnlockSurface", (uintptr_t)&SDL_UnlockSurface },
	{ "SDL_UpdateTexture", (uintptr_t)&SDL_UpdateTexture },
	{ "SDL_UpperBlit", (uintptr_t)&SDL_UpperBlit },
	{ "SDL_WaitThread", (uintptr_t)&SDL_WaitThread },
	{ "SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode },
	{ "SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays },
	{ "SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds },
	{ "SDL_UnionRect", (uintptr_t)&SDL_UnionRect },
	{ "SDL_GetKeyboardFocus", (uintptr_t)&SDL_GetKeyboardFocus },
	{ "SDL_GetRelativeMouseMode", (uintptr_t)&SDL_GetRelativeMouseMode },
	{ "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks },
	{ "SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize },
	{ "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen },
	{ "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
	{ "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick },
	{ "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
	{ "SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter },
	{ "SDL_GetMouseFocus", (uintptr_t)&SDL_GetMouseFocus },
	{ "SDL_ShowMessageBox", (uintptr_t)&ret0 },
	{ "SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow },
	{ "SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute },
	{ "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext },
	{ "SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress_fake },
	{ "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
	{ "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode },
	{ "SDL_SetWindowData", (uintptr_t)&SDL_SetWindowData },
	{ "SDL_GetWindowFlags", (uintptr_t)&SDL_GetWindowFlags },
	{ "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
	{ "SDL_GetWindowDisplayIndex", (uintptr_t)&SDL_GetWindowDisplayIndex },
	{ "SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen },
	{ "SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize },
	{ "SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition },
	{ "SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow },
	{ "SDL_GetWindowData", (uintptr_t)&SDL_GetWindowData },
	{ "SDL_GetWindowTitle", (uintptr_t)&SDL_GetWindowTitle },
	{ "SDL_ResetKeyboard", (uintptr_t)&SDL_ResetKeyboard },
	{ "SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle },
	{ "SDL_GetWindowPosition", (uintptr_t)&SDL_GetWindowPosition },
	{ "SDL_GL_SetSwapInterval", (uintptr_t)&ret0 },
	{ "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
	{ "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
	{ "SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex },
	{ "SDL_GetWindowFromID", (uintptr_t)&SDL_GetWindowFromID },
	{ "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow },
	{ "SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady },
	{ "SDL_NumAccelerometers", (uintptr_t)&ret0 },
	{ "SDL_AndroidGetJNIEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv },
	{ "SDL_RWFromConstMem", (uintptr_t)&SDL_RWFromConstMem },
	{ "SDL_ConvertSurface", (uintptr_t)&SDL_ConvertSurface },
	{ "SDL_SetError", (uintptr_t)&SDL_SetError },
	{ "SDL_MapRGBA", (uintptr_t)&SDL_MapRGBA },
	{ "SDL_EventState", (uintptr_t)&SDL_EventState },
	{ "SDL_SetSurfaceBlendMode", (uintptr_t)&SDL_SetSurfaceBlendMode },
	{ "SDL_UpperBlitScaled", (uintptr_t)&SDL_UpperBlitScaled },
	{ "SDL_FreeRW", (uintptr_t)&SDL_FreeRW },
	{ "SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState },
	{ "SDL_JoystickNumAxes", (uintptr_t)&ret4 },
	{ "SDL_JoystickUpdate", (uintptr_t)&SDL_JoystickUpdate },
	{ "SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis },
	{ "SDL_JoystickGetButton", (uintptr_t)&SDL_JoystickGetButton },
	{ "SDL_GetScancodeFromKey", (uintptr_t)&SDL_GetScancodeFromKey },
	{ "SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName },
	{ "SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName },
	{ "SDL_JoystickGetHat", (uintptr_t)&SDL_JoystickGetHat },
	{ "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
	{ "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen },
	{ "SDL_JoystickEventState", (uintptr_t)&SDL_JoystickEventState },
	{ "SDL_LogSetAllPriority", (uintptr_t)&SDL_LogSetAllPriority },
	{ "SDL_LogMessageV", (uintptr_t)&SDL_LogMessageV },
	{ "SDL_RWtell", (uintptr_t)&SDL_RWtell },
	{ "SDL_AndroidGetActivity", (uintptr_t)&ret0 },
	{ "SDL_free", (uintptr_t)&SDL_free },
	{ "SDL_AtomicAdd", (uintptr_t)&SDL_AtomicAdd },
	{ "SDL_AtomicSet", (uintptr_t)&SDL_AtomicSet },
	{ "SDL_CreateSystemCursor", (uintptr_t)&SDL_CreateSystemCursor },
	{ "SDL_OpenAudio", (uintptr_t)&SDL_OpenAudio },
	{ "SDL_CloseAudio", (uintptr_t)&SDL_CloseAudio },
	{ "SDL_PauseAudio", (uintptr_t)&SDL_PauseAudio },
	{ "SDL_CreateCursor", (uintptr_t)&SDL_CreateCursor },
	{ "SDL_SetCursor", (uintptr_t)&SDL_SetCursor },
	{ "SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose },
	{ "SDL_FreeCursor", (uintptr_t)&SDL_FreeCursor },
	{ "SDL_CreateColorCursor", (uintptr_t)&SDL_CreateColorCursor },
	{ "IMG_Init", (uintptr_t)&IMG_Init },
	{ "IMG_Quit", (uintptr_t)&IMG_Quit },
	{ "Mix_PauseMusic", (uintptr_t)&Mix_PauseMusic },
	{ "Mix_ResumeMusic", (uintptr_t)&Mix_ResumeMusic },
	{ "Mix_VolumeMusic", (uintptr_t)&Mix_VolumeMusic },
	{ "Mix_LoadMUS", (uintptr_t)&Mix_LoadMUS_hook },
	{ "Mix_PlayMusic", (uintptr_t)&Mix_PlayMusic },
	{ "Mix_FreeMusic", (uintptr_t)&ret0 }, // FIXME
	{ "nanosleep", (uintptr_t)&nanosleep_hook }, // FIXME
	{ "Mix_RewindMusic", (uintptr_t)&Mix_RewindMusic },
	{ "Mix_SetMusicPosition", (uintptr_t)&Mix_SetMusicPosition },
	{ "Mix_CloseAudio", (uintptr_t)&Mix_CloseAudio },
	{ "Mix_OpenAudio", (uintptr_t)&Mix_OpenAudio_hook },
	{ "Mix_RegisterEffect", (uintptr_t)&Mix_RegisterEffect },
	{ "Mix_Resume", (uintptr_t)&Mix_Resume },
	{ "Mix_AllocateChannels", (uintptr_t)&Mix_AllocateChannels },
	{ "Mix_ChannelFinished", (uintptr_t)&Mix_ChannelFinished },
	{ "Mix_LoadWAV_RW", (uintptr_t)&Mix_LoadWAV_RW },
	{ "Mix_FreeChunk", (uintptr_t)&Mix_FreeChunk },
	{ "Mix_PausedMusic", (uintptr_t)&Mix_PausedMusic },
	{ "Mix_Paused", (uintptr_t)&Mix_Paused },
	{ "Mix_PlayingMusic", (uintptr_t)&Mix_PlayingMusic },
	{ "Mix_Playing", (uintptr_t)&Mix_Playing },
	{ "Mix_Volume", (uintptr_t)&Mix_Volume },
	{ "Mix_SetDistance", (uintptr_t)&Mix_SetDistance },
	{ "Mix_SetPanning", (uintptr_t)&Mix_SetPanning },
	{ "Mix_QuerySpec", (uintptr_t)&Mix_QuerySpec },
	{ "Mix_UnregisterEffect", (uintptr_t)&Mix_UnregisterEffect },
	{ "Mix_HaltMusic", (uintptr_t)&Mix_HaltMusic },
	{ "Mix_HaltChannel", (uintptr_t)&Mix_HaltChannel },
	{ "Mix_LoadMUS_RW", (uintptr_t)&Mix_LoadMUS_RW },
	{ "Mix_PlayChannelTimed", (uintptr_t)&Mix_PlayChannelTimed },
	{ "Mix_Pause", (uintptr_t)&Mix_Pause },
	{ "Mix_Init", (uintptr_t)&Mix_Init },
	/*{ "TTF_Quit", (uintptr_t)&TTF_Quit },
	{ "TTF_Init", (uintptr_t)&TTF_Init },
	{ "TTF_RenderText_Blended", (uintptr_t)&TTF_RenderText_Blended },
	{ "TTF_OpenFontRW", (uintptr_t)&TTF_OpenFontRW },
	{ "TTF_SetFontOutline", (uintptr_t)&TTF_SetFontOutline },
	{ "TTF_CloseFont", (uintptr_t)&TTF_CloseFont },
	{ "TTF_GlyphIsProvided", (uintptr_t)&TTF_GlyphIsProvided },*/
	{ "IMG_Load", (uintptr_t)&IMG_Load_hook },
	{ "IMG_Load_RW", (uintptr_t)&IMG_Load_RW },
	{ "raise", (uintptr_t)&raise },
	{ "swprintf", (uintptr_t)&swprintf },
	{ "wcscpy", (uintptr_t)&wcscpy },
	{ "wcscat", (uintptr_t)&wcscat },
	{ "wcstombs", (uintptr_t)&wcstombs },
	{ "wcsstr", (uintptr_t)&wcsstr },
	{ "compress", (uintptr_t)&compress },
	{ "uncompress", (uintptr_t)&uncompress },
	{ "atof", (uintptr_t)&atof },
	{ "trunc", (uintptr_t)&trunc },
	{ "round", (uintptr_t)&round },
	{ "llrintf", (uintptr_t)&llrintf },
	{ "llrint", (uintptr_t)&llrint },
	{ "SDLNet_FreePacket", (uintptr_t)&SDLNet_FreePacket },
	{ "SDLNet_Quit", (uintptr_t)&SDLNet_Quit },
	{ "SDLNet_GetError", (uintptr_t)&SDLNet_GetError },
	{ "SDLNet_Init", (uintptr_t)&SDLNet_Init },
	{ "SDLNet_AllocPacket", (uintptr_t)&SDLNet_AllocPacket },
	{ "SDLNet_UDP_Recv", (uintptr_t)&SDLNet_UDP_Recv },
	{ "SDLNet_UDP_Send", (uintptr_t)&SDLNet_UDP_Send },
	{ "SDLNet_GetLocalAddresses", (uintptr_t)&SDLNet_GetLocalAddresses },
	{ "SDLNet_UDP_Close", (uintptr_t)&SDLNet_UDP_Close },
	{ "SDLNet_ResolveHost", (uintptr_t)&SDLNet_ResolveHost },
	{ "SDLNet_UDP_Open", (uintptr_t)&SDLNet_UDP_Open },
	{ "remove", (uintptr_t)&remove_hook },
	{ "IMG_SavePNG", (uintptr_t)&IMG_SavePNG },
	{ "SDL_DetachThread", (uintptr_t)&SDL_DetachThread },
	/*{ "TTF_SetFontHinting", (uintptr_t)&TTF_SetFontHinting },
	{ "TTF_FontHeight", (uintptr_t)&TTF_FontHeight },
	{ "TTF_FontAscent", (uintptr_t)&TTF_FontAscent },
	{ "TTF_FontDescent", (uintptr_t)&TTF_FontDescent },
	{ "TTF_SizeUTF8", (uintptr_t)&TTF_SizeUTF8 },
	{ "TTF_SizeText", (uintptr_t)&TTF_SizeText },
	{ "TTF_SetFontStyle", (uintptr_t)&TTF_SetFontStyle },
	{ "TTF_RenderUTF8_Blended", (uintptr_t)&TTF_RenderUTF8_Blended },*/
	{ "SDL_strlen", (uintptr_t)&SDL_strlen },
	{ "SDL_LogDebug", (uintptr_t)&ret0 },
	{ "SDL_HasEvents", (uintptr_t)&SDL_HasEvents },
	{ "SDL_RWseek", (uintptr_t)&SDL_RWseek },
	{ "SDL_JoystickNameForIndex", (uintptr_t)&SDL_JoystickNameForIndex },
	{ "SDL_JoystickNumButtons", (uintptr_t)&SDL_JoystickNumButtons },
	{ "SDL_JoystickGetGUID", (uintptr_t)&SDL_JoystickGetGUID },
	{ "SDL_JoystickGetGUIDString", (uintptr_t)&SDL_JoystickGetGUIDString },
	{ "SDL_JoystickNumHats", (uintptr_t)&SDL_JoystickNumHats },
	{ "SDL_JoystickNumBalls", (uintptr_t)&SDL_JoystickNumBalls },
	{ "SDL_JoystickName", (uintptr_t)&SDL_JoystickName_fake },
	{ "SDL_GetNumRenderDrivers", (uintptr_t)&SDL_GetNumRenderDrivers },
	{ "SDL_GetRenderDriverInfo", (uintptr_t)&SDL_GetRenderDriverInfo },
	{ "SDL_GetNumVideoDrivers", (uintptr_t)&SDL_GetNumVideoDrivers },
	{ "SDL_GetVideoDriver", (uintptr_t)&SDL_GetVideoDriver },
	{ "SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath_hook },
	{ "SDL_RenderReadPixels", (uintptr_t)&SDL_RenderReadPixels },
	{ "SDL_CreateRGBSurfaceFrom", (uintptr_t)&SDL_CreateRGBSurfaceFrom },
	{ "SDL_SetWindowBordered", (uintptr_t)&SDL_SetWindowBordered },
	{ "SDL_RestoreWindow", (uintptr_t)&SDL_RestoreWindow },
	{ "SDL_sqrt", (uintptr_t)&SDL_sqrt },
	{ "SDL_ThreadID", (uintptr_t)&SDL_ThreadID },
	{ "__system_property_get", (uintptr_t)&ret0 },
	{ "strnlen", (uintptr_t)&strnlen },
	{ "gethostname", (uintptr_t)&gethostname },
};

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	dlog("GetMethodID: %s\n", name);
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	dlog("GetStaticMethodID: %s\n", name);
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 1;
}

void *GetObjectArrayElement(void *env, uint8_t *obj, int idx) {
	return NULL;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

char duration[32];
void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return (void *)0x34343434;
	}
}

int CallIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetArrayLength(void *env, void *array) {
	dlog("GetArrayLength returned %d\n", *(int *)array);
	return *(int *)array;
}

/*int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}*/

void appThrowF(const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	sceClibPrintf("[appThrowF addr: %p] %s\n", __builtin_return_address(0), string);
#endif
}

int (*_CauseInputEvent)(void *this, int key, int action, float delta);
#define CauseInputEvent(x, y, z) _CauseInputEvent(this, x, y, z)
static float InputUpdateTime = 0.f;

enum EInputKey
{
    IK_None                 =0,
    IK_LeftMouse            =1,
    IK_RightMouse           =2,
    IK_Cancel               =3,
    IK_MiddleMouse          =4,
    IK_Unknown05            =5,
    IK_Unknown06            =6,
    IK_Unknown07            =7,
    IK_Backspace            =8,
    IK_Tab                  =9,
    IK_Unknown0A            =10,
    IK_Unknown0B            =11,
    IK_Unknown0C            =12,
    IK_Enter                =13,
    IK_Unknown0E            =14,
    IK_Unknown0F            =15,
    IK_Shift                =16,
    IK_Ctrl                 =17,
    IK_Alt                  =18,
    IK_Pause                =19,
    IK_CapsLock             =20,
    IK_Unknown15            =21,
    IK_Unknown16            =22,
    IK_Unknown17            =23,
    IK_Unknown18            =24,
    IK_Unknown19            =25,
    IK_Unknown1A            =26,
    IK_Escape               =27,
    IK_Unknown1C            =28,
    IK_Unknown1D            =29,
    IK_Unknown1E            =30,
    IK_Unknown1F            =31,
    IK_Space                =32,
    IK_PageUp               =33,
    IK_PageDown             =34,
    IK_End                  =35,
    IK_Home                 =36,
    IK_Left                 =37,
    IK_Up                   =38,
    IK_Right                =39,
    IK_Down                 =40,
    IK_Select               =41,
    IK_Print                =42,
    IK_Execute              =43,
    IK_PrintScrn            =44,
    IK_Insert               =45,
    IK_Delete               =46,
    IK_Help                 =47,
    IK_0                    =48,
    IK_1                    =49,
    IK_2                    =50,
    IK_3                    =51,
    IK_4                    =52,
    IK_5                    =53,
    IK_6                    =54,
    IK_7                    =55,
    IK_8                    =56,
    IK_9                    =57,
    IK_Unknown3A            =58,
    IK_Unknown3B            =59,
    IK_Unknown3C            =60,
    IK_Unknown3D            =61,
    IK_Unknown3E            =62,
    IK_Unknown3F            =63,
    IK_Unknown40            =64,
    IK_A                    =65,
    IK_B                    =66,
    IK_C                    =67,
    IK_D                    =68,
    IK_E                    =69,
    IK_F                    =70,
    IK_G                    =71,
    IK_H                    =72,
    IK_I                    =73,
    IK_J                    =74,
    IK_K                    =75,
    IK_L                    =76,
    IK_M                    =77,
    IK_N                    =78,
    IK_O                    =79,
    IK_P                    =80,
    IK_Q                    =81,
    IK_R                    =82,
    IK_S                    =83,
    IK_T                    =84,
    IK_U                    =85,
    IK_V                    =86,
    IK_W                    =87,
    IK_X                    =88,
    IK_Y                    =89,
    IK_Z                    =90,
    IK_Unknown5B            =91,
    IK_Unknown5C            =92,
    IK_Unknown5D            =93,
    IK_Unknown5E            =94,
    IK_Unknown5F            =95,
    IK_NumPad0              =96,
    IK_NumPad1              =97,
    IK_NumPad2              =98,
    IK_NumPad3              =99,
    IK_NumPad4              =100,
    IK_NumPad5              =101,
    IK_NumPad6              =102,
    IK_NumPad7              =103,
    IK_NumPad8              =104,
    IK_NumPad9              =105,
    IK_GreyStar             =106,
    IK_GreyPlus             =107,
    IK_Separator            =108,
    IK_GreyMinus            =109,
    IK_NumPadPeriod         =110,
    IK_GreySlash            =111,
    IK_F1                   =112,
    IK_F2                   =113,
    IK_F3                   =114,
    IK_F4                   =115,
    IK_F5                   =116,
    IK_F6                   =117,
    IK_F7                   =118,
    IK_F8                   =119,
    IK_F9                   =120,
    IK_F10                  =121,
    IK_F11                  =122,
    IK_F12                  =123,
    IK_F13                  =124,
    IK_F14                  =125,
    IK_F15                  =126,
    IK_F16                  =127,
    IK_F17                  =128,
    IK_F18                  =129,
    IK_F19                  =130,
    IK_F20                  =131,
    IK_F21                  =132,
    IK_F22                  =133,
    IK_F23                  =134,
    IK_F24                  =135,
    IK_Unknown88            =136,
    IK_Unknown89            =137,
    IK_Unknown8A            =138,
    IK_Unknown8B            =139,
    IK_Unknown8C            =140,
    IK_Unknown8D            =141,
    IK_Unknown8E            =142,
    IK_Unknown8F            =143,
    IK_NumLock              =144,
    IK_ScrollLock           =145,
    IK_Unknown92            =146,
    IK_Unknown93            =147,
    IK_Unknown94            =148,
    IK_Unknown95            =149,
    IK_Unknown96            =150,
    IK_Unknown97            =151,
    IK_Unknown98            =152,
    IK_Unknown99            =153,
    IK_Unknown9A            =154,
    IK_Unknown9B            =155,
    IK_Unknown9C            =156,
    IK_Unknown9D            =157,
    IK_Unknown9E            =158,
    IK_Unknown9F            =159,
    IK_LShift               =160,
    IK_RShift               =161,
    IK_LControl             =162,
    IK_RControl             =163,
    IK_UnknownA4            =164,
    IK_UnknownA5            =165,
    IK_UnknownA6            =166,
    IK_UnknownA7            =167,
    IK_UnknownA8            =168,
    IK_UnknownA9            =169,
    IK_UnknownAA            =170,
    IK_UnknownAB            =171,
    IK_UnknownAC            =172,
    IK_UnknownAD            =173,
    IK_UnknownAE            =174,
    IK_UnknownAF            =175,
    IK_UnknownB0            =176,
    IK_UnknownB1            =177,
    IK_UnknownB2            =178,
    IK_UnknownB3            =179,
    IK_UnknownB4            =180,
    IK_UnknownB5            =181,
    IK_UnknownB6            =182,
    IK_UnknownB7            =183,
    IK_UnknownB8            =184,
    IK_UnknownB9            =185,
    IK_Semicolon            =186,
    IK_Equals               =187,
    IK_Comma                =188,
    IK_Minus                =189,
    IK_Period               =190,
    IK_Slash                =191,
    IK_Tilde                =192,
    IK_UnknownC1            =193,
    IK_UnknownC2            =194,
    IK_UnknownC3            =195,
    IK_UnknownC4            =196,
    IK_UnknownC5            =197,
    IK_UnknownC6            =198,
    IK_UnknownC7            =199,
    IK_Joy1                 =200,
    IK_Joy2                 =201,
    IK_Joy3                 =202,
    IK_Joy4                 =203,
    IK_Joy5                 =204,
    IK_Joy6                 =205,
    IK_Joy7                 =206,
    IK_Joy8                 =207,
    IK_Joy9                 =208,
    IK_Joy10                =209,
    IK_Joy11                =210,
    IK_Joy12                =211,
    IK_Joy13                =212,
    IK_Joy14                =213,
    IK_Joy15                =214,
    IK_Joy16                =215,
    IK_UnknownD8            =216,
    IK_UnknownD9            =217,
    IK_UnknownDA            =218,
    IK_LeftBracket          =219,
    IK_Backslash            =220,
    IK_RightBracket         =221,
    IK_SingleQuote          =222,
    IK_UnknownDF            =223,
    IK_JoyX                 =224,
    IK_JoyY                 =225,
    IK_JoyZ                 =226,
    IK_JoyR                 =227,
    IK_MouseX               =228,
    IK_MouseY               =229,
    IK_MouseZ               =230,
    IK_MouseW               =231,
    IK_JoyU                 =232,
    IK_JoyV                 =233,
    IK_UnknownEA            =234,
    IK_UnknownEB            =235,
    IK_MouseWheelUp         =236,
    IK_MouseWheelDown       =237,
    IK_Unknown10E           =238,
    UK_Unknown10F           =239,
    IK_JoyPovUp             =240,
    IK_JoyPovDown           =241,
    IK_JoyPovLeft           =242,
    IK_JoyPovRight          =243,
    IK_UnknownF4            =244,
    IK_UnknownF5            =245,
    IK_Attn                 =246,
    IK_CrSel                =247,
    IK_ExSel                =248,
    IK_ErEof                =249,
    IK_Play                 =250,
    IK_Zoom                 =251,
    IK_NoName               =252,
    IK_PA1                  =253,
    IK_OEMClear             =254,
    IK_MAX                  =255,
};

enum EInputAction
{
    IST_None                =0,
    IST_Press               =1,
    IST_Hold                =2,
    IST_Release             =3,
    IST_Axis                =4,
    IST_MAX                 =5,
};

const uint8_t JoyButtonMap[SDL_CONTROLLER_BUTTON_MAX] =
{
	/* BUTTON_CROSS         */ IK_Space, // Jump
	/* BUTTON_CIRCLE        */ IK_P, // Weapon Slot 0 (Translator/Hammer)
	/* BUTTON_SQUARE        */ IK_C, // Crouch
	/* BUTTON_TRIANGLE      */ IK_B, // Best Weapon
	/* BUTTON_SELECT        */ IK_H, // Show Score
	/* BUTTON_UNUSED        */ IK_Joy6,
	/* BUTTON_START         */ IK_Escape, // Pause
	/* BUTTON_LEFTSTICK     */ IK_Z,
	/* BUTTON_RIGHTSTICK    */ IK_Joy9,
	/* BUTTON_LEFTSHOULDER  */ IK_RightMouse, // Alt Fire
	/* BUTTON_RIGHTSHOULDER */ IK_LeftMouse, // Fire
	/* BUTTON_DPAD_UP       */ IK_Y, // Feign Death
	/* BUTTON_DPAD_DOWN     */ IK_Joy6, // Open/Close Text Input
	/* BUTTON_DPAD_LEFT     */ IK_Q, // Prev Weapon
	/* BUTTON_DPAD_RIGHT    */ IK_N, // Next Weapon
};

const uint8_t JoyAxisMap[SDL_CONTROLLER_AXIS_MAX] =
{
	/* AXIS_LEFT_X          */ IK_JoyX,
	/* AXIS_LEFT_Y          */ IK_JoyY,
	/* AXIS_RIGHT_X         */ IK_MouseX,
	/* AXIS_RIGHT_Y         */ IK_MouseY,
	/* AXIS_LTRIGGER        */ IK_Joy12,
	/* AXIS_RTRIGGER        */ IK_Joy13,
};

const float JoyAxisDefaultScale[SDL_CONTROLLER_AXIS_MAX] =
{
	/* AXIS_LEFT_X          */ +60.f,
	/* AXIS_LEFT_Y          */ -60.f,
	/* AXIS_RIGHT_X         */ +60.f,
	/* AXIS_RIGHT_Y         */ +60.f,
	/* AXIS_LTRIGGER        */ +60.f,
	/* AXIS_RTRIGGER        */ +60.f,
};

static int JoyAxis[SDL_CONTROLLER_AXIS_MAX];

uint8_t UseJoystick = 0;
float ScaleXYZ = 1.f;
uint8_t InvertV = 0;
uint8_t InvertY = 0;
float ScaleRUV = 100.f;
float DeadZoneRUV = 0.1f, DeadZoneXYZ = 0.3f;

#define Abs(x) ((x) < 0 ? -(x) : (x))

#define CONSOLE_OFFSET 0x34
#define STATFRAME_OFFSET 0x0C
#define STATENODE_OFFSET 0x18
#define STATENAME_OFFSET 0x20
#define NAMEINDEX_MENU 5507

static inline int get_bInMenu(uint8_t *this) {
    uint8_t *Console = *(uint8_t **)(this + CONSOLE_OFFSET);
    uint8_t *StateFrame = *(uint8_t **)(Console + STATFRAME_OFFSET);
    uint8_t *StateNode = *(uint8_t **)(StateFrame + STATENODE_OFFSET);
    return *(int *)(StateNode + STATENAME_OFFSET) == NAMEINDEX_MENU;
}

void (*MouseDelta)(void *engine, void *vp, uint32_t click_flags, float dx, float dy);
static uint8_t is_textinput_active = 0;
int (*InputKey)(char key);

int TickInput(uint8_t *this) {
	// Detect if sceIme has been closed, if so exit text inputing mode
	if (is_textinput_active) {
		if (!SDL_IsScreenKeyboardShown(sdl_wnd)) {
			SDL_StopTextInput();
			is_textinput_active = 0;
		}
	}
	
	const float CurTime = ((float)sceKernelGetProcessTimeLow()) / 1000000.f;
	const float DeltaTime = CurTime - InputUpdateTime;
	
	float mouse_delta[2] = {0.f, 0.f};
#if 0
	float *mouse_pos = ((float *)this + 15);
	this[56] &= ~1; // This seemingly makes the cursor pop back in
#endif

	uint8_t *client = *(uint8_t **)(this + 240);
	uint8_t *engine = *(uint8_t **)(client + 40);
	
	uint8_t is_touching = 0;

	SDL_Event Ev;
	while( SDL_PollEvent( &Ev ) ) {
	switch( Ev.type )
	{
	case SDL_KEYDOWN:
		if (is_textinput_active && Ev.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
			CauseInputEvent(IK_Backspace, IST_Press, 0.f);
			CauseInputEvent(IK_Backspace, IST_Release, 0.f);
		}
		break;
	case SDL_TEXTINPUT:
		for( const char *p = Ev.text.text; *p && p < Ev.text.text + sizeof( Ev.text.text ); ++p ) {
			if ( *p < 0 )
				break;
			if( isprint( *p ) || *p == '\r' ) {
				InputKey(*p);
			}
		}
		break;
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		if (Ev.cbutton.button == 12) {
			if (Ev.type == SDL_CONTROLLERBUTTONDOWN && !is_textinput_active) {
				is_textinput_active = 1;
				SDL_StartTextInput();
			}
		} else {
			CauseInputEvent( JoyButtonMap[Ev.cbutton.button], ( Ev.type == SDL_CONTROLLERBUTTONDOWN ) ? IST_Press : IST_Release, 0.f );
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
#if 0
		CauseInputEvent( IK_LeftMouse, ( Ev.type == SDL_MOUSEBUTTONDOWN ) ? IST_Press : IST_Release, 0.f );
#endif
		break;
	case SDL_MOUSEMOTION:
		is_touching = 1;
#if 0
		mouse_pos[0] = Ev.motion.x;
		mouse_pos[1] = Ev.motion.y;
		this[56] |= 1; // Without this, the cursor doesn't warp (?)
#else
		CauseInputEvent( IK_MouseX, IST_Axis, Ev.motion.xrel * 2 );
		CauseInputEvent( IK_MouseY, IST_Axis, -Ev.motion.yrel * 2 );
		MouseDelta(engine, this, 0, Ev.motion.xrel, -Ev.motion.yrel * 2);
#endif
		break;
	case SDL_CONTROLLERAXISMOTION:
		{
			const uint8_t Key = JoyAxisMap[Ev.caxis.axis];
			const int PrevValue = JoyAxis[Ev.caxis.axis];
			int NewValue = Ev.caxis.value;
			int DeadZone = 0;
			if ( Key < IK_JoyX )
			{
				// Treat the axis like a trigger.
				if ( PrevValue < 8192 && NewValue >= 8192 ) {
					CauseInputEvent( Key, IST_Press, 0.f );
				} else if ( PrevValue >= 8192 && NewValue < 8192 ) {
					CauseInputEvent( Key, IST_Release, 0.f );
				}
			}
			else
			{
				// Apply deadzone.
				if ( Key >= IK_JoyX && Key <= IK_JoyZ )
					DeadZone = (int)(DeadZoneXYZ * 32767.f);
				else
					DeadZone = (int)(DeadZoneRUV * 32767.f);
				if ( Abs(NewValue) < DeadZone )
					NewValue = 0;
			}
			if ( Key >= IK_JoyX && Key <= IK_JoyZ ) {
				if (NewValue < 0 && JoyAxis[Ev.caxis.axis] >= 0) {
					if (Key == IK_JoyX) {
						CauseInputEvent( IK_A, IST_Press, 0.f );
					} else {
						CauseInputEvent( IK_W, IST_Press, 0.f );
					}
				} else if (NewValue > 0 && JoyAxis[Ev.caxis.axis] <= 0) {
					if (Key == IK_JoyX) {
						CauseInputEvent( IK_D, IST_Press, 0.f );
					} else {
						CauseInputEvent( IK_S, IST_Press, 0.f );
					}
				} else if (NewValue == 0 && JoyAxis[Ev.caxis.axis] != 0) {
					if (Key == IK_JoyX) {
						CauseInputEvent( IK_A, IST_Release, 0.f );
						CauseInputEvent( IK_D, IST_Release, 0.f );
					} else {
						CauseInputEvent( IK_W, IST_Release, 0.f );
						CauseInputEvent( IK_S, IST_Release, 0.f );
					}
				}
			}
			JoyAxis[Ev.caxis.axis] = NewValue;
		}
		break;
	default:
		break;
	}
	}
	
	if (!is_touching) {
		int isMenu = get_bInMenu(this);
		float x_gyro = 0.f, y_gyro = 0.f;
		if (UseJoystick && !isMenu) {
			SceMotionState motionstate;
			sceMotionGetState(&motionstate);
			x_gyro = -motionstate.angularVelocity.y * ScaleXYZ;
			y_gyro = (InvertY ? motionstate.angularVelocity.x : -motionstate.angularVelocity.x) * ScaleXYZ;
		}
		
		for ( int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i ) {
			const uint8_t Key = JoyAxisMap[i];
			const int Value = JoyAxis[i];
			if ( (Value || !isMenu) && Key >= IK_MouseX )
			{
				float FltValue = ((float)Value) / 32767.f;
				if (!isMenu) {
					FltValue += Key == IK_MouseX ? x_gyro : y_gyro;
				}
				if (FltValue < -1.f)
					FltValue = -1.f;
				else if (FltValue > 1.f)
					FltValue = 1.f;
				float Scale = isMenu ? 10.f : ScaleRUV;
				Scale *= JoyAxisDefaultScale[i] * DeltaTime;
				if ( !InvertV && Key == IK_MouseY )
					Scale = -Scale;
				FltValue *= Scale;
				if (Key == IK_MouseX)
					mouse_delta[0] = FltValue;
				else if (Key == IK_MouseY)
					mouse_delta[1] = FltValue;
				CauseInputEvent( Key, IST_Axis, FltValue );
			}
		}
		MouseDelta(engine, this, 0, mouse_delta[0], mouse_delta[1]);
	}
	
	InputUpdateTime = CurTime;
	
	return 0;
}

double appSeconds() {
	return (double)sceKernelGetProcessTimeLow() * 0.000001f;
}

void patch_game(void) {
	for (int i = 0; i < FDS_ARRAY_SIZE; i++) {
		fake_fds_pool[i] = &fake_fds[i];
	}
	
	// Optimized variants
	hook_addr(so_symbol(&main_mod, "_Z10appSecondsv"), (uintptr_t)appSeconds);
	hook_addr(so_symbol(&main_mod, "_Z9appCyclesv"), (uintptr_t)ret0);
	
	// Disable file logging
	hook_addr(so_symbol(&main_mod, "_ZN13FOutputDevice4LogfE5ENamePKcz"), (uintptr_t)ret0);
	hook_addr(so_symbol(&main_mod, "_ZN17FOutputDeviceFile9SerializeEPKc5EName"), (uintptr_t)ret0);
	
	// Hook appThrow for debugging purposes
	hook_addr(so_symbol(&main_mod, "_Z9appThrowfPKcz"), (uintptr_t)appThrowF);
	
	// Reimplement input code
	hook_addr(so_symbol(&main_mod, "_ZN13UNSDLViewport9TickInputEv"), (uintptr_t)TickInput);
	_CauseInputEvent = so_symbol(&main_mod, "_ZN13UNSDLViewport15CauseInputEventEi12EInputActionf");
	MouseDelta = so_symbol(&main_mod, "_ZN11UGameEngine10MouseDeltaEP9UViewportjff");
	InputKey = main_mod.text_base + 0x1D4F38;
}

void *pthread_main(void *arg) {
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
	
	int (*SDL_main)(int argc, char *argv[]) = (void *) so_symbol(&main_mod, "SDL_main");
	int (*nativePrepareProcess)(void *env, void* class, char *data_dir, char *home_dir) = (void *) so_symbol(&bridge_mod, "Java_com_ast_ut99_GameActivity_nativePrepareProcess");

	sceClibPrintf("nativePrepareProcess\n");
	nativePrepareProcess(fake_env, NULL, "ux0:data/ut99", "ux0:data/ut99");
	
	sceClibPrintf("Freeing bridge so\n");
	so_finalize(&bridge_mod);

	sceClibPrintf("Entering main loop\n");
	char *args[] = {
        "",
        "CityIntro",
        "LOG=UT99Vita.log",
        "INI=VitaUT99.ini",
        "USERINI=VitaUser.ini"
    };
    SDL_main(5, args);
	return NULL;
}

// List of files that can be removed since not used, saves space and reduces I/O overhead
static const char *to_purge_files[] = {
	"ux0:data/ut99/System/UNREALTOURNAMENT.EXE",
	"ux0:data/ut99/System/Core.dll",
	"ux0:data/ut99/System/D3DDrv.dll",
	"ux0:data/ut99/System/D3DDrv.est",
	"ux0:data/ut99/System/D3DDrv.frt",
	"ux0:data/ut99/System/D3DDrv.int",
	"ux0:data/ut99/System/D3DDrv.itt",
	"ux0:data/ut99/System/Editor.dll",
	"ux0:data/ut99/System/Editor.est",
	"ux0:data/ut99/System/Editor.frt",
	"ux0:data/ut99/System/Editor.int",
	"ux0:data/ut99/System/Editor.itt",
	"ux0:data/ut99/System/Engine.dll",
	"ux0:data/ut99/System/Fire.dll",
	"ux0:data/ut99/System/Galaxy.dll",
	"ux0:data/ut99/System/Galaxy.est",
	"ux0:data/ut99/System/Galaxy.frt",
	"ux0:data/ut99/System/Galaxy.int",
	"ux0:data/ut99/System/Galaxy.itt",
	"ux0:data/ut99/System/GlideDrv.dll",
	"ux0:data/ut99/System/GlideDrv.est",
	"ux0:data/ut99/System/GlideDrv.frt",
	"ux0:data/ut99/System/GlideDrv.int",
	"ux0:data/ut99/System/GlideDrv.itt",
	"ux0:data/ut99/System/IpDrv.dll",
	"ux0:data/ut99/System/MeTaLDrv.dll",
	"ux0:data/ut99/System/MeTaLDrv.est",
	"ux0:data/ut99/System/MeTaLDrv.frt",
	"ux0:data/ut99/System/MeTaLDrv.int",
	"ux0:data/ut99/System/MSVCRT.dll",
	"ux0:data/ut99/System/OpenGlDrv.dll",
	"ux0:data/ut99/System/OpenGlDrv.est",
	"ux0:data/ut99/System/OpenGlDrv.frt",
	"ux0:data/ut99/System/OpenGlDrv.ini",
	"ux0:data/ut99/System/OpenGlDrv.int",
	"ux0:data/ut99/System/OpenGlDrv.itt",
	"ux0:data/ut99/System/Render.dll",
	"ux0:data/ut99/System/SGLDrv.dll",
	"ux0:data/ut99/System/SglDrv.est",
	"ux0:data/ut99/System/SglDrv.frt",
	"ux0:data/ut99/System/SGLDrv.int",
	"ux0:data/ut99/System/SglDrv.itt",	
	"ux0:data/ut99/System/SoftDrv.dll",
	"ux0:data/ut99/System/SoftDrv.est",
	"ux0:data/ut99/System/SoftDrv.frt",
	"ux0:data/ut99/System/SoftDrv.int",
	"ux0:data/ut99/System/SoftDrv.itt",
	"ux0:data/ut99/System/UWeb.dll",
	"ux0:data/ut99/System/Window.dll",
	"ux0:data/ut99/System/WinDrv.dll",
	"ux0:data/ut99/System/WinDrv.est",
	"ux0:data/ut99/System/WinDrv.frt",
	"ux0:data/ut99/System/WinDrv.int",
	"ux0:data/ut99/System/WinDrv.itt",
	"ux0:data/ut99/System/RunServer.bat",
	"ux0:data/ut99/System/GotoHeat.exe",
	"ux0:data/ut99/System/mplaynow.exe",
	"ux0:data/ut99/System/Setup.exe",
	"ux0:data/ut99/System/Setup.est",
	"ux0:data/ut99/System/Setup.frt",
	"ux0:data/ut99/System/Setup.int",
	"ux0:data/ut99/System/Setup.itt",
	"ux0:data/ut99/System/SetupUnreal.frt",
	"ux0:data/ut99/System/SetupUnrealTournament.est",
	"ux0:data/ut99/System/SetupUnrealTournament.frt",
	"ux0:data/ut99/System/SetupUnrealTournament.itt",
	"ux0:data/ut99/System/UCC.exe",
	"ux0:data/ut99/System/UnrealEd.exe",
	"ux0:data/ut99/System/GlobalStatsWeb.url",
	"ux0:data/ut99/System/NetGamesUsaWeb.url",
	"ux0:data/ut99/System/UnrealTournamentWeb.url",
	"ux0:data/ut99/System/User.ini",
	"ux0:data/ut99/System/Default.ini",
	"ux0:data/ut99/System/DefUser.ini",
	"ux0:data/ut99/System/UnrealTournament.ini",
};

// Files that need case sensitiveness fixes
static const char *to_case_fix[] = {
	"ux0:data/ut99/Textures/hubeffects.utx",
	"ux0:data/ut99/Textures/uttech2.utx",
	"ux0:data/ut99/Textures/genfluid.utx",
	"ux0:data/ut99/Textures/utcrypt.utx",
	"ux0:data/ut99/System/Botpack.u",
	"ux0:data/ut99/Maps/CityIntro.unr",
	"ux0:data/ut99/Textures/Skybox.utx",
	"ux0:data/ut99/Textures/shaneDAY.utx",
	"ux0:data/ut99/Textures/CommandoSkins.utx",
};

int main(int argc, char *argv[]) {
	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(SceAppUtilInitParam));
	memset(&boot_param, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&init_param, &boot_param);
	
	SceAppUtilAppEventParam eventParam;
	memset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
	sceAppUtilReceiveAppEvent(&eventParam);
	if (eventParam.type == 0x05) {
		eglSwapInterval(NULL, 2);
	}
	
	sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
	//sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);	
	
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	
	if (file_exists(to_purge_files[0])) {
		// Purge useless files
		for (int i = 0; i < sizeof(to_purge_files) / sizeof(*to_purge_files); i++) {
			sceIoRemove(to_purge_files[i]);
		}
		
		// Rename files to prevent case mismatches and subsequent cache misses
		for (int i = 0; i < sizeof(to_case_fix) / sizeof(*to_case_fix); i++) {
			sceIoRename(to_case_fix[i], "ux0:data/t.u");
			sceIoRename("ux0:data/t.u", to_case_fix[i]);
		}
	}

#ifdef HAVE_FILEPATH_CACHE	
	// Cache directory structs
	#define CACHE_DIR(x, i) \
	{ \
		SceUID d = sceIoDopen(x); \
		SceIoDirent sce_dir; \
		while (sceIoDread(d, &sce_dir) > 0) { \
			sprintf(cached_dirs[i][cached_entries[i]].fullpath, "%s/%s", x, sce_dir.d_name); \
			strcpy(cached_dirs[i][cached_entries[i]].d_name, sce_dir.d_name); \
			cached_dirs[i][cached_entries[i]].d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG; \
			char * s = strstr(sce_dir.d_name, "."); \
			if (s) { \
				if (s[1] == 'u') { \
					s[0] = 0; \
				} else { \
					while (*s) s++; \
				}\
				uint64_t hash = XXH3_64bits(sce_dir.d_name, s - sce_dir.d_name); \
				cache_insert(hash, cached_dirs[i][cached_entries[i]].fullpath); \
			} \
			cached_entries[i]++; \
		} \
		sceIoDclose(d); \
	}
	CACHE_DIR("ux0:data/ut99/System", SYSTEM_DIR)
	CACHE_DIR("ux0:data/ut99/Maps", MAPS_DIR)
	CACHE_DIR("ux0:data/ut99/Music", MUSIC_DIR)
	CACHE_DIR("ux0:data/ut99/Sounds", SOUNDS_DIR)
	CACHE_DIR("ux0:data/ut99/Textures", TEXTURES_DIR)
#endif
	
	#define extractValue(val) \
		{ \
			char *s = strstr(tmp, #val "="); \
			if (s) { \
				char *end = strstr(s, "\n"); \
				sceClibMemcpy(line, s, end - s); \
				line[end - s] = 0; \
				sscanf(line, #val "=%f", &val); \
				sceClibPrintf(#val " set to %f\n", val); \
			} \
		}
		
	#define extractBoolValue(val) \
		{ \
			char *s = strstr(tmp, #val "="); \
			if (s) { \
				val = s[strlen(#val "=")] == 'T'; \
				sceClibPrintf(#val " set to %s\n", val ? "True" : "False"); \
			} \
		}	
	
	FILE *f = sceLibcBridge_fopen("ux0:data/ut99/System/VitaUT99.ini", "rb");
	char *tmp = vglMalloc(1024 * 1024);
	sceLibcBridge_fread(tmp, 1, 1024 * 1024, f);
	sceLibcBridge_fclose(f);
	char line[32];
	extractValue(DeadZoneXYZ)
	extractValue(DeadZoneRUV)
	extractValue(ScaleRUV)
	extractValue(ScaleXYZ)
	extractBoolValue(InvertY)
	extractBoolValue(InvertV)
	extractBoolValue(UseJoystick)
	vglFree(tmp);
	
	if (UseJoystick) {
		sceMotionReset();
		sceMotionStartSampling();
	}

	if (check_kubridge() < 0)
		fatal_error("Error kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error libshacccg.suprx is not installed.");
	
	char fname[256];
	sprintf(data_path, "ux0:data/ut99");
	
	sceClibPrintf("Loading libut99dc_android_bridge\n");
	sprintf(fname, "%s/libut99dc_android_bridge.so", data_path);
	if (so_file_load(&bridge_mod, fname, LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&bridge_mod);
	so_resolve(&bridge_mod, default_dynlib, sizeof(default_dynlib), 0);
	so_flush_caches(&bridge_mod);
	so_initialize(&bridge_mod);
	
	sceClibPrintf("Loading libUnrealTournament\n");
	sprintf(fname, "%s/libUnrealTournament.so", data_path);
	if (so_file_load(&main_mod, fname, LOAD_ADDRESS + 0x01000000) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&main_mod);
	so_resolve(&main_mod, default_dynlib, sizeof(default_dynlib), 0);

	vglSetCircularPoolSize(3 * 1024 * 1024);
	vglSetParamBufferSize(6 * 1024 * 1024);
	vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, 1024 * 1024, 0, 0, 0, SCE_GXM_MULTISAMPLE_4X);

	patch_game();
	so_flush_caches(&main_mod);
	so_initialize(&main_mod);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x44) = (uintptr_t)ret0; // NewLocalRef
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B4) = (uintptr_t)GetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)ret0; // RegisterNatives
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
	*(uintptr_t *)(fake_env + 0x390) = (uintptr_t)ret0; // CallStaticVoidMethodV

	pthread_t t2;
	pthread_attr_t attr2;
	pthread_attr_init(&attr2);
	pthread_attr_setstacksize(&attr2, 2 * 1024 * 1024);
	pthread_create(&t2, &attr2, pthread_main, NULL);

	return sceKernelExitDeleteThread(0);
}
