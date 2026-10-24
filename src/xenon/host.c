// swanstation: SwanStation on the Xbox 360, its libretro core linked in and driven from here (xsdk's
// docs/scope/62-swanstation.md). It opens the game picker (picker.cpp) on the games in games\ beside the .xex and on
// USB drives; pressing both sticks in for a second in a game opens the pause menu. host.txt beside the .xex, which can
// be left out, changes that, one KEY=VALUE a line:
//   game=game:\Spyro the Dragon.cue   starts that disc image, from any folder, without the picker
//   cpu=CachedInterpreter             the core's CPU mode: Interpreter, CachedInterpreter or Recompiler (the
//                                     default; no fastmem yet)
//   frames=1800                       ends the title after that many frames of game=; without it, it runs until
//                                     Xbox Home
//   capture=600,1200                  writes the core's frame at those frames as capture_<frame>.ppm beside the .xex
//   report=1                          logs, every 300 frames, the time the core took for a frame, the scope's measure
//                                     of full speed, where it went, and how the sound kept up; and each frame that
//                                     ran slow or missed its vblank. Each log line is a write to the drive the game
//                                     streams from, which can hold a frame up, so play goes without.
//   check=list                       the picker's listing check (picker_check_list)
//   check=reload                      starts game= four times over, 300 frames each or frames=, and checks each
//                                     start ended on a frame that isn't black
//   check=shots                       opens the pause menu SHOT_FRAME frames into game= and writes it as a PPM, then
//                                     the picker's (picker_shots)
// The BIOS goes in the title's folder too, where the core looks for one by its contents. Frames are drawn 4:3 in the
// middle of a 1280x720 display, pads are the PlayStation's digital pad, and the sound plays from the front left and
// right speakers. When a game ends, the log gives its frames, the vblanks that showed a frame again and the gaps in
// its sound.

#include <libretro.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/xenon_profile.h"
#include "picker.h"
#include "swanstation_shaders.h"
#include "xsdk/audio.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

enum {
	SPLASH_MS = 2000,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	// The Guide's first opening held a frame up 2.7 s (xsdk's docs/HARDWARE.md, "The Guide, milestone 37").
	BEGIN_TIMEOUT_MS = 5000,
	FENCE_TIMEOUT_MS = 1000,
	LINE_BYTES = 512,
	MAX_CAPTURES = 8,
	REPORT_FRAMES = 300,
	// The time base's rate (xsdk's docs/HARDWARE.md, "The time base runs at 49.875 MHz").
	TIME_BASE_HZ = 49875000,
	// A trigger past a quarter of its travel presses L2 or R2.
	TRIGGER_PRESSED = 64,
	CORNERS = 4,
	BLACK = 0xFF000000,
	// The feeder's hardware thread: the third core's second, beside the CD-ROM read thread, which mostly waits.
	AUDIO_HARDWARE_THREAD = 5,
	AUDIO_STACK_BYTES = 64 * 1024,
	// The ring's room, 186 ms at 44.1 kHz, a power of two; the fill the feeder steers for, 46 ms, about three of the
	// core's frames of sound.
	AUDIO_RING_FRAMES = 8192,
	AUDIO_TARGET_FRAMES = 2048,
	AUDIO_WAIT_MS = 100,
	// A frame of the core's past this, beyond a 60 Hz frame and then some, is logged with where its time went.
	SLOW_FRAME_US = 20000,
	// How long both sticks are pressed in together to open the pause menu.
	PAUSE_HOLD_US = 1000000,
	// The missed vblanks and gaps in the sound a game's last line names, at most.
	MARKS = 16,
	RELOAD_RUNS = 4,
	RELOAD_FRAMES = 300,
	SHOT_FRAME = 600,
};

// How far the feeder's step leans with the ring's fill, either way: more than the 0.3% between the display's 60 Hz and
// the PlayStation's 59.82, too little to hear.
static const double AUDIO_RATE_LEAN = 0.005;

// The core's files, the BIOS and memory cards, and the captures: the title's folder.
static const char DATA_DIRECTORY[] = "game:";
static const char CONFIG_NAME[] = "host.txt";
// The PlayStation's 4:3 across the middle of 16:9: 960 of 1280 columns.
static const float FRAME_HALF_WIDTH = 0.75f;

// A corner of the quad the frame is drawn on: its place in clip space, and where it sits in the frame.
typedef struct {
	float x, y, u, v;
} Corner;

enum { VERTICES_BYTES = CORNERS * sizeof(Corner) };

typedef struct {
	char game[LINE_BYTES];
	char cpu[LINE_BYTES];
	uint32_t frames;
	uint32_t captures[MAX_CAPTURES];
	uint32_t capture_count;
	char check[LINE_BYTES];
	bool report;
} Config;

// How a game's run ended: frames= reached or the display failed, or the pause menu's Back to games.
typedef enum {
	RUN_ENDED,
	RUN_TO_GAMES,
} RunEnd;

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	XsdkSamplerState sampler;
	volatile uint32_t *vertices;
	// The core's last frame, which every display frame draws until the next.
	XsdkTexture texture;
	bool has_texture;
	uint32_t texture_format;
	// The draw that last read the texture, which an upload waits for.
	uint32_t fence;
} Screen;

