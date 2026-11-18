#include "cover_art.h"
#include "cover_download.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <memory>
#include <map>
#include <set>
#include <utility>

#include "common/cd_image.h"
#include "core/system.h"
#include "imgui.h"
#include "stb_image.h"
#include "stb_image_resize.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

namespace CoverArt
{
namespace
{
constexpr size_t MAX_FILE_BYTES = 2 * 1024 * 1024;
constexpr int MAX_DIMENSION = 2048;
constexpr int MAX_PIXELS = 4 * 1024 * 1024;
constexpr int PREVIEW_DIMENSION = 512;
constexpr size_t RAM_CACHE_ENTRIES = 6;
constexpr uint32_t WORKER_STACK_BYTES = 512 * 1024;
constexpr uint32_t WORKER_HARDWARE_THREAD = 5;
constexpr uint32_t GPU_TIMEOUT_MS = 5000;
constexpr uint64_t REQUEST_INTERVAL_US = 1000000;
constexpr uint32_t CANCEL_POLL_MS = 50;
const char CACHE_DIRECTORY[] = "game:\\covers";

struct Job {
	std::string serial;
	std::string game_path;
	uint64_t generation = 0;
	uint64_t retry_generation = 0;
	Settings settings;
	std::set<std::string> attempted;
	std::vector<unsigned char> pixels;
	int width = 0;
	int height = 0;
	bool selected = false;
	bool downloaded = false;
	bool done = false;
	bool cancelled = false;
	bool prefetch = false;
};

struct CachedCover {
	std::string serial;
	std::vector<unsigned char> pixels;
	int width;
	int height;
};

Settings settings;
std::unique_ptr<Job> job;
HANDLE worker;
std::vector<std::string> startup_paths;
std::string startup_retry;
size_t next_startup;
std::set<std::string> attempted_downloads;
std::string selected_serial;
std::string texture_serial;
uint64_t generation;
uint64_t retry_generation;
bool selection_attempted;
XsdkTexture texture;
bool has_texture;
bool drawn;
uint32_t texture_fence;
// Wyłącznie pojedynczy pracownik sieciowy; kolejne zadania uruchamiamy po join poprzedniego.
uint64_t next_request_us;
std::vector<CachedCover> ram_cache;
std::map<std::string, std::string> known_serials;
std::vector<std::string> prefetch_paths;
size_t next_prefetch;

CachedCover *cached_cover(const std::string &serial)
{
	for (CachedCover &cover : ram_cache) {
		if (cover.serial == serial)
			return &cover;
	}
	return nullptr;
}

void remember_cover(Job &result)
{
	if (result.pixels.empty() || cached_cover(result.serial))
		return;
	if (ram_cache.size() == RAM_CACHE_ENTRIES) {
		// Aktywna okładka pozostaje w RAM, a sąsiedzi zajmują najwyżej pięć miejsc.
		auto oldest = std::find_if(ram_cache.begin(), ram_cache.end(),
			[](const CachedCover &cover) { return cover.serial != selected_serial; });
		ram_cache.erase(oldest);
	}
	ram_cache.push_back({ result.serial, std::move(result.pixels), result.width, result.height });
}

uint64_t monotonic_us()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

bool allow_request(Job &result)
{
	while (monotonic_us() < next_request_us) {
		if (__atomic_load_n(&result.cancelled, __ATOMIC_ACQUIRE))
			return false;
		const NTSTATUS status = xsdk_thread_sleep(CANCEL_POLL_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("covers: blad oczekiwania limitu zapytan: 0x%08X", (unsigned)status);
			return false;
		}
	}
	if (__atomic_load_n(&result.cancelled, __ATOMIC_ACQUIRE))
		return false;
	next_request_us = monotonic_us() + REQUEST_INTERVAL_US;
	return true;
}

std::string cache_path(const std::string &serial, const char *extension)
{
	return std::string(CACHE_DIRECTORY) + "\\" + serial + extension;
}

bool read_bounded(const std::string &path, std::vector<unsigned char> &bytes)
{
	FILE *const file = fopen(path.c_str(), "rb");
	if (!file)
		return false;
	struct stat info;
	if (fstat(fileno(file), &info) != 0 || info.st_size <= 0 || (uint64_t)info.st_size > MAX_FILE_BYTES) {
		fclose(file);
		return false;
	}
	bytes.resize((size_t)info.st_size);
	const bool complete = fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
	const bool closed = fclose(file) == 0;
	return complete && closed;
}

const char *image_extension(const std::vector<unsigned char> &bytes)
{
	static const unsigned char PNG_SIGNATURE[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if (bytes.size() >= sizeof PNG_SIGNATURE && !memcmp(bytes.data(), PNG_SIGNATURE, sizeof PNG_SIGNATURE))
		return ".png";
	if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
		return ".jpg";
	return nullptr;
}

bool decode(const std::vector<unsigned char> &bytes, Job &result)
{
	if (bytes.empty() || bytes.size() > MAX_FILE_BYTES || !image_extension(bytes))
		return false;
	int width, height, channels;
	if (!stbi_info_from_memory(bytes.data(), (int)bytes.size(), &width, &height, &channels) ||
		width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION ||
		width > MAX_PIXELS / height)
		return false;
	unsigned char *const pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &width, &height, &channels, 4);
	if (!pixels)
		return false;
	// Nagłówek i dekoder muszą zgadzać się co do bezpiecznego rozmiaru obrazu.
	const bool valid = width > 0 && height > 0 && width <= MAX_DIMENSION && height <= MAX_DIMENSION &&
		width <= MAX_PIXELS / height;
	if (valid) {
		const int longest = std::max(width, height);
		result.width = longest > PREVIEW_DIMENSION ? std::max(1, width * PREVIEW_DIMENSION / longest) : width;
		result.height = longest > PREVIEW_DIMENSION ? std::max(1, height * PREVIEW_DIMENSION / longest) : height;
		result.pixels.resize((size_t)result.width * (size_t)result.height * 4);
		if (longest <= PREVIEW_DIMENSION)
			memcpy(result.pixels.data(), pixels, result.pixels.size());
		else if (!stbir_resize_uint8(pixels, width, height, 0, result.pixels.data(), result.width, result.height, 0, 4)) {
			result.pixels.clear();
			stbi_image_free(pixels);
			return false;
		}
	}
	stbi_image_free(pixels);
	return valid;
}

bool save_cache(const std::string &serial, const std::vector<unsigned char> &bytes)
{
	if (mkdir(CACHE_DIRECTORY, 0777) != 0 && errno != EEXIST) {
		xsdk_log_write("covers: nie mozna utworzyc katalogu cache: %d", errno);
		return false;
	}
	const std::string temporary = cache_path(serial, ".tmp");
	const std::string destination = cache_path(serial, image_extension(bytes));
	FILE *const file = fopen(temporary.c_str(), "wb");
	if (!file) {
		xsdk_log_write("covers: nie mozna zapisac pliku tymczasowego: %s", temporary.c_str());
		return false;
	}
	const bool complete = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	const bool closed = fclose(file) == 0;
	if (!complete || !closed || rename(temporary.c_str(), destination.c_str()) != 0) {
		xsdk_log_write("covers: nie mozna opublikowac cache: %s", destination.c_str());
		if (remove(temporary.c_str()) != 0)
			xsdk_log_write("covers: pozostawiono niepelny plik tymczasowy: %s", temporary.c_str());
		return false;
	}
	return true;
}

uint32_t load_cover(void *context)
{
	Job &result = *(Job *)context;
	if (!result.game_path.empty() && result.serial.empty()) {
		const std::unique_ptr<CDImage> image = CDImage::Open(result.game_path.c_str(), CDImage::OpenFlags::None, nullptr);
		if (image)
			result.serial = System::GetGameCodeForImage(image.get(), false);
	}
	if (CoverDownload::IsValidSerial(result.serial)) {
		if (__atomic_load_n(&result.cancelled, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&result.done, true, __ATOMIC_RELEASE);
			return 0;
		}
		std::vector<unsigned char> bytes;
		bool loaded = false;
		for (const char *extension : { ".jpg", ".png" }) {
			if (read_bounded(cache_path(result.serial, extension), bytes) && decode(bytes, result)) {
				loaded = true;
				break;
			}
		}
		if (!loaded && !__atomic_load_n(&result.cancelled, __ATOMIC_ACQUIRE) &&
			!result.prefetch && result.settings.auto_download && CoverDownload::Available() &&
			result.attempted.count(result.serial) == 0 && allow_request(result)) {
			result.downloaded = true;
			if (CoverDownload::FetchBytes(result.serial, &bytes, result.settings.fallback_source, &result.cancelled) &&
				!__atomic_load_n(&result.cancelled, __ATOMIC_ACQUIRE) && decode(bytes, result)) {
				save_cache(result.serial, bytes);
				xsdk_log_write("covers: pobrano poprawna okladke %s", result.serial.c_str());
			}
		}
	}
	__atomic_store_n(&result.done, true, __ATOMIC_RELEASE);
	return 0;
}

bool release_texture(bool wait)
{
	if (!has_texture)
		return true;
	if (texture_fence && !xsdk_gpu_fence_done(texture_fence)) {
		if (!wait)
			return false;
		const NTSTATUS status = xsdk_gpu_wait(texture_fence, GPU_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			// Zachowanie zasobu jest bezpieczniejsze od zwolnienia pamięci czytanej przez GPU.
			xsdk_log_write("covers: GPU nie zwolnilo tekstury: 0x%08X", (unsigned)status);
			return false;
		}
	}
	xsdk_texture_free(&texture);
	has_texture = false;
	texture_serial.clear();
	texture_fence = 0;
	return true;
}

ImVec2 fit_size(uint32_t width, uint32_t height, float extent)
{
	const float factor = extent / (float)std::max(width, height);
	return ImVec2((float)width * factor, (float)height * factor);
}

void start_job(bool selected, const std::string &value, bool prefetch = false)
{
	job.reset(new Job);
	job->settings = settings;
	job->generation = generation;
	job->retry_generation = retry_generation;
	job->selected = selected;
	job->prefetch = prefetch;
	job->attempted = attempted_downloads;
	if (selected)
		job->serial = value;
	else {
		job->game_path = value;
		auto known = known_serials.find(value);
		if (known != known_serials.end()) {
			job->serial = known->second;
			// Ścieżka pozostaje kluczem indeksu, ale nie trzeba ponownie otwierać CHD.
		}
	}
	const NTSTATUS status = xsdk_thread_create_on(&worker, WORKER_STACK_BYTES, load_cover, job.get(), WORKER_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("covers: nie mozna uruchomic pracownika: 0x%08X", (unsigned)status);
		worker = 0;
		job.reset();
	}
}

bool upload_cached(const CachedCover &cover)
{
	if (!release_texture(false))
		return false;
	const NTSTATUS made = xsdk_texture_create(&texture, XSDK_FORMAT_R8G8B8A8,
		(uint32_t)cover.width, (uint32_t)cover.height, 1, true);
	if (!NT_SUCCESS(made)) {
		xsdk_log_write("covers: nie mozna utworzyc tekstury: 0x%08X", (unsigned)made);
		return false;
	}
	xsdk_texture_upload(&texture, 0, cover.pixels.data(), (uint32_t)cover.width * 4);
	xsdk_gpu_invalidate(texture.address, texture.size_bytes);
	has_texture = true;
	texture_serial = cover.serial;
	return true;
}
} // namespace

void Begin(const Settings &new_settings, const std::vector<std::string> &game_paths)
{
	settings = new_settings;
	selected_serial.clear();
	selection_attempted = false;
	++generation;
	startup_paths = settings.enabled && settings.auto_download && settings.download_on_startup && CoverDownload::Available()
		? game_paths : std::vector<std::string>();
	next_startup = 0;
	startup_retry.clear();
	prefetch_paths.clear();
	next_prefetch = 0;
	// Nowe skanowanie może oznaczać wymianę obrazu pod tą samą nazwą.
	known_serials.clear();
}

std::string KnownSerial(const std::string &game_path)
{
	const auto found = known_serials.find(game_path);
	return found == known_serials.end() ? std::string() : found->second;
}

void Prefetch(const std::vector<std::string> &game_paths)
{
	if (prefetch_paths == game_paths)
		return;
	prefetch_paths.assign(game_paths.begin(), game_paths.begin() + std::min(game_paths.size(), RAM_CACHE_ENTRIES - 2));
	next_prefetch = 0;
}

void Select(const std::string &serial)
{
	const std::string next = settings.enabled && CoverDownload::IsValidSerial(serial) ? serial : std::string();
	if (next != selected_serial) {
		selected_serial = next;
		selection_attempted = false;
		++generation;
		if (worker && !__atomic_load_n(&job->cancelled, __ATOMIC_ACQUIRE)) {
			if (!job->selected && !job->prefetch && !next.empty())
				startup_retry = job->game_path;
			if (job->selected || !next.empty())
				__atomic_store_n(&job->cancelled, true, __ATOMIC_RELEASE);
		}
	}
}

void Update()
{
	if (has_texture && texture_serial != selected_serial && !release_texture(false))
		return;
	// Trafienie w RAM nie czeka na anulowanie trwającego żądania sieciowego.
	if (!selected_serial.empty() && !has_texture) {
		CachedCover *const cached = cached_cover(selected_serial);
		if (cached && upload_cached(*cached))
			selection_attempted = true;
	}
	if (worker) {
		if (!__atomic_load_n(&job->done, __ATOMIC_ACQUIRE))
			return;
		const NTSTATUS status = NtWaitForSingleObjectEx(worker, USER_MODE, 0, nullptr);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("covers: blad oczekiwania na pracownika: 0x%08X", (unsigned)status);
			return;
		}
		NtClose(worker);
		worker = 0;
		if (!job->game_path.empty() && CoverDownload::IsValidSerial(job->serial))
			known_serials[job->game_path] = job->serial;
		if (job->downloaded && job->retry_generation == retry_generation &&
			!__atomic_load_n(&job->cancelled, __ATOMIC_ACQUIRE))
			attempted_downloads.insert(job->serial);
		if (!job->pixels.empty() && (job->selected || job->prefetch))
			remember_cover(*job);
		if (job->selected && job->generation == generation && job->serial == selected_serial &&
			(!has_texture || texture_serial != selected_serial)) {
			CachedCover *const cached = cached_cover(job->serial);
			if (cached)
				upload_cached(*cached);
		} else if (!job->selected && job->serial == selected_serial && !job->pixels.empty()) {
			selection_attempted = false;
		}
		if (!job->selected && job->serial == selected_serial && cached_cover(job->serial) &&
			(!has_texture || texture_serial != selected_serial))
			selection_attempted = false;
		job.reset();
	}
	if (!settings.enabled)
		return;
	if (!selected_serial.empty() && !selection_attempted) {
		CachedCover *const cached = cached_cover(selected_serial);
		if (cached) {
			selection_attempted = upload_cached(*cached);
			return;
		}
		selection_attempted = true;
		start_job(true, selected_serial);
	} else if (next_prefetch < prefetch_paths.size()) {
		const std::string path = prefetch_paths[next_prefetch++];
		const std::string serial = KnownSerial(path);
		if (serial.empty() || !cached_cover(serial))
			start_job(false, path, true);
	} else if (!startup_retry.empty()) {
		const std::string path = std::move(startup_retry);
		startup_retry.clear();
		start_job(false, path);
	} else if (next_startup < startup_paths.size()) {
		start_job(false, startup_paths[next_startup++]);
	}
}

