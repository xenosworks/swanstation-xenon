// The game picker and the pause menu, drawn with Dear ImGui (picker.cpp).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	PAUSE_RESUME,
	PAUSE_RESET,
	PAUSE_TO_GAMES,
} PauseChoice;

// host.txt's check=shots: the pause menu and then the picker each write their frame, as shot_pause.ppm and
// shot_picker.ppm beside the .xex, and close, the pause menu as Back to games and the picker as quitting.
extern bool picker_shots;

// After xsdk_present_open(); picker_close() before xsdk_present_close().
bool picker_open(void);
void picker_close(void);
// Shows the games on the hard drive and USB drives until one is started, its path written to path, true; false when
// the player quits. start_failed says the last game picked didn't start, which a line under the list then says.
bool picker_choose(uint32_t width, uint32_t height, bool start_failed, char *path, size_t path_bytes);
// Shows the pause menu over the game's last frame, which draw_frame draws into the target set, until a choice.
PauseChoice picker_pause(uint32_t width, uint32_t height, const char *game_path, void (*draw_frame)(void));
// host.txt's check=list: writes a fixture of files under game:\games_check, lists it as the picker would, and logs
// whether the list is the one expected.
bool picker_check_list(void);

#ifdef __cplusplus
}
#endif
