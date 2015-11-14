#include <stdio.h>
#include <stdint.h>
#ifndef _MSC_VER
#include <stdbool.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include "libretro.h"
#include "memstream.h"

#include "../src/snes9x.h"
#include "../src/memmap.h"
#include "../src/cpuexec.h"
#include "../src/srtc.h"
#include "../src/apu.h"
#include "../src/ppu.h"
#include "../src/snapshot.h"
#include "../src/soundux.h"
#include "../src/cheats.h"
#include "../src/display.h"
#include "../src/os9x_asm_cpu.h"

#ifdef _3DS
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#endif

#define MAP_BUTTON(id, name) S9xMapButton((id), S9xGetCommandT((name)), false)
#define MAKE_BUTTON(pad, btn) (((pad)<<4)|(btn))

#define BTN_POINTER (RETRO_DEVICE_ID_JOYPAD_R + 1)
#define BTN_POINTER2 (BTN_POINTER + 1)

static retro_video_refresh_t video_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_environment_t environ_cb = NULL;

static uint32 joys[5];

bool8 ROMAPUEnabled = 0;
char currentWorkingDir[MAX_PATH+1] = {0};

memstream_t *s_stream;

int s_open(const char *fname, const char *mode)
{
	s_stream = memstream_open();
	return TRUE;
}

int s_read(void *p, int l)
{
	return memstream_read(s_stream, p, l);
}

int s_write(void *p, int l)
{
	return memstream_write(s_stream, p, l);
}

void s_close()
{
	memstream_close(s_stream);
}

int  (*statef_open)(const char *fname, const char *mode) = s_open;
int  (*statef_read)(void *p, int l) = s_read;
int  (*statef_write)(void *p, int l) = s_write;
void (*statef_close)() = s_close;



void *retro_get_memory_data(unsigned type)
{
   uint8_t* data;

   switch(type)
   {
      case RETRO_MEMORY_SAVE_RAM:
         data = Memory.SRAM;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         data = Memory.RAM;
         break;
      case RETRO_MEMORY_VIDEO_RAM:
         data = Memory.VRAM;
         break;
      default:
         data = NULL;
         break;
   }

   return data;
}

size_t retro_get_memory_size(unsigned type)
{
   unsigned size;

   switch(type)
   {
      case RETRO_MEMORY_SAVE_RAM:
         size = (unsigned) (Memory.SRAMSize ? (1 << (Memory.SRAMSize + 3)) * 128 : 0);
         if (size > 0x20000)
            size = 0x20000;
         break;
      /*case RETRO_MEMORY_RTC:
         size = (Settings.SRTC || Settings.SPC7110RTC)?20:0;
         break;*/
      case RETRO_MEMORY_SYSTEM_RAM:
         size = 128 * 1024;
         break;
      case RETRO_MEMORY_VIDEO_RAM:
         size = 64 * 1024;
         break;
      default:
         size = 0;
         break;
   }

   return size;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

static bool use_overscan;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->need_fullpath = false;
   info->valid_extensions = "smc|fig|sfc|gd3|gd7|dx2|bsx|swc|zip|SMC|FIG|SFC|BSX|GD3|GD7|DX2|SWC|ZIP";
   info->library_version = "7.2.0";
   info->library_name = "PocketSNES";
   info->block_extract = false;
}

static int16 audio_buf[0x10000];
static unsigned avail;
static float samplerate = 32040.5f;

void S9xGenerateSound()
{
}

uint32 S9xReadJoypad(int which1)
{
    if (which1 > 4)
        return 0;
	return joys[which1];
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width = SNES_WIDTH;
   info->geometry.base_height = SNES_HEIGHT;
   info->geometry.max_width = 512;
   info->geometry.max_height = 512;

   if(PPU.ScreenHeight == SNES_HEIGHT_EXTENDED)
      info->geometry.base_height = SNES_HEIGHT_EXTENDED;

   if (!Settings.PAL)
      info->timing.fps = 21477272.0 / 357366.0;
   else
      info->timing.fps = 21281370.0 / 425568.0;

   info->timing.sample_rate = samplerate;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
}

