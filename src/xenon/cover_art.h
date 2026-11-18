#pragma once

#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace CoverArt
{
struct Settings {
	bool enabled = true;
	bool auto_download = true;
	bool download_on_startup = true;
	bool fallback_source = true;
};

// Wszystkie wywołania publiczne pochodzą z wątku interfejsu.
void Begin(const Settings &settings, const std::vector<std::string> &game_paths);
void Select(const std::string &serial);
std::string KnownSerial(const std::string &game_path);
void Prefetch(const std::vector<std::string> &game_paths);
void Update();
bool Draw(ImDrawList *draw, const ImVec2 &centre, float extent);
void AfterRender();
void RetryMissing();
void RequestStop();
bool TryStop();
// Kończy pracownika i czeka na GPU przed uruchomieniem emulacji.
bool Stop();
} // namespace CoverArt
