// swanstation: SwanStation on the Xbox 360, its libretro core linked in and driven from here (xsdk's
// docs/scope/62-swanstation.md). host.txt beside the .xex says what to run, one KEY=VALUE a line:
//   game=game:\Spyro the Dragon.cue   the disc image, from the title's folder or any other
//   cpu=CachedInterpreter             the core's CPU mode: Interpreter or CachedInterpreter; no recompiler yet
//   frames=1800                       ends the title after that many frames; without it, it runs until Xbox Home
//   capture=600,1200                  writes the core's frame at those frames as capture_<frame>.ppm beside the .xex
// The BIOS goes in the title's folder too, where the core looks for one by its contents. Frames are drawn 4:3 in the
// middle of a 1280x720 display, pads are the PlayStation's digital pad, and sound isn't played yet. Every 300 frames
// the log gives the time the core took for a frame, the scope's measure of full speed.

#include <libretro.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "swanstation_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

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
	// A trigger past a quarter of its travel presses L2 or R2.
	TRIGGER_PRESSED = 64,
	CORNERS = 4,
	BLACK = 0xFF000000,
};

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
} Config;

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
// The joypad's buttons as a bitmask of libretro's ids, read once a frame.
static int16_t joypad;

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

static bool read_config(void)
{
	snprintf(config.cpu, sizeof config.cpu, "CachedInterpreter");
	FILE *const file = fopen(CONFIG_NAME, "r");
	if (!file) {
		xsdk_log_write("swanstation: no %s beside the .xex", CONFIG_NAME);
		return false;
	}
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
		}
	}
	fclose(file);
	if (!config.game[0]) {
		xsdk_log_write("swanstation: %s names no game=", CONFIG_NAME);
		return false;
	}
	return true;
}

static void core_log(enum retro_log_level level, const char *format, ...)
{
	if (level < RETRO_LOG_INFO)
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

// Copies the core's frame into the texture, made again whenever the frame's size or format changes.
static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data)
		return;
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

static void audio_sample(int16_t left, int16_t right)
{
	(void)left;
	(void)right;
}

static size_t audio_sample_batch(const int16_t *data, size_t frames)
{
	(void)data;
	return frames;
}

static void input_poll(void)
{
	INPUT_STATE input = { 0 };
	joypad = 0;
	if (XamInputGetState(0, 0, &input) != ERROR_SUCCESS)
		return;
	for (unsigned id = 0; id < sizeof joypad_buttons / sizeof joypad_buttons[0]; id++) {
		if (joypad_buttons[id] && input.gamepad.buttons & joypad_buttons[id])
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

static void draw(uint32_t address, uint32_t width, uint32_t height)
{
	xsdk_gpu_set_target(width, height);
	if (screen.has_texture) {
		xsdk_gpu_set_shaders(&screen.vertex, &screen.pixel);
		xsdk_built_shader_set_literals(&frame_vert);
		xsdk_built_shader_set_literals(&frame_frag);
		xsdk_gpu_set_texture(FRAME_FRAG_SAMPLER_0_0, &screen.texture, &screen.sampler);
		xsdk_gpu_set_vertices(0, xsdk_gpu_address(screen.vertices), VERTICES_BYTES);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_STRIP, CORNERS);
	}
	xsdk_gpu_resolve(address, width, height, BLACK);
	screen.fence = xsdk_gpu_fence();
}

// Runs the core a frame at a time, each shown as the display's next, until frames= or the title is left; logs the
// core's time a frame over each REPORT_FRAMES.
static void run(uint32_t width, uint32_t height, double core_fps)
{
	uint64_t period_start = now_us();
	uint64_t core_us = 0;
	uint64_t longest_core_us = 0;
	for (frame_number = 0; config.frames == 0 || frame_number < config.frames; frame_number++) {
		const uint64_t start = now_us();
		retro_run();
		const uint64_t took = now_us() - start;
		core_us += took;
		if (took > longest_core_us)
			longest_core_us = took;
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("swanstation: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return;
		}
		draw(address, width, height);
		xsdk_present_end();
		if ((frame_number + 1) % REPORT_FRAMES == 0) {
			const uint64_t period_us = now_us() - period_start;
			xsdk_log_write(
				"swanstation: frames %u-%u: the core took %.2f ms a frame on average, %.2f at most, of the "
				"%.2f its %.2f fps allow; %.2f frames a second shown",
				(unsigned)(frame_number + 1 - REPORT_FRAMES), (unsigned)frame_number, core_us / 1000.0 / REPORT_FRAMES,
				longest_core_us / 1000.0, 1000.0 / core_fps, core_fps, REPORT_FRAMES * 1e6 / period_us);
			period_start = now_us();
			core_us = 0;
			longest_core_us = 0;
		}
	}
}

int main(void)
{
	if (!read_config())
		return 1;
	xsdk_log_write("swanstation: %s with the %s CPU, %s", config.game, config.cpu,
		config.frames ? "for a fixed count of frames" : "until Xbox Home");
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
		} else {
			retro_set_environment(environment);
			retro_set_video_refresh(video_refresh);
			retro_set_audio_sample(audio_sample);
			retro_set_audio_sample_batch(audio_sample_batch);
			retro_set_input_poll(input_poll);
			retro_set_input_state(input_state);
			retro_init();
			const struct retro_game_info game = { .path = config.game };
			if (retro_load_game(&game)) {
				struct retro_system_av_info av;
				retro_get_system_av_info(&av);
				xsdk_log_write("swanstation: loaded; %.2f fps, sound at %.0f Hz", av.timing.fps, av.timing.sample_rate);
				run(width, height, av.timing.fps);
				retro_unload_game();
				result = 0;
			} else {
				xsdk_log_write("swanstation: the core couldn't load %s", config.game);
			}
			retro_deinit();
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
