// Prawdziwy dekoder i logika cache działają na hoście; GPU, wątki i sieć zastępują kontrolowane porty.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "../src/xenon/cover_art.cpp"
#include "../src/xenon/picker.cpp"

#include <assert.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <thread>

namespace
{
bool gpu_finished = true;
unsigned freed_textures;
unsigned created_textures;
unsigned network_requests;
bool network_available = true;
std::vector<unsigned char> network_bytes;

std::vector<unsigned char> read_fixture(const char *name)
{
	std::ifstream input(name, std::ios::binary);
	assert(input.good());
	return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
} // namespace

namespace CoverDownload
{
bool Available() { return network_available; }
bool IsValidSerial(const std::string &serial) { return serial == "SLUS-00402" || serial == "SCES-01237"; }
bool FetchBytes(const std::string &, std::vector<unsigned char> *bytes, bool, const bool *cancel)
{
	++network_requests;
	if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE))
		return false;
	*bytes = network_bytes;
	return !bytes->empty();
}
} // namespace CoverDownload

std::unique_ptr<CDImage> CDImage::Open(const char *, OpenFlags, Common::Error *) { return {}; }
std::string System::GetGameCodeForImage(CDImage *, bool) { return {}; }

extern "C" {
void xsdk_log_write(const char *, ...) {}
bool xsdk_gpu_fence_done(uint32_t) { return gpu_finished; }
NTSTATUS xsdk_gpu_wait(uint32_t, uint32_t) { return gpu_finished ? 0 : -1; }
uint32_t xsdk_gpu_fence() { return 7; }
void xsdk_gpu_invalidate(uint32_t, uint32_t) {}
void xsdk_texture_free(XsdkTexture *) { ++freed_textures; }
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t, uint32_t width, uint32_t height, uint32_t, bool)
{
	++created_textures;
	texture->width = width;
	texture->height = height;
	return 0;
}
void xsdk_texture_upload(XsdkTexture *, uint32_t, const void *, uint32_t) {}
NTSTATUS xsdk_thread_create_on(HANDLE *, uint32_t, XsdkThreadEntry, void *, uint32_t) { return -1; }
NTSTATUS xsdk_thread_sleep(uint32_t milliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
	return 0;
}
NTSTATUS NtWaitForSingleObjectEx(HANDLE, uint32_t, uint32_t, const int64_t *) { return 0; }
NTSTATUS NtClose(HANDLE) { return 0; }
}

