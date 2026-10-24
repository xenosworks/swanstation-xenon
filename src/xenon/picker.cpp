// The game picker and the pause menu (xsdk's docs/scope/62-swanstation.md, step 5): an open CD case, the selected
// game's disc on the left and the games as a numbered track listing on the right, black and white, the selected entry
// inverted. Drawn by hand on Dear ImGui's draw lists, not with its widgets; the controller comes through ImGui's keys.

#include "picker.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "common/cd_image.h"
#include "core/system.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_xsdk.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

// Archivo Narrow, regular and medium, OFL 1.1 (fonts/OFL.txt), built in by fonts.S.
extern "C" const unsigned char font_regular[], font_regular_end[], font_medium[], font_medium_end[];

namespace
{

enum : uint32_t {
	// The Guide's first opening held a frame up 2.7 s (xsdk's docs/HARDWARE.md, "The Guide, milestone 37").
	BEGIN_TIMEOUT_MS = 5000,
	BLACK_ARGB = 0xFF000000,
	LINE_BYTES = 512,
	// Frames the selection rests on a game before its disc is opened for the serial: scrolling past opens nothing.
	SERIAL_REST_FRAMES = 15,
	// The serial's thread: the third core's first, the CD-ROM read thread's while a game runs, and its stack, for
	// libchdr's reads.
	SERIAL_HARDWARE_THREAD = 4,
	SERIAL_STACK_BYTES = 256 * 1024,
	// check=shots: the frames each menu shows before it's written.
	SHOT_FRAMES = 4,
};

// The colours: the case's black, the print, the labels, the track numbers, the rules, and the disc's parts.
const ImU32 PAPER = IM_COL32(0x10, 0x10, 0x12, 0xFF);
const ImU32 INK = IM_COL32(0xE9, 0xE7, 0xE2, 0xFF);
const ImU32 FADED = IM_COL32(0x9C, 0x99, 0x8F, 0xFF);
const ImU32 NUMBER = IM_COL32(0x6F, 0x6C, 0x65, 0xFF);
const ImU32 RULE = IM_COL32(0x2F, 0x2F, 0x35, 0xFF);
const ImU32 DISC = IM_COL32(0x1C, 0x1C, 0x20, 0xFF);
const ImU32 DISC_EDGE = IM_COL32(0x3A, 0x3A, 0x40, 0xFF);
const ImU32 DISC_RING = IM_COL32(0x2B, 0x2B, 0x31, 0xFF);
const ImU32 DISC_HUB = IM_COL32(0x26, 0x26, 0x2B, 0xFF);
// The game's frame under the pause menu, 62% darker.
const ImU32 DIM = IM_COL32(0x00, 0x00, 0x00, 0x9E);

// The layout in pixels of a 720-line display, the mockup's at 1.88 times its size; every size scales with the
// display's height. The title-safe area is the middle 90%.
const float SAFE_X = 64.0f;
const float SAFE_Y = 36.0f;
const float COLUMN_GAP = 41.0f;
// The disc column's share against the list's: 1 to 1.15.
const float DISC_COLUMN_SHARE = 1.0f / 2.15f;
// The list's padding from its rule.
const float COLUMN_PAD = 41.0f;
const float DISC_RADIUS = 200.0f;
// The disc's ring, hub and hole, against its radius.
const float DISC_RING_SHARE = 0.83f;
const float DISC_HUB_SHARE = 0.41f;
const float DISC_HOLE_SHARE = 0.124f;
const float LINE_WIDTH = 1.5f;
// The label: its size, its smaller size for long names, how far above the disc's middle it sits, and the margin it
// keeps from the disc's edge; the serial: its size and how far below the middle.
const float LABEL_SIZE = 30.0f;
const float LABEL_SMALL_SIZE = 24.0f;
const float LABEL_RISE = 110.0f;
const float LABEL_MARGIN = 24.0f;
const float SERIAL_SIZE = 22.0f;
const float SERIAL_DROP = 125.0f;
// Letter spacing, against the size.
const float LABEL_TRACKING = 0.08f;
const float SERIAL_TRACKING = 0.10f;
const float HEADER_TRACKING = 0.14f;
const float HEADER_SIZE = 22.0f;
const float HEADER_GAP = 19.0f;
const float ROW_SIZE = 36.0f;
const float ROW = 58.0f;
const float SELECTED_SIZE = 45.0f;
const float SELECTED_ROW = 75.0f;
// How far the selected entry's bar reaches past the list each side.
const float SELECTED_BLEED = 15.0f;
const float NUMBER_GAP = 26.0f;
const float DISCS_SIZE = 26.0f;
const float MESSAGE_SIZE = 26.0f;
const float MESSAGE_GAP = 19.0f;
const float EMPTY_SIZE = 30.0f;
const float HINT_SIZE = 24.0f;
const float HINT_GAP = 26.0f;
// The pause menu's panel: its padding, its disc, the gap to the rule, its smallest text column, and its text.
const float PAUSE_PAD_X = 53.0f;
const float PAUSE_PAD_Y = 41.0f;
const float PAUSE_DISC_RADIUS = 105.0f;
const float PAUSE_GAP = 49.0f;
const float PAUSE_COLUMN = 357.0f;
const float PAUSE_NAME_SIZE = 38.0f;
const float PAUSE_NAME_TOP = 8.0f;
const float PAUSE_NAME_GAP = 19.0f;
const float PAUSE_ROW_SIZE = 34.0f;
const float PAUSE_ROW = 60.0f;
const float PAUSE_HINT_GAP = 23.0f;

const char ELLIPSIS[] = "\xE2\x80\xA6";
const char MIDDLE_DOT[] = "  \xC2\xB7  ";

// The disc image formats CDImage::Open takes (common/cd_image.cpp).
const char *const DISC_EXTENSIONS[] = { ".cue", ".chd", ".m3u", ".pbp", ".iso", ".ecm", ".mds", ".bin", ".img" };

const char *const PAUSE_ITEMS[] = { "Resume", "Reset", "Back to games" };

struct Game {
	// The file's name without its extension, as the list shows it.
	std::string name;
	std::string path;
	// The disc's serial, once read.
	std::string serial;
	// An .m3u's discs.
	int discs = 1;
	bool serial_read = false;
};

struct Source {
	const char *label;
	const char *root;
	std::vector<Game> games;
	bool present = false;
	size_t selected = 0;
	size_t first = 0;
	// The entries the list showed last frame, which a page moves by.
	size_t fit = 1;
};

Source sources[] = {
	{ "HARD DRIVE", "game:\\games", {} },
	{ "USB 1", "usb0:\\games", {} },
	{ "USB 2", "usb1:\\games", {} },
	{ "USB 3", "usb2:\\games", {} },
};

} // namespace