// The vertex shader's inputs, by location: 0 the position, 2 the texture coordinates (xsdk's
// xenos-toolchain/docs/xsc.md, "Interface conventions").
static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = offsetof(Corner, u) },
};

// libretro's joypad buttons, by id, from the pad's: Cross is A's place, Circle B's, Square X's and Triangle Y's.
static const uint16_t joypad_buttons[] = {
	[RETRO_DEVICE_ID_JOYPAD_B] = INPUT_GAMEPAD_A,
	[RETRO_DEVICE_ID_JOYPAD_Y] = INPUT_GAMEPAD_X,
	[RETRO_DEVICE_ID_JOYPAD_SELECT] = INPUT_GAMEPAD_BACK,
	[RETRO_DEVICE_ID_JOYPAD_START] = INPUT_GAMEPAD_START,
	[RETRO_DEVICE_ID_JOYPAD_UP] = INPUT_GAMEPAD_DPAD_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN] = INPUT_GAMEPAD_DPAD_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT] = INPUT_GAMEPAD_DPAD_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT] = INPUT_GAMEPAD_DPAD_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A] = INPUT_GAMEPAD_B,
	[RETRO_DEVICE_ID_JOYPAD_X] = INPUT_GAMEPAD_Y,
	[RETRO_DEVICE_ID_JOYPAD_L] = INPUT_GAMEPAD_LEFT_SHOULDER,
	[RETRO_DEVICE_ID_JOYPAD_R] = INPUT_GAMEPAD_RIGHT_SHOULDER,
	[RETRO_DEVICE_ID_JOYPAD_L3] = INPUT_GAMEPAD_LEFT_THUMB,
	[RETRO_DEVICE_ID_JOYPAD_R3] = INPUT_GAMEPAD_RIGHT_THUMB,
};

static Config config;
static Screen screen;
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static uint32_t frame_number;
// Whether the core's frames have started.
static bool frames_running;
// Copying the core's frames into the texture, inside the core's frames, over a report (common/xenon_profile.h).
static uint32_t upload_ticks;
// The joypad's buttons as a bitmask of libretro's ids, read once a frame.
static int16_t joypad;
// When both sticks were first pressed in together, 0 while they aren't; and whether they've been held long enough.
static uint64_t pause_held_since;
static bool pause_requested;
// After a menu, the game sees no buttons until every one is let go, so the A that resumed isn't the game's Cross.
static bool held_since_menu;
// check=reload: whether the core's last frame had a pixel that isn't black.
static bool frame_lit;
// The core's sound, stereo 16-bit frames at its rate, from the core's thread to the feeder, which resamples it to the
// console's 48 kHz: a ring with a writer each side, counting the frames put in and taken out.
static int16_t audio_ring[AUDIO_RING_FRAMES][2];
static uint32_t audio_head;
static uint32_t audio_tail;
// Frames the core gave while the ring was full, since the game started.
static uint32_t audio_dropped;
// Samples the feeder played silent as the ring ran short, since the game started: gaps in the sound. The feeder
// writes it.
static uint32_t audio_starved;
// The frames a vblank showed the last frame again at, and the frames a gap in the sound started at, for the line a game
// ends with: kept here, as a log line during the game is a write to the drive it streams from, which can hold a frame
// up.
typedef struct {
	uint32_t frames[MARKS];
	uint32_t count;
} Marks;
static Marks missed_marks;
static Marks gap_marks;
// The counts at the last report, which the next one gives the growth of.
static uint32_t audio_head_reported;
static uint32_t audio_tail_reported;
static uint32_t audio_dropped_reported;
static uint32_t audio_starved_reported;
static double audio_rate_hz;
static HANDLE audio_thread;

static uint64_t now_us(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

// Drops the line's end, "\n" or "\r\n".
static void trim_line(char *line)
{
	size_t length_bytes = strlen(line);
	while (length_bytes > 0 && (line[length_bytes - 1] == '\n' || line[length_bytes - 1] == '\r'))
		line[--length_bytes] = '\0';
}

static void read_config(void)
{
	snprintf(config.cpu, sizeof config.cpu, "Recompiler");
	FILE *const file = fopen(CONFIG_NAME, "r");
	if (!file)
		return;
	char line[LINE_BYTES];
	while (fgets(line, sizeof line, file)) {
		trim_line(line);
		char *const value = strchr(line, '=');
		if (!value)
			continue;
		*value = '\0';
		if (!strcmp(line, "game")) {
			snprintf(config.game, sizeof config.game, "%s", value + 1);
		} else if (!strcmp(line, "cpu")) {
			snprintf(config.cpu, sizeof config.cpu, "%s", value + 1);
		} else if (!strcmp(line, "frames")) {
			config.frames = (uint32_t)strtoul(value + 1, NULL, 10);
		} else if (!strcmp(line, "capture")) {
			for (char *next = value + 1; *next && config.capture_count < MAX_CAPTURES; next++) {
				config.captures[config.capture_count++] = (uint32_t)strtoul(next, &next, 10);
				if (*next != ',')
					break;
			}
		} else if (!strcmp(line, "report")) {
			config.report = strtoul(value + 1, NULL, 10) != 0;
		} else if (!strcmp(line, "check")) {
			snprintf(config.check, sizeof config.check, "%s", value + 1);
		}
	}
	fclose(file);
}

static void core_log(enum retro_log_level level, const char *format, ...)
{
	// Once frames run, only warnings and errors: each line is a write to the drive the game streams from, and the
	// memory card's info line held a frame up 43 ms.
	if (level < (frames_running ? RETRO_LOG_WARN : RETRO_LOG_INFO))
		return;
	char line[LINE_BYTES];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	trim_line(line);
	xsdk_log_write("core: %s", line);
}

// The core's options this host sets; the rest keep the core's defaults.
static const char *core_option(const char *key)
{
	if (!strcmp(key, "swanstation_GPU_Renderer"))
		return "Software";
	if (!strcmp(key, "swanstation_CPU_ExecutionMode"))
		return config.cpu;
	if (!strcmp(key, "swanstation_CPU_FastmemMode"))
		return "Disabled";
	if (!strcmp(key, "swanstation_GPU_UseThread") || !strcmp(key, "swanstation_CDROM_ReadThread"))
		return "true";
	return NULL;
}

static bool environment(unsigned command, void *data)
{
	switch (command) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = DATA_DIRECTORY;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		const enum retro_pixel_format format = *(const enum retro_pixel_format *)data;
		if (format != RETRO_PIXEL_FORMAT_RGB565 && format != RETRO_PIXEL_FORMAT_XRGB8888)
			return false;
		pixel_format = format;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *const variable = data;
		variable->value = core_option(variable->key);
		return variable->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		return true;
	default:
		return false;
	}
}