int main()
{
	using namespace CoverArt;
	const std::vector<unsigned char> png = read_fixture("valid.png");
	Job valid;
	assert(decode(png, valid));
	assert(valid.width == 2 && valid.height == 1 && valid.pixels.size() == 8);
	assert(valid.pixels[0] == 255 && valid.pixels[1] == 0 && valid.pixels[2] == 0 && valid.pixels[3] == 255);
	Job valid_jpeg;
	assert(decode(read_fixture("valid.jpg"), valid_jpeg));
	assert(valid_jpeg.width == 2 && valid_jpeg.height == 1 && valid_jpeg.pixels.size() == 8);
	// JPEG jest stratny, dlatego sprawdzamy format i pełną nieprzezroczystość zamiast dokładnych wartości RGB.
	assert(valid_jpeg.pixels[3] == 255 && valid_jpeg.pixels[7] == 255);
	Job rejected;
	assert(!decode(read_fixture("wide.png"), rejected));
	assert(!decode(read_fixture("corrupt.png"), rejected));
	assert(!decode({ 0xFF, 0xD8, 0xFF, 0x00, 0x01 }, rejected));
	assert(!decode({ 'G', 'I', 'F', '8', '9', 'a' }, rejected));
	assert(!decode(std::vector<unsigned char>(MAX_FILE_BYTES + 1), rejected));

	std::vector<unsigned char> bounded;
	assert(read_bounded("valid.png", bounded) && bounded == png);
	assert(!read_bounded("missing.png", bounded));
	assert(!read_bounded("oversized.bin", bounded));
	const ImVec2 portrait = fit_size(300, 600, 400.0f);
	const ImVec2 landscape = fit_size(600, 300, 400.0f);
	assert(portrait.x == 200.0f && portrait.y == 400.0f);
	assert(landscape.x == 400.0f && landscape.y == 200.0f);

	Job offline;
	offline.serial = "SLUS-00402";
	offline.settings.auto_download = false;
	network_bytes = png;
	load_cover(&offline);
	assert(offline.done && offline.pixels.empty() && network_requests == 0);
	Job corrupt;
	corrupt.serial = "SLUS-00402";
	network_bytes = { 0xFF, 0xD8, 0xFF, 0x00 };
	load_cover(&corrupt);
	assert(corrupt.done && corrupt.pixels.empty() && network_requests == 1);
	assert(!read_bounded(cache_path(corrupt.serial, ".jpg"), bounded));
	Job downloaded;
	downloaded.serial = "SLUS-00402";
	network_bytes = png;
	load_cover(&downloaded);
	assert(downloaded.done && downloaded.pixels.size() == 8 && network_requests == 2);
	assert(read_bounded(cache_path(downloaded.serial, ".png"), bounded) && bounded == png);
	Job cached;
	cached.serial = downloaded.serial;
	load_cover(&cached);
	assert(cached.pixels.size() == 8 && network_requests == 2);
	Job cancelled;
	cancelled.serial = "SCES-01237";
	cancelled.cancelled = true;
	load_cover(&cancelled);
	assert(cancelled.done && cancelled.pixels.empty() && network_requests == 2);

	// Wynik poprzedniego zaznaczenia nigdy nie tworzy tekstury bieżącej gry.
	settings.enabled = false;
	generation = 8;
	selected_serial = "SCES-01237";
	job.reset(new Job);
	job->serial = "SLUS-00402";
	job->generation = 7;
	job->selected = true;
	job->done = true;
	job->pixels = valid.pixels;
	job->width = valid.width;
	job->height = valid.height;
	worker = 1;
	Update();
	assert(created_textures == 0 && !job && worker == 0);
	has_texture = true;
	texture_fence = 42;
	gpu_finished = false;
	assert(!release_texture(false) && freed_textures == 0 && has_texture);
	gpu_finished = true;
	assert(release_texture(false) && freed_textures == 1 && !has_texture);
	job.reset(new Job);
	worker = 1;
	RequestStop();
	assert(job->cancelled && !TryStop());
	job->done = true;
	assert(TryStop() && !job && worker == 0);
	// Wielokrotne oddawanie priorytetu zaznaczeniu nie powiększa kolejki startup.
	settings.enabled = true;
	startup_paths = { "one.chd", "two.chd" };
	job.reset(new Job);
	job->game_path = "one.chd";
	worker = 1;
	for (unsigned move = 0; move < 100; ++move) {
		Select("");
		job->cancelled = false;
		Select("SLUS-00402");
		assert(startup_paths.size() == 2 && startup_retry == "one.chd");
	}
	job->done = true;
	RequestStop();
	assert(TryStop());
	has_texture = true;
	texture_fence = 42;
	gpu_finished = false;
	assert(!TryStop() && has_texture && freed_textures == 1);
	gpu_finished = true;
	assert(TryStop() && !has_texture && freed_textures == 2);

	// Trafienie w RAM rysuje nową okładkę nawet podczas kończenia starego HTTP.
	ram_cache.clear();
	selected_serial.clear();
	startup_paths.clear();
	prefetch_paths.clear();
	Job preview;
	assert(decode(read_fixture("large.png"), preview));
	assert(preview.width == 512 && preview.height == 256);
	assert(preview.pixels.size() == 512 * 256 * 4);
	Job warm;
	warm.serial = "SLUS-00402";
	warm.width = valid.width;
	warm.height = valid.height;
	warm.pixels = valid.pixels;
	remember_cover(warm);
	job.reset(new Job);
	job->serial = "SCES-01237";
	job->selected = true;
	worker = 1;
	const unsigned before_upload = created_textures;
	Select("SLUS-00402");
	Update();
	assert(has_texture && texture_serial == "SLUS-00402");
	assert(created_textures == before_upload + 1 && worker == 1 && !job->done);
	job->done = true;
	RequestStop();
	assert(TryStop() && ram_cache.empty());
	// Prefetch nigdy nie inicjuje pobrania brakującej grafiki.
	Job neighbour;
	neighbour.serial = "SCES-01237";
	neighbour.prefetch = true;
	const unsigned before_prefetch = network_requests;
	load_cover(&neighbour);
	assert(network_requests == before_prefetch && neighbour.pixels.empty());
	settings.enabled = false;
	job.reset(new Job);
	job->serial = "SLUS-00402";
	job->game_path = "known.chd";
	job->done = true;
	worker = 1;
	Update();
	assert(KnownSerial("known.chd") == "SLUS-00402");
	assert(KnownSerial("unknown.chd").empty());
	// Limit pamięci nie rośnie razem z kolekcją gier.
	for (unsigned index = 0; index < 100; ++index) {
		Job item;
		item.serial = std::to_string(index);
		item.width = valid.width;
		item.height = valid.height;
		item.pixels = valid.pixels;
		remember_cover(item);
		assert(ram_cache.size() <= RAM_CACHE_ENTRIES);
	}
	assert(ram_cache.size() == RAM_CACHE_ENTRIES && !cached_cover("0"));
	ram_cache.clear();
	Begin(settings, { "known.chd" });
	assert(KnownSerial("known.chd").empty());

	// Zmiana trybu pada zachowuje ustawienia okładek, ścieżkę gier i obce linie.
	{
		std::ofstream config("game:\\settings.txt");
		config << "# komentarz\ncustom_option=keep\nanalog=0\ncover_auto_download=false\n"
			"cover_enabled=1\ncover_download_on_startup=0\ncover_fallback_source=0\n"
			"games_root=hdd1:\\apps\\swanstation\\games\n";
	}
	load_settings();
	assert(!picker_analog && !cover_settings.auto_download && !cover_settings.download_on_startup);
	assert(!cover_settings.fallback_source && cover_settings.enabled);
	assert(games_root == "hdd:\\apps\\swanstation\\games");
	toggle_analog();
	const auto config_bytes = read_fixture("game:\\settings.txt");
	const std::string config(config_bytes.begin(), config_bytes.end());
	assert(config.find("custom_option=keep\n") != std::string::npos);
	assert(config.find("# komentarz\n") != std::string::npos);
	assert(config.find("analog=1\n") != std::string::npos);
	assert(config.find("cover_auto_download=0\n") != std::string::npos);
	assert(config.find("games_root=hdd:\\apps\\swanstation\\games\n") != std::string::npos);
	puts("PASS: dekoder, cache, limity, proporcje, offline, anulowanie, generacje, fence i ustawienia");
}