static void snes_init (void)
{
   memset(&Settings, 0, sizeof(Settings));
	Settings.JoystickEnabled = FALSE;
	Settings.SoundPlaybackRate = samplerate;
	Settings.Stereo = TRUE;
	Settings.SoundBufferSize = 0;
	Settings.CyclesPercentage = 100;
	Settings.DisableSoundEcho = FALSE;
	Settings.APUEnabled = FALSE;
	Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
	Settings.SkipFrames = AUTO_FRAMERATE;
	Settings.Shutdown = Settings.ShutdownMaster = TRUE;
	Settings.FrameTimePAL = 20000;
	Settings.FrameTimeNTSC = 16667;
	Settings.FrameTime = Settings.FrameTimeNTSC;
	Settings.DisableSampleCaching = FALSE;
	Settings.DisableMasterVolume = FALSE;
	Settings.Mouse = FALSE;
	Settings.SuperScope = FALSE;
	Settings.MultiPlayer5 = FALSE;
	//	Settings.ControllerOption = SNES_MULTIPLAYER5;
	Settings.ControllerOption = 0;
	
	Settings.ForceTransparency = FALSE;
	Settings.Transparency = TRUE;
	Settings.SixteenBit = TRUE;
	
	Settings.SupportHiRes = FALSE;
	Settings.NetPlay = FALSE;
	Settings.ServerName [0] = 0;
	Settings.AutoSaveDelay = 30;
	Settings.ApplyCheats = TRUE;
	Settings.TurboMode = FALSE;
	Settings.TurboSkipFrames = 15;
	Settings.ThreadSound = FALSE;
	Settings.SoundSync = FALSE;
#ifdef ASMCPU
	Settings.asmspc700 = TRUE;
#endif
	Settings.SpeedHacks = TRUE;

	Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;

   Settings.InterpolatedSound = TRUE;
   /*
   	Settings.SoundPlaybackRate = 5;
    Settings.Stereo = TRUE;
    Settings.SoundBufferSize = 0;
    Settings.DisableSoundEcho = 0;
    Settings.AltSampleDecode = 0;
    Settings.SoundEnvelopeHeightReading = FALSE;
    Settings.FixFrequency = 0;
    Settings.CyclesPercentage = 100;
    Settings.InterpolatedSound = TRUE;
    Settings.APUEnabled = Settings.NextAPUEnabled = TRUE;
    Settings.SoundMixInterval = 0;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.SkipFrames = 10;
    Settings.ShutdownMaster = TRUE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.DisableSampleCaching = FALSE;
    Settings.DisableMasterVolume = FALSE;
    Settings.Mouse = FALSE;
    Settings.SuperScope = FALSE;
    Settings.MultiPlayer5 = FALSE;
    Settings.TurboMode = FALSE;
    Settings.TurboSkipFrames = 40;
    Settings.ControllerOption = SNES_MULTIPLAYER5;
    Settings.Transparency = TRUE;
    Settings.SixteenBit = TRUE;
    Settings.SupportHiRes = TRUE;
    Settings.NetPlay = FALSE;
    Settings.ServerName [0] = 0;
    Settings.ThreadSound = FALSE;
    Settings.AutoSaveDelay = 30;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.DisplayFrameRate = FALSE;
    Settings.ReverseStereo = TRUE;
	*/

   CPU.Flags = 0;

   if (!MemoryInit() || !S9xInitAPU())
   {
      MemoryDeinit();
      S9xDeinitAPU();
      fprintf(stderr, "[libsnes]: Failed to init Memory or APU.\n");
      exit(1);
   }

   if (!S9xInitSound() || !S9xGraphicsInit()) exit(1);
   //S9xSetSamplesAvailableCallback(S9xAudioCallback);

   GFX.Pitch = use_overscan ? 1024 : 2048;
   
   const int safety = 128;
   // hack to make sure GFX.Delta is always  (2048 * 512 * 2) >> 1, needed for tile16_t.h
#ifdef _3DS
   GFX.Screen_buffer = (uint8 *) linearMemAlign(2048 * 512 * 2 * 2 + safety, 0x80);
#else
   GFX.Screen_buffer = (uint8 *) calloc(1, 2048 * 512 * 2 * 2 + safety);
#endif
   GFX.Screen = GFX.Screen_buffer + safety;

   GFX.SubScreen = GFX.Screen + 2048 * 512 * 2;
   GFX.ZBuffer_buffer = (uint8 *) calloc(1, GFX.Pitch * 512 * sizeof(uint16) + safety);
   GFX.ZBuffer = GFX.ZBuffer_buffer + safety;
   GFX.SubZBuffer_buffer = (uint8 *) calloc(1, GFX.Pitch * 512 * sizeof(uint16) + safety);
   GFX.SubZBuffer = GFX.SubZBuffer_buffer + safety;
   GFX.Delta = 1048576; //(GFX.SubScreen - GFX.Screen) >> 1;

   if (GFX.Delta != ((GFX.SubScreen - GFX.Screen) >> 1))
   {
      printf("BAD DELTA! (is %u, should be %u)\n", ((GFX.SubScreen - GFX.Screen) >> 1), GFX.Delta);
      exit(1);
   }

   /* controller port 1 */
   //S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
   //retro_devices[0] = RETRO_DEVICE_JOYPAD;

   /* controller port 2 */
   //S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);
   //retro_devices[1] = RETRO_DEVICE_JOYPAD;

   //S9xUnmapAllControls();
   //map_buttons();
   
   //S9xSetSoundMute(FALSE);
}