// Writes the frame as a binary PPM beside the .xex.
static void capture(const void *data, unsigned width, unsigned height, size_t pitch)
{
	char name[64];
	snprintf(name, sizeof name, "capture_%u.ppm", (unsigned)frame_number);
	FILE *const file = fopen(name, "wb");
	if (!file) {
		xsdk_log_write("swanstation: can't write %s", name);
		return;
	}
	fprintf(file, "P6\n%u %u\n255\n", width, height);
	for (unsigned y = 0; y < height; y++) {
		const uint8_t *const row = (const uint8_t *)data + y * pitch;
		for (unsigned x = 0; x < width; x++) {
			uint8_t rgb[3];
			if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t pixel;
				memcpy(&pixel, row + x * 4, sizeof pixel);
				rgb[0] = (uint8_t)(pixel >> 16);
				rgb[1] = (uint8_t)(pixel >> 8);
				rgb[2] = (uint8_t)pixel;
			} else {
				uint16_t pixel;
				memcpy(&pixel, row + x * 2, sizeof pixel);
				rgb[0] = (uint8_t)((pixel >> 11) * 255 / 31);
				rgb[1] = (uint8_t)((pixel >> 5 & 0x3F) * 255 / 63);
				rgb[2] = (uint8_t)((pixel & 0x1F) * 255 / 31);
			}
			fwrite(rgb, 1, sizeof rgb, file);
		}
	}
	fclose(file);
	xsdk_log_write("swanstation: captured frame %u, %ux%u, as %s", (unsigned)frame_number, width, height, name);
}

static void copy_frame(const void *data, unsigned width, unsigned height, size_t pitch);

// Copies the core's frame into the texture, timed for the report.
static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data)
		return;
	const uint32_t start = xenon_profile_now();
	copy_frame(data, width, height, pitch);
	upload_ticks += xenon_profile_now() - start;
	if (!strcmp(config.check, "reload")) {
		const unsigned pixel_bytes = pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
		frame_lit = false;
		for (unsigned y = 0; y < height && !frame_lit; y++) {
			const uint8_t *const row = (const uint8_t *)data + y * pitch;
			// XRGB8888's X, the first byte of each big-endian word, doesn't count.
			for (unsigned x = 0; x < width * pixel_bytes && !frame_lit; x++)
				frame_lit = row[x] && (pixel_bytes == 2 || x % 4 != 0);
		}
	}
}

// Copies the core's frame into the texture, made again whenever the frame's size or format changes.
static void copy_frame(const void *data, unsigned width, unsigned height, size_t pitch)
{
	for (uint32_t i = 0; i < config.capture_count; i++) {
		if (config.captures[i] == frame_number)
			capture(data, width, height, pitch);
	}
	const uint32_t format = pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? XSDK_FORMAT_A8R8G8B8 : XSDK_FORMAT_R5G6B5;
	const NTSTATUS waited = xsdk_gpu_wait(screen.fence, FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(waited))
		xsdk_log_write("swanstation: xsdk_gpu_wait failed: 0x%08X", (unsigned)waited);
	if (!screen.has_texture || screen.texture.width != width || screen.texture.height != height ||
		screen.texture_format != format) {
		if (screen.has_texture)
			xsdk_texture_free(&screen.texture);
		const NTSTATUS status = xsdk_texture_create(&screen.texture, format, width, height, 1, false);
		screen.has_texture = NT_SUCCESS(status);
		if (!screen.has_texture) {
			xsdk_log_write("swanstation: xsdk_texture_create(%ux%u) failed: 0x%08X", width, height, (unsigned)status);
			return;
		}
		screen.texture_format = format;
		xsdk_log_write("swanstation: the core's frames are %ux%u, %s", width, height,
			format == XSDK_FORMAT_A8R8G8B8 ? "XRGB8888" : "RGB565");
	}
	xsdk_texture_upload(&screen.texture, 0, data, (uint32_t)pitch);
	xsdk_gpu_invalidate(screen.texture.address, screen.texture.size_bytes);
}