bool picker_shots;

namespace
{

ImFont *regular;
ImFont *medium;
size_t current;
// The game started last, which the list selects again on its return.
std::string last_path;

uint64_t now_us()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

// ASCII letters only: names on the console's drives are ASCII.
std::string lower(std::string text)
{
	for (char &c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

std::string upper(std::string text)
{
	for (char &c : text)
		c = (char)toupper((unsigned char)c);
	return text;
}

std::string base_name(const std::string &path)
{
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stem(const std::string &path)
{
	const std::string name = base_name(path);
	const size_t dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool is_disc(const std::string &name)
{
	const size_t dot = name.rfind('.');
	if (dot == std::string::npos)
		return false;
	const std::string extension = lower(name.substr(dot));
	return std::find(std::begin(DISC_EXTENSIONS), std::end(DISC_EXTENSIONS), extension) != std::end(DISC_EXTENSIONS);
}

// The file names a .cue's FILE lines or an .m3u's lines give, without their folders and in lower case, added to names;
// returns how many.
int read_names(const std::string &path, bool cue, std::vector<std::string> &names)
{
	FILE *const file = fopen(path.c_str(), "r");
	if (!file)
		return 0;
	int count = 0;
	char line[LINE_BYTES];
	while (fgets(line, sizeof line, file)) {
		char *start = line;
		while (isspace((unsigned char)*start))
			start++;
		char *end = start + strlen(start);
		while (end > start && isspace((unsigned char)end[-1]))
			end--;
		*end = '\0';
		if (cue) {
			if (strncasecmp(start, "FILE", 4) != 0 || !isspace((unsigned char)start[4]))
				continue;
			start += 5;
			while (isspace((unsigned char)*start))
				start++;
			if (*start == '"')
				end = strchr(++start, '"');
			else
				end = start + strcspn(start, " \t");
			if (!end)
				continue;
			*end = '\0';
		} else if (!*start || *start == '#') {
			continue;
		}
		names.push_back(lower(base_name(start)));
		count++;
	}
	fclose(file);
	return count;
}

// Adds a folder's games, and with subfolders those of the folders in it, one level down: every disc image but those a
// .cue or .m3u beside it names. Returns whether the folder opened.
bool list_folder(const std::string &folder, std::vector<Game> &games, bool subfolders)
{
	DIR *const dir = opendir(folder.c_str());
	if (!dir)
		return false;
	std::vector<std::string> files;
	std::vector<std::string> folders;
	while (const struct dirent *entry = readdir(dir)) {
		if (entry->d_name[0] != '.')
			(entry->d_type == DT_DIR ? folders : files).push_back(entry->d_name);
	}
	closedir(dir);
	std::vector<std::string> named;
	std::vector<Game> found;
	for (const std::string &file : files) {
		if (!is_disc(file))
			continue;
		const std::string path = folder + "\\" + file;
		const std::string extension = lower(file.substr(file.rfind('.')));
		Game game;
		game.name = stem(file);
		game.path = path;
		if (extension == ".cue")
			read_names(path, true, named);
		else if (extension == ".m3u")
			game.discs = read_names(path, false, named);
		found.push_back(std::move(game));
	}
	// shortcut: an .m3u naming discs in another folder doesn't hide them; fine for the usual layout, one folder a game.
	for (Game &game : found) {
		if (std::find(named.begin(), named.end(), lower(base_name(game.path))) == named.end())
			games.push_back(std::move(game));
	}
	if (subfolders) {
		for (const std::string &name : folders)
			list_folder(folder + "\\" + name, games, false);
	}
	return true;
}

bool list_games(const char *root, std::vector<Game> &games)
{
	games.clear();
	const bool opened = list_folder(root, games, true);
	std::sort(games.begin(), games.end(),
		[](const Game &a, const Game &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
	return opened;
}

// Lists every source again: the hard drive always, a USB drive while it has a games folder. Each keeps its selection,
// or takes the game started last when it has it.
void scan()
{
	for (Source &source : sources) {
		source.present = list_games(source.root, source.games) || &source == &sources[0];
		for (size_t i = 0; i < source.games.size(); i++) {
			if (source.games[i].path == last_path)
				source.selected = i;
		}
		if (source.selected >= source.games.size())
			source.selected = source.games.empty() ? 0 : source.games.size() - 1;
		if (!last_path.empty() && last_path.compare(0, strlen(source.root), source.root) == 0)
			current = (size_t)(&source - sources);
	}
	if (!sources[current].present)
		current = 0;
}

// The serial being read, a disc at a time on a thread of its own: opening Spyro's cue sheet and reading its serial took
// 136 ms on the console's hard drive once, eight frames the list would stand still for, and 10 ms another time.
struct SerialRead {
	std::string path;
	std::string serial;
	HANDLE thread = 0;
	bool done = false;
};
SerialRead serial_read;

uint32_t read_serial(void *context)
{
	(void)context;
	const uint64_t start = now_us();
	const std::unique_ptr<CDImage> image = CDImage::Open(serial_read.path.c_str(), CDImage::OpenFlags::None, nullptr);
	if (image)
		serial_read.serial = System::GetGameCodeForImage(image.get(), false);
	xsdk_log_write("picker: read %s's serial, \"%s\", in %.1f ms", serial_read.path.c_str(), serial_read.serial.c_str(),
		(now_us() - start) / 1000.0);
	__atomic_store_n(&serial_read.done, true, __ATOMIC_RELEASE);
	return 0;
}

void start_serial(Game &game)
{
	game.serial_read = true;
	serial_read.path = game.path;
	serial_read.serial.clear();
	serial_read.done = false;
	const NTSTATUS status =
		xsdk_thread_create_on(&serial_read.thread, SERIAL_STACK_BYTES, read_serial, nullptr, SERIAL_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("picker: xsdk_thread_create_on failed for the serial: 0x%08X", (unsigned)status);
		serial_read.thread = 0;
	}
}

// Gives a finished read's serial to its game; with wait, waits for a read still going first.
void finish_serial(bool wait)
{
	if (!serial_read.thread || (!wait && !__atomic_load_n(&serial_read.done, __ATOMIC_ACQUIRE)))
		return;
	const NTSTATUS status = NtWaitForSingleObjectEx(serial_read.thread, USER_MODE, 0, nullptr);
	if (!NT_SUCCESS(status))
		xsdk_log_write("picker: waiting for the serial's thread failed: 0x%08X", (unsigned)status);
	NtClose(serial_read.thread);
	serial_read.thread = 0;
	for (Source &source : sources) {
		for (Game &game : source.games) {
			if (game.path == serial_read.path)
				game.serial = serial_read.serial;
		}
	}
}

// Text's width at size, with tracking times the size between characters.
float text_width(ImFont *font, float size, const std::string &text, float tracking = 0.0f)
{
	if (tracking == 0.0f)
		return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
	float width = 0.0f;
	for (const char *c = text.c_str(); *c;) {
		int bytes = 1;
		while ((c[bytes] & 0xC0) == 0x80)
			bytes++;
		width += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + bytes).x + (c[bytes] ? tracking * size : 0.0f);
		c += bytes;
	}
	return width;
}

void draw_text(
	ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text, float tracking = 0.0f)
{
	if (tracking == 0.0f) {
		draw->AddText(font, size, at, colour, text.c_str());
		return;
	}
	for (const char *c = text.c_str(); *c;) {
		int bytes = 1;
		while ((c[bytes] & 0xC0) == 0x80)
			bytes++;
		draw->AddText(font, size, at, colour, c, c + bytes);
		at.x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + bytes).x + tracking * size;
		c += bytes;
	}
}

// Text cut short with an ellipsis to fit max_width.
std::string fitted(ImFont *font, float size, std::string text, float max_width, float tracking = 0.0f)
{
	if (text_width(font, size, text, tracking) <= max_width)
		return text;
	while (!text.empty()) {
		size_t end = text.size() - 1;
		while (end > 0 && (text[end] & 0xC0) == 0x80)
			end--;
		text.erase(end);
		while (!text.empty() && text.back() == ' ')
			text.pop_back();
		if (text_width(font, size, text + ELLIPSIS, tracking) <= max_width)
			break;
	}
	return text + ELLIPSIS;
}

// A line of the label centred across the disc, its middle rise above the disc's centre.
void draw_label_line(ImDrawList *draw, ImVec2 centre, float rise, float size, const std::string &line)
{
	const float width = text_width(regular, size, line, LABEL_TRACKING);
	draw_text(
		draw, regular, size, ImVec2(centre.x - width / 2.0f, centre.y - rise - size / 2.0f), INK, line, LABEL_TRACKING);
}

// The disc: black like a PlayStation disc's underside, with its ring, hub and hole.
void draw_disc(ImDrawList *draw, ImVec2 centre, float radius, float scale)
{
	draw->AddCircleFilled(centre, radius, DISC);
	draw->AddCircle(centre, radius, DISC_EDGE, 0, LINE_WIDTH * scale);
	draw->AddCircle(centre, radius * DISC_RING_SHARE, DISC_RING, 0, LINE_WIDTH * scale);
	draw->AddCircleFilled(centre, radius * DISC_HUB_SHARE, DISC_HUB);
	draw->AddCircleFilled(centre, radius * DISC_HOLE_SHARE, PAPER);
	draw->AddCircle(centre, radius * DISC_HOLE_SHARE, DISC_EDGE, 0, LINE_WIDTH * scale);
}

// The game's name across the disc's upper half, in capitals: smaller and then on two lines when it's long, cut short
// past that. Each line keeps inside the disc's edge at its outer side.
void draw_label(ImDrawList *draw, ImVec2 centre, float radius, float scale, const std::string &name)
{
	const std::string caps = upper(name);
	const auto room = [&](float outer) {
		return 2.0f * sqrtf(radius * radius - outer * outer) - 2.0f * LABEL_MARGIN * scale;
	};
	const float rise = LABEL_RISE * scale;
	for (const float size : { LABEL_SIZE * scale, LABEL_SMALL_SIZE * scale }) {
		if (text_width(regular, size, caps, LABEL_TRACKING) <= room(rise + size / 2.0f)) {
			draw_label_line(draw, centre, rise, size, caps);
			return;
		}
	}
	const float size = LABEL_SMALL_SIZE * scale;
	size_t split = std::string::npos;
	for (size_t i = caps.find(' '); i != std::string::npos; i = caps.find(' ', i + 1)) {
		if (split == std::string::npos || fabsf(i - caps.size() / 2.0f) < fabsf(split - caps.size() / 2.0f))
			split = i;
	}
	if (split == std::string::npos) {
		draw_label_line(
			draw, centre, rise, size, fitted(regular, size, caps, room(rise + size / 2.0f), LABEL_TRACKING));
		return;
	}
	const float upper_rise = rise + size * 0.55f;
	const float lower_rise = rise - size * 0.55f;
	draw_label_line(draw, centre, upper_rise, size,
		fitted(regular, size, caps.substr(0, split), room(upper_rise + size / 2.0f), LABEL_TRACKING));
	draw_label_line(draw, centre, lower_rise, size,
		fitted(regular, size, caps.substr(split + 1), room(lower_rise + size / 2.0f), LABEL_TRACKING));
}

void draw_picker(ImDrawList *draw, ImVec2 display, Source &source, size_t source_count, const std::string &message)
{
	const float scale = display.y / 720.0f;
	draw->AddRectFilled(ImVec2(0.0f, 0.0f), display, PAPER);

	// The disc, in the middle of the left column.
	const float content = display.x - 2.0f * SAFE_X * scale;
	const float disc_column = (content - COLUMN_GAP * scale) * DISC_COLUMN_SHARE;
	const ImVec2 centre(SAFE_X * scale + disc_column / 2.0f, display.y / 2.0f);
	const float radius = DISC_RADIUS * scale;
	draw_disc(draw, centre, radius, scale);
	const size_t count = source.games.size();
	if (count) {
		const Game &game = source.games[source.selected];
		draw_label(draw, centre, radius, scale, game.name);
		if (!game.serial.empty()) {
			const float size = SERIAL_SIZE * scale;
			const float width = text_width(regular, size, game.serial, SERIAL_TRACKING);
			draw_text(draw, regular, size,
				ImVec2(centre.x - width / 2.0f, centre.y + SERIAL_DROP * scale - size / 2.0f), FADED, game.serial,
				SERIAL_TRACKING);
		}
	}

	// The list's block, centred down the display inside the title-safe area: the header, the entries or what to do
	// with none, the message, and the hints.
	const float rule_x = SAFE_X * scale + disc_column + COLUMN_GAP * scale;
	const float x = rule_x + COLUMN_PAD * scale;
	const float right = display.x - SAFE_X * scale;
	const float width = right - x;
	const float header_height = (HEADER_SIZE + HEADER_GAP) * scale;
	const float message_height = message.empty()
		? 0.0f
		: MESSAGE_GAP * scale + regular->CalcTextSizeA(MESSAGE_SIZE * scale, FLT_MAX, width, message.c_str()).y;
	const float hint_height = (HINT_GAP + HINT_SIZE) * scale;
	const float room = display.y - 2.0f * SAFE_Y * scale - header_height - message_height - hint_height;
	source.fit = 1 + (size_t)std::max(0.0f, (room - SELECTED_ROW * scale) / (ROW * scale));
	const size_t rows = std::min(count, source.fit);
	if (source.selected < source.first)
		source.first = source.selected;
	if (source.selected >= source.first + source.fit)
		source.first = source.selected + 1 - source.fit;
	if (source.first + rows > count)
		source.first = count - rows;
	static const char EMPTY[] = "Put disc images in games\\ beside swanstation.xex, or in games\\ on a USB drive.";
	static const char FORMATS[] = ".cue   .chd   .m3u   .pbp   .iso   .ecm   .mds   .bin";
	const float empty_height =
		regular->CalcTextSizeA(EMPTY_SIZE * scale, FLT_MAX, width, EMPTY).y + (MESSAGE_GAP + HINT_SIZE) * scale;
	const float list_height = count ? (SELECTED_ROW + (rows - 1) * ROW) * scale : empty_height;
	const float block = header_height + list_height + message_height + hint_height;
	const float top = std::max(SAFE_Y * scale, (display.y - block) / 2.0f);
	draw->AddLine(ImVec2(rule_x, top), ImVec2(rule_x, top + block), RULE, scale);

	const std::string discs = count == 0 ? "NO DISCS" : count == 1 ? "1 DISC" : std::to_string(count) + " DISCS";
	draw_text(draw, regular, HEADER_SIZE * scale, ImVec2(x, top), FADED, source.label + std::string(MIDDLE_DOT) + discs,
		HEADER_TRACKING);
	float y = top + header_height;
	if (count) {
		const bool wide = count >= 100;
		const float title_x = x + text_width(regular, ROW_SIZE * scale, wide ? "000" : "00") + NUMBER_GAP * scale;
		for (size_t i = source.first; i < source.first + rows; i++) {
			const Game &game = source.games[i];
			const bool selected = i == source.selected;
			const float row = (selected ? SELECTED_ROW : ROW) * scale;
			const float size = (selected ? SELECTED_SIZE : ROW_SIZE) * scale;
			ImFont *const font = selected ? medium : regular;
			if (selected) {
				draw->AddRectFilled(
					ImVec2(x - SELECTED_BLEED * scale, y), ImVec2(right + SELECTED_BLEED * scale, y + row), INK);
			}
			char number[12];
			snprintf(number, sizeof number, wide ? "%03u" : "%02u", (unsigned)(i + 1));
			draw_text(draw, regular, ROW_SIZE * scale, ImVec2(x, y + (row - ROW_SIZE * scale) / 2.0f),
				selected ? PAPER : NUMBER, number);
			float title_right = right;
			if (game.discs > 1) {
				const std::string label = std::to_string(game.discs) + " discs";
				const float label_width = text_width(regular, DISCS_SIZE * scale, label);
				draw_text(draw, regular, DISCS_SIZE * scale,
					ImVec2(right - label_width, y + (row - DISCS_SIZE * scale) / 2.0f), selected ? PAPER : NUMBER,
					label);
				title_right -= label_width + NUMBER_GAP * scale;
			}
			draw_text(draw, font, size, ImVec2(title_x, y + (row - size) / 2.0f), selected ? PAPER : INK,
				fitted(font, size, game.name, title_right - title_x));
			y += row;
		}
	} else {
		draw->AddText(regular, EMPTY_SIZE * scale, ImVec2(x, y), INK, EMPTY, nullptr, width);
		draw_text(draw, regular, HINT_SIZE * scale, ImVec2(x, y + list_height - HINT_SIZE * scale), FADED, FORMATS);
		y += list_height;
	}
	if (!message.empty()) {
		draw->AddText(
			regular, MESSAGE_SIZE * scale, ImVec2(x, y + MESSAGE_GAP * scale), INK, message.c_str(), nullptr, width);
		y += message_height;
	}
	std::string hints = count ? "A  play      B  quit" : "B  quit";
	if (source_count > 1)
		hints += "      LB RB  drive";
	if (count > source.fit)
		hints += "      LT RT  page";
	draw_text(draw, regular, HINT_SIZE * scale, ImVec2(x, y + HINT_GAP * scale), FADED, hints);
}

void draw_pause(ImDrawList *draw, ImVec2 display, const std::string &game_name, int selected)
{
	const float scale = display.y / 720.0f;
	draw->AddRectFilled(ImVec2(0.0f, 0.0f), display, DIM);
	const float radius = PAUSE_DISC_RADIUS * scale;
	const float name_size = PAUSE_NAME_SIZE * scale;
	const float most =
		display.x - 2.0f * SAFE_X * scale - (2.0f * PAUSE_PAD_X + PAUSE_GAP + COLUMN_PAD) * scale - 2.0f * radius;
	const std::string name = fitted(regular, name_size, game_name, most);
	const float column = std::max(PAUSE_COLUMN * scale, text_width(regular, name_size, name));
	const float column_height = HEADER_SIZE * scale + PAUSE_NAME_TOP * scale + name_size + PAUSE_NAME_GAP * scale +
		IM_ARRAYSIZE(PAUSE_ITEMS) * PAUSE_ROW * scale + (PAUSE_HINT_GAP + HINT_SIZE) * scale;
	const ImVec2 size(2.0f * PAUSE_PAD_X * scale + 2.0f * radius + (PAUSE_GAP + COLUMN_PAD) * scale + column,
		2.0f * PAUSE_PAD_Y * scale + std::max(2.0f * radius, column_height));
	const ImVec2 at((display.x - size.x) / 2.0f, (display.y - size.y) / 2.0f);
	draw->AddRectFilled(at, at + size, PAPER);
	draw->AddRect(at, at + size, RULE, 0.0f, 0, scale);
	draw_disc(draw, ImVec2(at.x + PAUSE_PAD_X * scale + radius, at.y + size.y / 2.0f), radius, scale);
	const float rule_x = at.x + PAUSE_PAD_X * scale + 2.0f * radius + PAUSE_GAP * scale;
	draw->AddLine(
		ImVec2(rule_x, at.y + PAUSE_PAD_Y * scale), ImVec2(rule_x, at.y + size.y - PAUSE_PAD_Y * scale), RULE, scale);

	const float x = rule_x + COLUMN_PAD * scale;
	float y = at.y + (size.y - column_height) / 2.0f;
	draw_text(draw, regular, HEADER_SIZE * scale, ImVec2(x, y), FADED, "PAUSED", HEADER_TRACKING);
	y += (HEADER_SIZE + PAUSE_NAME_TOP) * scale;
	draw_text(draw, regular, name_size, ImVec2(x, y), INK, name);
	y += name_size + PAUSE_NAME_GAP * scale;
	const float row = PAUSE_ROW * scale;
	const float row_size = PAUSE_ROW_SIZE * scale;
	const float title_x = x + text_width(regular, row_size, "00") + NUMBER_GAP * scale;
	for (int i = 0; i < IM_ARRAYSIZE(PAUSE_ITEMS); i++) {
		const bool chosen = i == selected;
		if (chosen) {
			draw->AddRectFilled(
				ImVec2(x - SELECTED_BLEED * scale, y), ImVec2(x + column + SELECTED_BLEED * scale, y + row), INK);
		}
		char number[12];
		snprintf(number, sizeof number, "%02d", i + 1);
		const float text_y = y + (row - row_size) / 2.0f;
		draw_text(draw, regular, row_size, ImVec2(x, text_y), chosen ? PAPER : NUMBER, number);
		draw_text(
			draw, chosen ? medium : regular, row_size, ImVec2(title_x, text_y), chosen ? PAPER : INK, PAUSE_ITEMS[i]);
		y += row;
	}
	draw_text(
		draw, regular, HINT_SIZE * scale, ImVec2(x, y + PAUSE_HINT_GAP * scale), FADED, "A  select      B  resume");
}

bool pressed(ImGuiKey key, bool repeat = false)
{
	return ImGui::IsKeyPressed(key, repeat);
}

// Writes the frame resolved into the front buffer at address as a binary PPM beside the .xex. The front buffer is a
// tiled A8R8G8B8 texture whose words hold B, G, R then A (xsdk's docs/GUIDE.md, "Textures, render targets and
// resolves").
void write_shot(uint32_t address, uint32_t width, uint32_t height, const char *name)
{
	const NTSTATUS waited = xsdk_gpu_wait(xsdk_gpu_fence(), BEGIN_TIMEOUT_MS);
	XsdkTexture frame;
	const NTSTATUS status = xsdk_texture_init(&frame, XSDK_FORMAT_A8R8G8B8, width, height, 1, true);
	FILE *const file = NT_SUCCESS(waited) && NT_SUCCESS(status) ? fopen(name, "wb") : nullptr;
	if (!file) {
		xsdk_log_write("picker: can't write %s: 0x%08X, 0x%08X", name, (unsigned)waited, (unsigned)status);
		return;
	}
	xsdk_texture_attach(&frame, xsdk_present_pixels(address));
	fprintf(file, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height);
	std::vector<uint32_t> row(width);
	std::vector<uint8_t> rgb(width * 3);
	for (uint32_t y = 0; y < height; y++) {
		xsdk_texture_download_rows(&frame, 0, y, 1, row.data(), width * 4);
		for (uint32_t x = 0; x < width; x++) {
			const uint32_t pixel = __builtin_bswap32(row[x]);
			rgb[x * 3] = (uint8_t)(pixel >> 16);
			rgb[x * 3 + 1] = (uint8_t)(pixel >> 8);
			rgb[x * 3 + 2] = (uint8_t)pixel;
		}
		fwrite(rgb.data(), 1, rgb.size(), file);
	}
	fclose(file);
	xsdk_log_write("picker: wrote %s", name);
}

// Presents ImGui's frame, over what draw_frame draws first if it's given, and writes it as shot if that's given.
bool present(uint32_t width, uint32_t height, void (*draw_frame)(void), const char *shot = nullptr)
{
	uint32_t address;
	const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("picker: xsdk_present_begin failed: 0x%08X", (unsigned)status);
		return false;
	}
	xsdk_gpu_set_target(width, height);
	if (draw_frame)
		draw_frame();
	ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
	xsdk_gpu_resolve(address, width, height, BLACK_ARGB);
	if (shot)
		write_shot(address, width, height, shot);
	xsdk_present_end();
	return true;
}

// The fixture picker_check_list writes, each file's name and text, and the list it should give: each entry's path
// below the fixture's folder, then its name and its discs.
const char CHECK_ROOT[] = "game:\\games_check";
const struct {
	const char *name;
	const char *text;
} CHECK_FILES[] = {
	{ "Alpha.cue", "FILE \"Alpha.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n" },
	{ "Alpha.bin", "" },
	{ "Beta.bin", "" },
	{ "Delta.cue",
		"FILE \"Delta (Track 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
		"FILE \"Delta (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n" },
	{ "Delta (Track 1).bin", "" },
	{ "Delta (Track 2).bin", "" },
	{ "Gamma.m3u", "# two discs\r\nGamma (Disc 1).cue\r\nGamma (Disc 2).cue\r\n" },
	{ "Gamma (Disc 1).cue", "FILE \"Gamma (Disc 1).bin\" BINARY\n" },
	{ "Gamma (Disc 1).bin", "" },
	{ "Gamma (Disc 2).cue", "FILE \"Gamma (Disc 2).bin\" BINARY\n" },
	{ "Gamma (Disc 2).bin", "" },
	{ "readme.txt", "not a disc\n" },
	{ "Epsilon\\Epsilon.chd", "" },
	{ "zeta.iso", "" },
};
const char *const CHECK_EXPECTED[] = {
	"Alpha.cue: Alpha",
	"Beta.bin: Beta",
	"Delta.cue: Delta",
	"Epsilon\\Epsilon.chd: Epsilon",
	"Gamma.m3u: Gamma, 2 discs",
	"zeta.iso: zeta",
};

} // namespace

bool picker_open(void)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false;
	regular =
		io.Fonts->AddFontFromMemoryTTF((void *)font_regular, (int)(font_regular_end - font_regular), ROW_SIZE, &config);
	medium =
		io.Fonts->AddFontFromMemoryTTF((void *)font_medium, (int)(font_medium_end - font_medium), ROW_SIZE, &config);
	if (!regular || !medium || !ImGui_ImplXsdk_Init()) {
		xsdk_log_write("picker: setting up Dear ImGui failed");
		ImGui::DestroyContext();
		return false;
	}
	return true;
}

void picker_close(void)
{
	ImGui_ImplXsdk_Shutdown();
	ImGui::DestroyContext();
}

bool disc_opens(const char *path)
{
	return CDImage::Open(path, CDImage::OpenFlags::None, nullptr) != nullptr;
}

bool picker_choose(uint32_t width, uint32_t height, const char *failed_path, char *path, size_t path_bytes)
{
	scan();
	const std::string message = failed_path
		? stem(failed_path) + " didn't start. Check its disc image, and that a PS1 BIOS is beside swanstation.xex."
		: std::string();
	size_t rest_frames = 0;
	const Game *rested_on = nullptr;
	for (;;) {
		ImGui_ImplXsdk_NewFrame(width, height);
		ImGui::NewFrame();
		Source &source = sources[current];
		const size_t count = source.games.size();
		if (count) {
			if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true))
				source.selected = std::min(source.selected + 1, count - 1);
			if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true))
				source.selected = source.selected ? source.selected - 1 : 0;
			if (pressed(ImGuiKey_GamepadR2, true))
				source.selected = std::min(source.selected + source.fit, count - 1);
			if (pressed(ImGuiKey_GamepadL2, true))
				source.selected = source.selected > source.fit ? source.selected - source.fit : 0;
		}
		const bool previous = pressed(ImGuiKey_GamepadL1);
		if (previous || pressed(ImGuiKey_GamepadR1)) {
			const size_t total = IM_ARRAYSIZE(sources);
			do
				current = (current + (previous ? total - 1 : 1)) % total;
			while (!sources[current].present);
		}
		Source &shown = sources[current];
		if (shown.games.size() && pressed(ImGuiKey_GamepadFaceDown)) {
			last_path = shown.games[shown.selected].path;
			snprintf(path, path_bytes, "%s", last_path.c_str());
			finish_serial(true);
			ImGui::EndFrame();
			return true;
		}
		if (pressed(ImGuiKey_GamepadFaceRight)) {
			finish_serial(true);
			ImGui::EndFrame();
			return false;
		}
		finish_serial(false);
		Game *const game = shown.games.empty() ? nullptr : &shown.games[shown.selected];
		rest_frames = game == rested_on ? rest_frames + 1 : 0;
		rested_on = game;
		if (game && !game->serial_read && !serial_read.thread && rest_frames >= SERIAL_REST_FRAMES)
			start_serial(*game);
		size_t present_count = 0;
		for (const Source &source_shown : sources)
			present_count += source_shown.present;
		draw_picker(ImGui::GetBackgroundDrawList(), ImVec2((float)width, (float)height), shown, present_count, message);
		ImGui::Render();
		// The shot once the selected game's serial is in.
		const bool shoot = picker_shots && (!game || (game->serial_read && !serial_read.thread)) &&
			rest_frames >= SERIAL_REST_FRAMES + SHOT_FRAMES;
		if (!present(width, height, nullptr, shoot ? "shot_picker.ppm" : nullptr) || shoot) {
			finish_serial(true);
			return false;
		}
	}
}