void retro_init (void)
{
   if (!environ_cb(RETRO_ENVIRONMENT_GET_OVERSCAN, &use_overscan))
	   use_overscan = FALSE;

   snes_init();
}

/* libsnes uses relative values for analogue devices. 
   S9x seems to use absolute values, but do convert these into relative values in the core. (Why?!)
   Hack around it. :) */

void retro_deinit(void)
{
   S9xDeinitAPU();
   MemoryDeinit();
   S9xGraphicsDeinit();
   //S9xUnmapAllControls();
   if(GFX.Screen_buffer)
#ifdef _3DS
      linearFree(GFX.Screen_buffer);
#else
      free(GFX.Screen_buffer);
#endif
   GFX.Screen_buffer = NULL;
   GFX.Screen = NULL;
   GFX.SubScreen = NULL;

   if(GFX.ZBuffer_buffer)
      free(GFX.ZBuffer_buffer);
   GFX.ZBuffer_buffer = NULL;

   if(GFX.SubZBuffer_buffer)
      free(GFX.SubZBuffer_buffer);

   GFX.SubZBuffer_buffer = NULL;


}

void retro_reset (void)
{
   S9xReset();
}

//static int16_t retro_mouse_state[2][2] = {{0}, {0}};
//static int16_t retro_scope_state[2] = {0};
//static int16_t retro_justifier_state[2][2] = {{0}, {0}};
void S9xSetButton(int i, uint16 b, bool pressed);

static void report_buttons (void)
{
	int i, j;
	for ( i = 0; i < 5; i++)
	{
		for (j = 0; j <= RETRO_DEVICE_ID_JOYPAD_R; j++)
		{
			if (input_cb(i, RETRO_DEVICE_JOYPAD, 0, j))
				joys[i] |= (1 << (15 - j));
			else
				joys[i] &= ~(1 << (15 - j));
		}
	}
}

//#define FRAME_SKIP

void retro_run (void)
{
#ifdef FRAME_SKIP
   IPPU.RenderThisFrame = !IPPU.RenderThisFrame;
#else
   IPPU.RenderThisFrame = TRUE;
#endif

   S9xMainLoop();
//   asm_S9xMainLoop();
   S9xMixSamples(audio_buf, avail);
   audio_batch_cb((int16_t *) audio_buf, avail >> 1);

#ifdef FRAME_SKIP
   if(!IPPU.RenderThisFrame)
      video_cb(NULL, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, GFX_PITCH);
#endif

   poll_cb();

   report_buttons();
}

size_t retro_serialize_size (void)
{
   uint8_t *tmpbuf;

   tmpbuf = (uint8_t*)malloc(5000000);
   memstream_set_buffer(tmpbuf, 5000000);
   S9xFreezeGame("");
   free(tmpbuf);
   return memstream_get_last_size();
}