// Puts the core's frames in the ring for the feeder; frames past the ring's room are dropped, and counted.
static size_t audio_sample_batch(const int16_t *data, size_t frames)
{
	const uint32_t tail = __atomic_load_n(&audio_tail, __ATOMIC_ACQUIRE);
	uint32_t head = audio_head;
	size_t i = 0;
	for (; i < frames && head - tail < AUDIO_RING_FRAMES; i++, head++) {
		audio_ring[head % AUDIO_RING_FRAMES][0] = data[2 * i];
		audio_ring[head % AUDIO_RING_FRAMES][1] = data[2 * i + 1];
	}
	audio_dropped += (uint32_t)(frames - i);
	__atomic_store_n(&audio_head, head, __ATOMIC_RELEASE);
	return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
	const int16_t frame[2] = { left, right };
	audio_sample_batch(frame, 1);
}

// The feeder: fills each frame the render driver frees from the ring, each sample between the core's two nearest, and
// plays silence from the ring running dry until it fills again. Its step through the core's samples leans up to
// AUDIO_RATE_LEAN with the ring's fill, holding it near AUDIO_TARGET_FRAMES: the display's 60 Hz paces the core a
// little off its own rate.
static uint32_t audio_feed(void *context)
{
	(void)context;
	const double base_step = audio_rate_hz / XSDK_AUDIO_RATE_HZ;
	double position = 0.0;
	// Off at the start and after the ring runs dry, until it holds AUDIO_TARGET_FRAMES again: one clean gap, not a
	// crackle of short ones while the ring hovers near empty.
	bool playing = false;
	// Starting fills the driver's queue at once, so it starts with the queue's worth in the ring beyond the target.
	const uint32_t start_frames =
		AUDIO_TARGET_FRAMES + (uint32_t)(XSDK_AUDIO_FRAMES_AHEAD_DEFAULT * XSDK_AUDIO_FRAME_SAMPLES * base_step);
	for (;;) {
		float *frame;
		const NTSTATUS status = xsdk_audio_next_frame(AUDIO_WAIT_MS, &frame);
		if (status == STATUS_IO_TIMEOUT)
			continue;
		if (!NT_SUCCESS(status))
			return 0;
		const uint32_t head = __atomic_load_n(&audio_head, __ATOMIC_ACQUIRE);
		uint32_t tail = audio_tail;
		playing = playing || head - tail >= start_frames;
		double fill = ((double)(head - tail) - AUDIO_TARGET_FRAMES) / AUDIO_TARGET_FRAMES;
		fill = fill > 1.0 ? 1.0 : fill < -1.0 ? -1.0 : fill;
		const double step = base_step * (1.0 + AUDIO_RATE_LEAN * fill);
		float *const left = frame + XSDK_AUDIO_FRONT_LEFT * XSDK_AUDIO_FRAME_SAMPLES;
		float *const right = frame + XSDK_AUDIO_FRONT_RIGHT * XSDK_AUDIO_FRAME_SAMPLES;
		for (uint32_t i = 0; i < XSDK_AUDIO_FRAME_SAMPLES; i++) {
			if (!playing || head - tail < 2) {
				playing = false;
				left[i] = 0.0f;
				right[i] = 0.0f;
				__atomic_fetch_add(&audio_starved, 1, __ATOMIC_RELAXED);
				continue;
			}
			const int16_t *const a = audio_ring[tail % AUDIO_RING_FRAMES];
			const int16_t *const b = audio_ring[(tail + 1) % AUDIO_RING_FRAMES];
			const float t = (float)position;
			left[i] = (a[0] + (b[0] - a[0]) * t) * (1.0f / 32768.0f);
			right[i] = (a[1] + (b[1] - a[1]) * t) * (1.0f / 32768.0f);
			position += step;
			const uint32_t whole = (uint32_t)position;
			position -= whole;
			tail += whole;
		}
		// The rest of the six channels silent, as slots are reused (xsdk's docs/GUIDE.md, "Audio").
		memset(frame + XSDK_AUDIO_CENTER * XSDK_AUDIO_FRAME_SAMPLES, 0,
			(XSDK_AUDIO_CHANNELS - XSDK_AUDIO_CENTER) * XSDK_AUDIO_FRAME_SAMPLES * sizeof(float));
		__atomic_store_n(&audio_tail, tail, __ATOMIC_RELEASE);
		xsdk_audio_queue_frame();
	}
}

// Opens the render driver and starts the feeder on its own hardware thread.
static bool open_audio(double rate_hz)
{
	audio_rate_hz = rate_hz;
	// The last game's sound left in the ring isn't this one's.
	audio_head = 0;
	audio_tail = 0;
	audio_head_reported = 0;
	audio_tail_reported = 0;
	audio_dropped = 0;
	audio_starved = 0;
	audio_dropped_reported = 0;
	audio_starved_reported = 0;
	NTSTATUS status = xsdk_audio_open(0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("swanstation: xsdk_audio_open failed: 0x%08X", (unsigned)status);
		return false;
	}
	status = xsdk_thread_create_on(&audio_thread, AUDIO_STACK_BYTES, audio_feed, NULL, AUDIO_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("swanstation: xsdk_thread_create_on failed for the feeder: 0x%08X", (unsigned)status);
		xsdk_audio_close();
		return false;
	}
	return true;
}