PauseChoice picker_pause(uint32_t width, uint32_t height, const char *game_path, void (*draw_frame)(void))
{
	const std::string name = stem(game_path);
	// Back to games selects the paused game, host.txt's game= too when it's in the list.
	last_path = game_path;
	int selected = PAUSE_RESUME;
	for (uint32_t frame = 0;; frame++) {
		ImGui_ImplXsdk_NewFrame(width, height);
		ImGui::NewFrame();
		if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true))
			selected = std::min(selected + 1, (int)IM_ARRAYSIZE(PAUSE_ITEMS) - 1);
		if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true))
			selected = std::max(selected - 1, 0);
		if (pressed(ImGuiKey_GamepadFaceDown) || pressed(ImGuiKey_GamepadFaceRight)) {
			const PauseChoice choice = pressed(ImGuiKey_GamepadFaceDown) ? (PauseChoice)selected : PAUSE_RESUME;
			ImGui::EndFrame();
			return choice;
		}
		draw_pause(ImGui::GetBackgroundDrawList(), ImVec2((float)width, (float)height), name, selected);
		ImGui::Render();
		const bool shoot = picker_shots && frame == SHOT_FRAMES;
		if (!present(width, height, draw_frame, shoot ? "shot_pause.ppm" : nullptr) || shoot)
			return PAUSE_TO_GAMES;
	}
}