bool Draw(ImDrawList *draw, const ImVec2 &centre, float extent)
{
	if (!has_texture || texture_serial != selected_serial || selected_serial.empty())
		return false;
	const ImVec2 size = fit_size(texture.width, texture.height, extent);
	const ImVec2 half(size.x / 2.0f, size.y / 2.0f);
	draw->AddImage((ImTextureID)(intptr_t)&texture, ImVec2(centre.x - half.x, centre.y - half.y),
		ImVec2(centre.x + half.x, centre.y + half.y));
	drawn = true;
	return true;
}

void AfterRender()
{
	if (drawn) {
		texture_fence = xsdk_gpu_fence();
		drawn = false;
	}
}

void RetryMissing()
{
	// Jedno świadome ponowienie usuwa blokadę nieudanych żądań z tej sesji.
	attempted_downloads.clear();
	++retry_generation;
	selection_attempted = false;
	next_startup = 0;
	startup_retry.clear();
}

void RequestStop()
{
	startup_paths.clear();
	startup_retry.clear();
	prefetch_paths.clear();
	selected_serial.clear();
	if (worker)
		__atomic_store_n(&job->cancelled, true, __ATOMIC_RELEASE);
}

bool TryStop()
{
	if (worker) {
		if (!__atomic_load_n(&job->done, __ATOMIC_ACQUIRE))
			return false;
		const NTSTATUS status = NtWaitForSingleObjectEx(worker, USER_MODE, 0, nullptr);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("covers: nie mozna zakonczyc pracownika: 0x%08X", (unsigned)status);
			return false;
		}
		NtClose(worker);
		worker = 0;
		if (job->downloaded && job->retry_generation == retry_generation &&
			!__atomic_load_n(&job->cancelled, __ATOMIC_ACQUIRE))
			attempted_downloads.insert(job->serial);
		job.reset();
	}
	if (!release_texture(false))
		return false;
	ram_cache.clear();
	return true;
}

bool Stop()
{
	RequestStop();
	if (worker) {
		const NTSTATUS status = NtWaitForSingleObjectEx(worker, USER_MODE, 0, nullptr);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("covers: nie mozna zakonczyc pracownika: 0x%08X", (unsigned)status);
			return false;
		}
		NtClose(worker);
		worker = 0;
		if (job->downloaded)
			attempted_downloads.insert(job->serial);
		job.reset();
	}
	startup_paths.clear();
	selected_serial.clear();
	if (!release_texture(true))
		return false;
	ram_cache.clear();
	return true;
}
} // namespace CoverArt