// Closes the render driver, which returns the feeder from its wait, and waits for it to end.
static void close_audio(void)
{
	xsdk_audio_close();
	const NTSTATUS status = NtWaitForSingleObjectEx(audio_thread, USER_MODE, 0, NULL);
	if (!NT_SUCCESS(status))
		xsdk_log_write("swanstation: waiting for the feeder failed: 0x%08X", (unsigned)status);
	NtClose(audio_thread);
}

// The render driver's counts, the ring's fill and the frames dropped over the report.
static void log_audio(void)
{
	XsdkAudioStats stats;
	xsdk_audio_stats(&stats);
	const uint32_t head = __atomic_load_n(&audio_head, __ATOMIC_ACQUIRE);
	const uint32_t tail = __atomic_load_n(&audio_tail, __ATOMIC_ACQUIRE);
	const uint32_t starved = __atomic_load_n(&audio_starved, __ATOMIC_RELAXED);
	xsdk_log_write(
		"swanstation: sound: %u frames played, %u silent (the feeder behind), at least %u queued; %u of the "
		"core's samples waiting, %u dropped, %u played silent as they ran short; the core gave %u, the feeder took %u",
		(unsigned)stats.played, (unsigned)stats.silent, (unsigned)stats.fewest_queued, (unsigned)(head - tail),
		(unsigned)(audio_dropped - audio_dropped_reported), (unsigned)(starved - audio_starved_reported),
		(unsigned)(head - audio_head_reported), (unsigned)(tail - audio_tail_reported));
	audio_head_reported = head;
	audio_tail_reported = tail;
	audio_dropped_reported = audio_dropped;
	audio_starved_reported = starved;
}

// Reads the pad for the core. Both sticks pressed in together reach it as neither, and held a second they ask for the
// pause menu. Not Back and Start: Aurora's plugin takes a screenshot on those.
static void input_poll(void)
{
	INPUT_STATE input = { 0 };
	joypad = 0;
	if (XamInputGetState(0, 0, &input) != ERROR_SUCCESS)
		return;
	uint16_t buttons = input.gamepad.buttons;
	if (held_since_menu) {
		if (buttons || input.gamepad.left_trigger >= TRIGGER_PRESSED || input.gamepad.right_trigger >= TRIGGER_PRESSED)
			return;
		held_since_menu = false;
	}
	const uint16_t combination = INPUT_GAMEPAD_LEFT_THUMB | INPUT_GAMEPAD_RIGHT_THUMB;
	if ((buttons & combination) == combination) {
		const uint64_t now = now_us();
		if (!pause_held_since)
			pause_held_since = now;
		pause_requested = now - pause_held_since >= PAUSE_HOLD_US;
		buttons &= (uint16_t)~combination;
	} else {
		pause_held_since = 0;
	}
	for (unsigned id = 0; id < sizeof joypad_buttons / sizeof joypad_buttons[0]; id++) {
		if (joypad_buttons[id] && buttons & joypad_buttons[id])
			joypad |= (int16_t)(1 << id);
	}
	if (input.gamepad.left_trigger >= TRIGGER_PRESSED)
		joypad |= 1 << RETRO_DEVICE_ID_JOYPAD_L2;
	if (input.gamepad.right_trigger >= TRIGGER_PRESSED)
		joypad |= 1 << RETRO_DEVICE_ID_JOYPAD_R2;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return joypad;
	return id < 16 ? joypad >> id & 1 : 0;
}

// Writes words into GPU memory with volatile 32-bit stores (xsdk's docs/PITFALLS.md), then drops the GPU's cached copy.
static void write_gpu(volatile uint32_t *memory, const void *data, uint32_t size_bytes)
{
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		uint32_t word;
		memcpy(&word, (const uint8_t *)data + i * 4, 4);
		memory[i] = word;
	}
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
}

static bool make_screen(void)
{
	NTSTATUS status = xsdk_shader_load(&screen.vertex, frame_vert.container, frame_vert.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&screen.pixel, frame_frag.container, frame_frag.container_bytes);
	if (NT_SUCCESS(status)) {
		const uint32_t strides[] = { sizeof(Corner) };
		status = xsdk_shader_set_vertex_layout(&screen.vertex, layout, sizeof layout / sizeof layout[0], strides);
	}
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("swanstation: loading the shaders failed: 0x%08X", (unsigned)status);
		return false;
	}
	// A strip of the quad's corners, top left first.
	const Corner corners[CORNERS] = {
		{ -FRAME_HALF_WIDTH, 1.0f, 0.0f, 0.0f },
		{ FRAME_HALF_WIDTH, 1.0f, 1.0f, 0.0f },
		{ -FRAME_HALF_WIDTH, -1.0f, 0.0f, 1.0f },
		{ FRAME_HALF_WIDTH, -1.0f, 1.0f, 1.0f },
	};
	screen.vertices = xsdk_gpu_alloc(VERTICES_BYTES);
	if (!screen.vertices) {
		xsdk_log_write("swanstation: xsdk_gpu_alloc failed");
		return false;
	}
	write_gpu(screen.vertices, corners, VERTICES_BYTES);
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&screen.sampler, &sampler);
	return true;
}