bool picker_check_list(void)
{
	mkdir(CHECK_ROOT, 0777);
	mkdir((std::string(CHECK_ROOT) + "\\Epsilon").c_str(), 0777);
	for (const auto &file : CHECK_FILES) {
		const std::string path = std::string(CHECK_ROOT) + "\\" + file.name;
		FILE *const out = fopen(path.c_str(), "wb");
		if (!out) {
			xsdk_log_write("picker: list check: can't write %s", path.c_str());
			return false;
		}
		fputs(file.text, out);
		fclose(out);
	}
	std::vector<Game> games;
	list_games(CHECK_ROOT, games);
	std::vector<std::string> got;
	for (const Game &game : games) {
		got.push_back(game.path.substr(strlen(CHECK_ROOT) + 1) + ": " + game.name +
			(game.discs > 1 ? ", " + std::to_string(game.discs) + " discs" : ""));
		xsdk_log_write("picker: list check: %s", got.back().c_str());
	}
	const bool passed = got == std::vector<std::string>(std::begin(CHECK_EXPECTED), std::end(CHECK_EXPECTED));
	xsdk_log_write("picker: list check: %s, %u entries of %u expected", passed ? "PASS" : "FAIL", (unsigned)got.size(),
		(unsigned)IM_ARRAYSIZE(CHECK_EXPECTED));
	return passed;
}