bool retro_serialize(void *data, size_t size)
{
   memstream_set_buffer((uint8_t*)data, size);
   if (S9xFreezeGame("") == FALSE)
      return FALSE;

   return TRUE;
}

bool retro_unserialize(const void * data, size_t size)
{
   memstream_set_buffer((uint8_t*)data, size);
   if (S9xUnfreezeGame("") == FALSE)
      return FALSE;

   return TRUE;
}

void retro_cheat_reset(void)
{
    S9xDeleteCheats();
}

void retro_cheat_set(unsigned index, bool enable, const char* in_code)
{
    // clean input
    char clean_code[strlen(in_code)];
    int j =0;
    unsigned i;

    for (i = 0; i < strlen(in_code); i++)          
    {
        switch (in_code[i])
        {
            case 'a': case 'A':
            case 'b': case 'B':
            case 'c': case 'C':
            case 'd': case 'D':
            case 'e': case 'E':
            case 'f': case 'F':
            
            case '-': case '0':
            case '1': case '2': case '3':
            case '4': case '5': case '6': 
            case '7': case '8': case '9':
                clean_code[j++]=in_code[i];
                break;
            default:
                break;
        }
    }
    clean_code[j]=0;
    uint32 address;
    uint8 byte;
    
    if ( S9xProActionReplayToRaw(clean_code, &address, &byte) == NULL)
        S9xAddCheat(true, true, address, byte);
    else if ( S9xGameGenieToRaw(clean_code, &address, &byte) == NULL)
        S9xAddCheat(true, true, address, byte);
    /* else, silently ignore */
}

bool retro_load_game(const struct retro_game_info *game)
{
   bool8 loaded;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      fprintf(stderr, "[libretro]: RGB565 is not supported.\n");
      return false;
   }

   /* Hack. S9x cannot do stuff from RAM. <_< */
   memstream_set_buffer((uint8_t*)game->data, game->size);

   loaded = LoadROM("");
   if (!loaded)
   {
      fprintf(stderr, "[libretro]: Rom loading failed...\n");
      return false;
   }

   //S9xGraphicsInit();
   S9xReset();
   CPU.APU_APUExecuting = Settings.APUEnabled = 1;
   Settings.SixteenBitSound = true;
   so.stereo = Settings.Stereo;
   so.playback_rate = Settings.SoundPlaybackRate;
   S9xSetPlaybackRate(so.playback_rate);
   S9xSetSoundMute(FALSE);

   avail = (int) (samplerate / (Settings.PAL ? 50 : 60)) << 1;

   ZeroMemory(audio_buf, sizeof(audio_buf));

   return true;
}

bool retro_load_game_special(
  unsigned game_type,
  const struct retro_game_info *info, size_t num_info
)
{ return false; }

void retro_unload_game (void)
{ }

unsigned retro_get_region (void)
{ 
   return Settings.PAL ? RETRO_REGION_PAL : RETRO_REGION_NTSC; 
}

bool8 S9xDeinitUpdate(int width, int height, bool8 sixteen_bit)
{
	int y;

	if (height == 448 || height == 478)
	{
		/* Pitch 2048 -> 1024, only done once per res-change. */
		if (GFX.Pitch == 2048)
		{
			for ( y = 1; y < height; y++)
			{
				uint8_t *src = GFX.Screen + y * 1024;
				uint8_t *dst = GFX.Screen + y * 512;
				memcpy(dst, src, width * sizeof(uint8_t) * 2);
			}
		}
		GFX.Pitch = 1024;
	}
	else
	{
		/* Pitch 1024 -> 2048, only done once per res-change. */
		if (GFX.Pitch == 1024)
		{
			for ( y = height - 1; y >= 0; y--)
			{
				uint8_t *src = GFX.Screen + y * 512;
				uint8_t *dst = GFX.Screen + y * 1024;
				memcpy(dst, src, width * sizeof(uint8_t) * 2);
			}
		}
		GFX.Pitch = 2048;
	}

	video_cb(GFX.Screen, width, height, GFX_PITCH);
	
	return TRUE;
}