static void free_screen(void)
{
	if (screen.has_texture)
		xsdk_texture_free(&screen.texture);
	xsdk_gpu_free(screen.vertices);
	xsdk_shader_free(&screen.pixel);
	xsdk_shader_free(&screen.vertex);
}

// Draws the core's last frame into the target set.
static void draw_frame(void)
{
	if (screen.has_texture) {
		xsdk_gpu_set_shaders(&screen.vertex, &screen.pixel);
		xsdk_built_shader_set_literals(&frame_vert);
		xsdk_built_shader_set_literals(&frame_frag);
		xsdk_gpu_set_texture(FRAME_FRAG_SAMPLER_0_0, &screen.texture, &screen.sampler);
		xsdk_gpu_set_vertices(0, xsdk_gpu_address(screen.vertices), VERTICES_BYTES);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_STRIP, CORNERS);
	}
}

static void draw(uint32_t address, uint32_t width, uint32_t height)
{
	xsdk_gpu_set_target(width, height);
	draw_frame();
	xsdk_gpu_resolve(address, width, height, BLACK);
	screen.fence = xsdk_gpu_fence();
}

// The ticks a frame, over a report's frames, in ms.
static double frame_ms(uint32_t ticks)
{
	return ticks / (TIME_BASE_HZ / 1000.0) / REPORT_FRAMES;
}

// Where the core's time went over the report: each timed event, the CPU thread's waits for the GPU thread, the copy of
// the frames, and what's left, the CPU itself; and the GPU thread's work beside them. The GPU sync before a frame is
// shown runs inside the GPU's CRTC event, and is in its time too. Clears the counts for the next report.
static void log_profile(uint64_t core_us)
{
	char events[LINE_BYTES] = "";
	size_t used = 0;
	uint32_t event_ticks = 0;
	for (uint32_t i = 0; i < xenon_profile.event_count && used < sizeof events; i++) {
		used += (size_t)snprintf(events + used, sizeof events - used, "%s%s %.2f", i ? ", " : "",
			xenon_profile.event_names[i], frame_ms(xenon_profile.event_ticks[i]));
		event_ticks += xenon_profile.event_ticks[i];
		xenon_profile.event_ticks[i] = 0;
	}
	const double core_ms = core_us / 1000.0 / REPORT_FRAMES;
	// Samples the SPU makes in its own "SPU Sample" event would count twice, there and here; the log shows when it
	// runs.
	const double cpu_ms = core_ms - frame_ms(event_ticks) - frame_ms(xenon_profile.gpu_space_ticks) -
		frame_ms(upload_ticks) - frame_ms(xenon_profile.spu_ticks);
	xsdk_log_write(
		"swanstation: ms a frame: events %.2f (%s); waiting for the GPU thread %.2f to sync and %.2f for room; "
		"the SPU %.2f; copying the frame %.2f; the CPU the rest, %.2f; the GPU thread busy %.2f",
		frame_ms(event_ticks), events, frame_ms(xenon_profile.gpu_sync_ticks), frame_ms(xenon_profile.gpu_space_ticks),
		frame_ms(xenon_profile.spu_ticks), frame_ms(upload_ticks), cpu_ms, frame_ms(xenon_profile.gpu_thread_ticks));
	xenon_profile.gpu_thread_ticks = 0;
	xenon_profile.gpu_sync_ticks = 0;
	xenon_profile.gpu_space_ticks = 0;
	xenon_profile.spu_ticks = 0;
	upload_ticks = 0;
	// The MDEC's parts run inside its block copy-out event, and are in that event's time.
	if (xenon_profile.mdec_macroblocks) {
		xsdk_log_write(
			"swanstation: the MDEC: %u macroblocks; ms a frame unpacking %.2f, the IDCT %.2f, YUV to RGB %.2f, "
			"packing %.2f",
			(unsigned)xenon_profile.mdec_macroblocks, frame_ms(xenon_profile.mdec_rle_ticks),
			frame_ms(xenon_profile.mdec_idct_ticks), frame_ms(xenon_profile.mdec_yuv_ticks),
			frame_ms(xenon_profile.mdec_pack_ticks));
	}
	xenon_profile.mdec_macroblocks = 0;
	xenon_profile.mdec_rle_ticks = 0;
	xenon_profile.mdec_idct_ticks = 0;
	xenon_profile.mdec_yuv_ticks = 0;
	xenon_profile.mdec_pack_ticks = 0;
}

// The counts a slow frame's log tells apart, as they stood before it.
typedef struct {
	uint32_t events;
	uint32_t event_ticks[XENON_PROFILE_EVENTS];
	uint32_t gpu_sync;
	uint32_t spu;
	uint32_t cdrom_wait;
} Counts;

static Counts counts_now(void)
{
	Counts counts = { .gpu_sync = xenon_profile.gpu_sync_ticks,
		.spu = xenon_profile.spu_ticks,
		.cdrom_wait = xenon_profile.cdrom_wait_ticks };
	for (uint32_t i = 0; i < xenon_profile.event_count; i++) {
		counts.event_ticks[i] = xenon_profile.event_ticks[i];
		counts.events += xenon_profile.event_ticks[i];
	}
	return counts;
}