/* Dummy functions that should probably be implemented correctly later. */
const char* S9xGetFilename(const char* in) { return in; }
const char* S9xGetFilenameInc(const char* in) { return in; }
const char *S9xGetHomeDirectory() { return NULL; }
const char *S9xGetSnapshotDirectory() { return NULL; }
const char *S9xGetROMDirectory() { return NULL; }
const char* S9xChooseFilename(bool8 a) { return NULL; }
bool8 S9xInitUpdate() { return TRUE; }
bool8 S9xContinueUpdate(int width, int height) { return TRUE; }
void S9xSetPalette() {}
void S9xAutoSaveSRAM() {}
void S9xLoadSDD1Data() {}
bool8 S9xReadMousePosition (int which1_0_to_1, int* x, int* y, uint32* buttons) { return FALSE; }
bool8 S9xReadSuperScopePosition (int* x, int* y, uint32* buttons) { return FALSE; }
bool JustifierOffscreen() { return false; }

START_EXTERN_C

void S9xToggleSoundChannel (int channel) {}

bool8 S9xMovieActive() { return FALSE; }
bool8 S9xMoviePlaying() { return FALSE; }
void S9xMovieFreeze() {}
void S9xMovieUnfreeze() {}
int S9xMovieCreate (const char* filename, uint8 controllers_mask, uint8 opts, const wchar_t* metadata, int metadata_length) { return FALSE; }
void S9xMovieStop (bool8 suppress_message) {}
const char *S9xChooseMovieFilename(bool8 read_only) { return NULL; }
void S9xMovieUpdate(bool addFrame) {}
void S9xMovieUpdateOnReset() {}
int S9xMovieOpen(const char* filename, bool8 read_only) { return FALSE; }
uint32 S9xMovieGetFrameCounter() { return 0; }
const char *S9xStringInput(const char *message) { return NULL; }

END_EXTERN_C

//void Write16(uint16 v, uint8*& ptr) {}
//uint16 Read16(const uint8*& ptr) { return 0; }

//void S9xHandlePortCommand(s9xcommand_t cmd, int16 data1, int16 data2) {}
//bool S9xPollButton(uint32 id, bool *pressed) { return false; }
//bool S9xPollPointer(uint32 id, int16 *x, int16 *y) { return false; }
//bool S9xPollAxis(uint32 id, int16 *value) { return false; }

void S9xExit() { exit(1); }
bool8 S9xOpenSoundDevice (int mode, bool8 stereo, int buffer_size) {
	//so.sixteen_bit = 1;
	so.stereo = TRUE;
	//so.buffer_size = 534;
	so.playback_rate = samplerate;
	return TRUE;
}

const char *emptyString = "";
const char *S9xBasename (const char *filename) { return emptyString; }
bool8 S9xOpenSnapshotFile (const char *base, bool8 read_only, STREAM *file) { *file = OPEN_STREAM(0, 0); return TRUE; }
void S9xCloseSnapshotFile (STREAM file) { CLOSE_STREAM(file); }

void S9xMessage(int a, int b, const char* msg)
{
   fprintf(stderr, "%s\n", msg);
}

/* S9x weirdness. */
#ifndef _WIN32
void _splitpath (const char * path, char * drive, char * dir, char * fname, char * ext)
{
	const char *slash, *dot;

	slash = strrchr(path, SLASH_CHAR);
	dot   = strrchr(path, '.');

	if (dot && slash && dot < slash)
		dot = NULL;

	if (!slash)
	{
		*dir = 0;

		strcpy(fname, path);

		if (dot)
		{
			fname[dot - path] = 0;
			strcpy(ext, dot + 1);
		}
		else
			*ext = 0;
	}
	else
	{
		strcpy(dir, path);
		dir[slash - path] = 0;

		strcpy(fname, slash + 1);

		if (dot)
		{
			fname[dot - slash - 1] = 0;
			strcpy(ext, dot + 1);
		}
		else
			*ext = 0;
	}
}

void _makepath (char *path, const char * a, const char *dir, const char *fname, const char *ext)
{
   if (dir && *dir)
   {
      strcpy(path, dir);
      strcat(path, SLASH_STR);
   }
   else
      *path = 0;

   strcat(path, fname);

   if (ext && *ext)
   {
      strcat(path, ".");
      strcat(path, ext);
   }
}
#endif