// A frame's ms from a count's two readings, unsigned so a count's wrap doesn't matter.
static double ticks_ms(uint32_t before, uint32_t after)
{
	return (after - before) / (TIME_BASE_HZ / 1000.0);
}

// Logs a frame the core took over SLOW_FRAME_US for: what it waited on, and the rest.
static void log_slow_frame(uint64_t took_us, const Counts *before)
{
	const Counts after = counts_now();
	// The event that took longest, by its count's growth; a count new this frame grew from 0.
	uint32_t longest = 0;
	for (uint32_t i = 1; i < xenon_profile.event_count; i++) {
		if (after.event_ticks[i] - before->event_ticks[i] > after.event_ticks[longest] - before->event_ticks[longest])
			longest = i;
	}
	xsdk_log_write(
		"swanstation: frame %u took %.2f ms: waiting for the CD-ROM read thread %.2f, events %.2f (the longest %s, "
		"%.2f; the GPU sync in them %.2f), the SPU %.2f",
		(unsigned)frame_number, took_us / 1000.0, ticks_ms(before->cdrom_wait, after.cdrom_wait),
		ticks_ms(before->events, after.events), xenon_profile.event_count ? xenon_profile.event_names[longest] : "none",
		ticks_ms(before->event_ticks[longest], after.event_ticks[longest]), ticks_ms(before->gpu_sync, after.gpu_sync),
		ticks_ms(before->spu, after.spu));
}

// Runs the core a frame at a time, each shown as the display's next, until frames=, the title is left, or the pause
// menu goes back to the games; logs the core's time a frame over each REPORT_FRAMES, and each slow frame.
static void mark(Marks *marks, uint32_t frame)
{
	if (marks->count < MARKS)
		marks->frames[marks->count] = frame;
	marks->count++;
}

// The marks' frames as "1088, 2100", with how many more past MARKS, or "none".
static void format_marks(const Marks *marks, char *text, size_t text_bytes)
{
	snprintf(text, text_bytes, "none");
	size_t used = 0;
	for (uint32_t i = 0; i < marks->count && i < MARKS && used < text_bytes; i++)
		used += (size_t)snprintf(text + used, text_bytes - used, "%s%u", i ? ", " : "", (unsigned)marks->frames[i]);
	if (marks->count > MARKS && used < text_bytes)
		snprintf(text + used, text_bytes - used, " and %u more", (unsigned)(marks->count - MARKS));
}

static RunEnd run(uint32_t width, uint32_t height, double core_fps, const char *game_path)
{
	uint64_t period_start = now_us();
	uint64_t core_us = 0;
	uint64_t longest_core_us = 0;
	XsdkPresentStats present;
	xsdk_present_stats(&present);
	uint32_t missed_before = present.missed;
	uint32_t starved_seen = 0;
	bool in_gap = false;
	missed_marks.count = 0;
	gap_marks.count = 0;
	frames_running = true;
	for (frame_number = 0; config.frames == 0 || frame_number < config.frames; frame_number++) {
		if (picker_shots && frame_number == SHOT_FRAME)
			pause_requested = true;
		if (pause_requested) {
			const PauseChoice choice = picker_pause(width, height, game_path, draw_frame);
			// The menu's draws read the frame's texture too, and the next upload waits for them.
			screen.fence = xsdk_gpu_fence();
			pause_requested = false;
			pause_held_since = 0;
			held_since_menu = true;
			if (choice == PAUSE_TO_GAMES)
				return RUN_TO_GAMES;
			if (choice == PAUSE_RESET)
				retro_reset();
		}
		const Counts before = counts_now();
		const uint64_t start = now_us();
		retro_run();
		const uint64_t took = now_us() - start;
		if (config.report && took > SLOW_FRAME_US)
			log_slow_frame(took, &before);
		core_us += took;
		if (took > longest_core_us)
			longest_core_us = took;
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("swanstation: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return RUN_ENDED;
		}
		draw(address, width, height);
		xsdk_present_end();
		// Each vblank that showed the last frame again, and each gap in the sound, by the frame it came at.
		const uint32_t missed_seen = present.missed;
		xsdk_present_stats(&present);
		if (present.missed != missed_seen) {
			mark(&missed_marks, frame_number);
			if (config.report) {
				xsdk_log_write("swanstation: frame %u: %u vblanks showed the last frame again; the core took %.2f ms",
					(unsigned)frame_number, (unsigned)(present.missed - missed_seen), took / 1000.0);
			}
		}
		const uint32_t starved = __atomic_load_n(&audio_starved, __ATOMIC_RELAXED);
		if (starved != starved_seen && !in_gap)
			mark(&gap_marks, frame_number);
		in_gap = starved != starved_seen;
		starved_seen = starved;
		if (!config.report)
			continue;
		if ((frame_number + 1) % REPORT_FRAMES == 0) {
			const uint64_t period_us = now_us() - period_start;
			xsdk_log_write(
				"swanstation: frames %u-%u: the core took %.2f ms a frame on average, %.2f at most, of the "
				"%.2f its %.2f fps allow; %.2f frames a second shown, %u vblanks with no new frame",
				(unsigned)(frame_number + 1 - REPORT_FRAMES), (unsigned)frame_number, core_us / 1000.0 / REPORT_FRAMES,
				longest_core_us / 1000.0, 1000.0 / core_fps, core_fps, REPORT_FRAMES * 1e6 / period_us,
				(unsigned)(present.missed - missed_before));
			missed_before = present.missed;
			log_profile(core_us);
			log_audio();
			period_start = now_us();
			core_us = 0;
			longest_core_us = 0;
		}
	}
	return RUN_ENDED;
}

// Starts the game at path and runs it until it ends, how it ended in *end; false if its disc image doesn't open or the
// core couldn't load it.
static bool play_game(uint32_t width, uint32_t height, const char *path, RunEnd *end)
{
	xsdk_log_write("swanstation: starting %s", path);
	// The core boots the BIOS with no disc in when the image doesn't open (core/system.cpp, System::Boot).
	if (!disc_opens(path)) {
		xsdk_log_write("swanstation: %s doesn't open", path);
		return false;
	}
	const struct retro_game_info game = { .path = path };
	if (!retro_load_game(&game)) {
		xsdk_log_write("swanstation: the core couldn't load %s", path);
		return false;
	}
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	xsdk_log_write("swanstation: loaded; %.2f fps, sound at %.0f Hz", av.timing.fps, av.timing.sample_rate);
	const bool sound = open_audio(av.timing.sample_rate);
	held_since_menu = true;
	XsdkPresentStats before;
	xsdk_present_stats(&before);
	*end = run(width, height, av.timing.fps, path);
	XsdkPresentStats after;
	xsdk_present_stats(&after);
	char missed_frames[LINE_BYTES];
	char gap_frames[LINE_BYTES];
	format_marks(&missed_marks, missed_frames, sizeof missed_frames);
	format_marks(&gap_marks, gap_frames, sizeof gap_frames);
	xsdk_log_write(
		"swanstation: %u frames run; %u vblanks showed the last frame again, at frames %s; %u samples played silent "
		"as the sound ran short, from frames %s; %u of the core's dropped",
		(unsigned)frame_number, (unsigned)(after.missed - before.missed), missed_frames,
		(unsigned)__atomic_load_n(&audio_starved, __ATOMIC_RELAXED), gap_frames, (unsigned)audio_dropped);
	if (sound)
		close_audio();
	retro_unload_game();
	// The counts name events by the unloaded game's strings, freed with it; the next game's are its own.
	memset(&xenon_profile, 0, sizeof xenon_profile);
	return true;
}

// Plays game= if host.txt names one, then what the picker starts, until frames= ends a game, the display fails, or the
// picker is left.
static void play(uint32_t width, uint32_t height)
{
	char path[LINE_BYTES];
	bool start_failed = false;
	bool from_config = config.game[0] != '\0';
	for (;;) {
		if (from_config)
			snprintf(path, sizeof path, "%s", config.game);
		else if (!picker_choose(width, height, start_failed ? path : NULL, path, sizeof path))
			return;
		from_config = false;
		RunEnd end = RUN_TO_GAMES;
		start_failed = !play_game(width, height, path, &end);
		if (end == RUN_ENDED)
			return;
	}
}

// check=reload: starts game= RELOAD_RUNS times over, each for frames= or RELOAD_FRAMES, and passes if every start ran
// and ended on a frame that isn't black, which a recompiler that didn't set itself up again wouldn't show.
static bool check_reload(uint32_t width, uint32_t height)
{
	if (!config.frames)
		config.frames = RELOAD_FRAMES;
	bool passed = true;
	for (uint32_t i = 0; i < RELOAD_RUNS; i++) {
		frame_lit = false;
		RunEnd end;
		const bool started = play_game(width, height, config.game, &end);
		const char *outcome = "failed";
		if (started)
			outcome = frame_lit ? "ended on a lit frame" : "ended on a black frame";
		xsdk_log_write("swanstation: reload check: start %u %s", (unsigned)(i + 1), outcome);
		passed = passed && started && frame_lit;
	}
	xsdk_log_write("swanstation: reload check: %s", passed ? "PASS" : "FAIL");
	return passed;
}

int main(void)
{
	read_config();
	xsdk_log_write("swanstation: %s, the %s CPU, on hardware thread %u", config.game[0] ? config.game : "the picker",
		config.cpu, (unsigned)xsdk_thread_hardware_thread());
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("swanstation: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("swanstation: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	int result = 1;
	uint32_t width;
	uint32_t height;
	if (make_screen()) {
		status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("swanstation: xsdk_present_open failed: 0x%08X", (unsigned)status);
		} else if (!picker_open()) {
			xsdk_present_close();
		} else {
			retro_set_environment(environment);
			retro_set_video_refresh(video_refresh);
			retro_set_audio_sample(audio_sample);
			retro_set_audio_sample_batch(audio_sample_batch);
			retro_set_input_poll(input_poll);
			retro_set_input_state(input_state);
			retro_init();
			picker_shots = !strcmp(config.check, "shots");
			if (!strcmp(config.check, "list")) {
				result = picker_check_list() ? 0 : 1;
			} else if (!strcmp(config.check, "reload")) {
				result = check_reload(width, height) ? 0 : 1;
			} else {
				play(width, height);
				result = 0;
			}
			retro_deinit();
			picker_close();
			xsdk_present_close();
		}
	}
	free_screen();
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("swanstation: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	xsdk_log_write("swanstation: done");
	return result;
}
