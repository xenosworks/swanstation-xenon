#include "cover_download.h"

#include <stdio.h>
#include <algorithm>

#ifdef SWANSTATION_COVER_HTTPS
#include <cstring>
#include <cstdint>
#include <limits>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include <curl/curl.h>

#include "xsdk/log.h"
#endif

namespace CoverDownload
{
namespace
{
constexpr size_t MAX_MAPPING_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_MAPPING_LINE_BYTES = 1024;
#ifdef SWANSTATION_COVER_HTTPS
constexpr size_t MAX_COVER_BYTES = 2 * 1024 * 1024;
constexpr long CONNECT_TIMEOUT_MS = 5000;
constexpr long TRANSFER_TIMEOUT_MS = 8000;
constexpr long MAX_REDIRECTS = 2;
constexpr char CA_PATH[] = "game:\\cacert.pem";
constexpr char COVER_BASE_URL[] = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/";
constexpr char FALLBACK_MAP_PATH[] = "game:\\cover_fallback.tsv";
// Pin odpowiada liście istniejących nazw sprawdzonej podczas generowania mapy.
constexpr char FALLBACK_BASE_URL[] = "https://raw.githubusercontent.com/libretro-thumbnails/"
  "Sony_-_PlayStation/ccee75c7744d81676b6725307aca27ef6be6231a/Named_Boxarts/";

// Jeden właściciel globalnego stanu curl zapobiega cleanup w trakcie innego pobierania.
pthread_mutex_t curl_mutex = PTHREAD_MUTEX_INITIALIZER;

class CurlSession
{
public:
  CurlSession()
  {
    locked = pthread_mutex_lock(&curl_mutex) == 0;
    if (locked) {
      initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
      if (initialized)
        handle = curl_easy_init();
    }
  }

  ~CurlSession()
  {
    if (handle)
      curl_easy_cleanup(handle);
    if (initialized)
      curl_global_cleanup();
    if (locked)
      pthread_mutex_unlock(&curl_mutex);
  }

  CurlSession(const CurlSession&) = delete;
  CurlSession& operator=(const CurlSession&) = delete;
  CURL* handle = nullptr;

private:
  bool locked = false;
  bool initialized = false;
};

struct ReceiveBuffer
{
  std::vector<unsigned char>* bytes;
  size_t received = 0;
};

size_t ReceiveBody(char* data, size_t size, size_t count, void* user)
{
  ReceiveBuffer* buffer = static_cast<ReceiveBuffer*>(user);
  if (size && count > std::numeric_limits<size_t>::max() / size)
    return 0;
  const size_t length = size * count;
  if (length > MAX_COVER_BYTES - buffer->received)
    return 0;
  // Bufor ma stały limit, dlatego callback nie alokuje pamięci ani nie rzuca wyjątków.
  std::memcpy(buffer->bytes->data() + buffer->received, data, length);
  buffer->received += length;
  return length;
}

int CheckCancellation(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  const bool* cancel = static_cast<const bool*>(user);
  return cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int64_t MonotonicMilliseconds()
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

std::string ReadFallbackTitle(const std::string& serial)
{
  FILE* file = fopen(FALLBACK_MAP_PATH, "rb");
  if (!file)
    return {};
  struct stat information;
  if (fstat(fileno(file), &information) != 0 || information.st_size <= 0 ||
      static_cast<uint64_t>(information.st_size) > MAX_MAPPING_BYTES) {
    fclose(file);
    return {};
  }
  std::string mapping(static_cast<size_t>(information.st_size), '\0');
  const bool complete = fread(&mapping[0], 1, mapping.size(), file) == mapping.size() && fgetc(file) == EOF;
  const bool closed = fclose(file) == 0;
  return complete && closed ? FallbackTitle(serial, mapping) : std::string();
}
#endif
} // namespace

bool IsValidSerial(const std::string& serial)
{
  if (serial.size() != 10 || serial[4] != '-')
    return false;
  for (size_t index = 0; index < 4; ++index) {
    if (serial[index] < 'A' || serial[index] > 'Z')
      return false;
  }
  for (size_t index = 5; index < serial.size(); ++index) {
    if (serial[index] < '0' || serial[index] > '9')
      return false;
  }
  return true;
}

std::string FallbackTitle(const std::string& serial, const std::string& mapping)
{
  if (!IsValidSerial(serial) || mapping.size() > MAX_MAPPING_BYTES)
    return {};
  std::string matched;
  size_t position = 0;
  while (position < mapping.size()) {
    const size_t newline = mapping.find('\n', position);
    const size_t end = newline == std::string::npos ? mapping.size() : newline;
    if (end - position > MAX_MAPPING_LINE_BYTES)
      return {};
    const std::string line = mapping.substr(position, end - position);
    const size_t separator = line.find('\t');
    if (separator != 10 || line.find('\t', separator + 1) != std::string::npos ||
        !IsValidSerial(line.substr(0, separator)))
      return {};
    const std::string title = line.substr(separator + 1);
    if (title.size() <= 4 || title.compare(title.size() - 4, 4, ".png") != 0)
      return {};
    for (unsigned char character : title) {
      if (character < 32 || character == 127 || character == '/' || character == '\\')
        return {};
    }
    if (line.compare(0, separator, serial) == 0) {
      if (!matched.empty() && matched != title)
        return {};
      matched = title;
    }
    position = newline == std::string::npos ? mapping.size() : newline + 1;
  }
  return matched;
}

bool Available()
{
#ifdef SWANSTATION_COVER_HTTPS
  FILE* file = fopen(CA_PATH, "rb");
  if (!file)
    return false;
  const bool readable = fgetc(file) != EOF;
  const bool closed = fclose(file) == 0;
  return readable && closed;
#else
  return false;
#endif
}

bool FetchBytes(const std::string& serial, std::vector<unsigned char>* bytes, bool use_fallback, const bool* cancel)
{
  if (!bytes)
    return false;
  bytes->clear();
  if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE))
    return false;
  if (!IsValidSerial(serial) || !Available())
    return false;
#ifdef SWANSTATION_COVER_HTTPS
  const int64_t started = MonotonicMilliseconds();
  if (started < 0)
    return false;
  CurlSession session;
  if (!session.handle) {
    xsdk_log_write("okladki: inicjalizacja HTTPS nie powiodla sie");
    return false;
  }
  bytes->resize(MAX_COVER_BYTES);
  ReceiveBuffer buffer{bytes};
  const std::string url = std::string(COVER_BASE_URL) + serial + ".jpg";
  CURL* curl = session.handle;
  // Nazwa hosta i łańcuch certyfikatów są sprawdzane także po przekierowaniu.
  const bool configured =
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, MAX_REDIRECTS) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TRANSFER_TIMEOUT_MS) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(MAX_COVER_BYTES)) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CheckCancellation) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ReceiveBody) == CURLE_OK &&
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer) == CURLE_OK;
  const int64_t initial_now = MonotonicMilliseconds();
  const int64_t initial_remaining = initial_now >= started ? TRANSFER_TIMEOUT_MS - (initial_now - started) : 0;
  const bool initial_budget = initial_remaining > 0 &&
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(initial_remaining)) == CURLE_OK;
  CURLcode result = configured && initial_budget ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
  long status = 0;
  bool got_status = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK;
  if (use_fallback && got_status && status == 404 && result == CURLE_HTTP_RETURNED_ERROR &&
      !(cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE))) {
    // Flaga false pomija również odczyt mapy, a błąd TLS nigdy nie zmienia źródła.
    const std::string title = ReadFallbackTitle(serial);
    const int64_t now = MonotonicMilliseconds();
    const int64_t remaining = now >= started ? TRANSFER_TIMEOUT_MS - (now - started) : 0;
    if (!title.empty() && remaining > 0) {
      char* encoded = curl_easy_escape(curl, title.c_str(), static_cast<int>(title.size()));
      if (encoded) {
        const std::string fallback_url = std::string(FALLBACK_BASE_URL) + encoded;
        curl_free(encoded);
        buffer.received = 0;
        const long timeout = static_cast<long>(remaining);
        const bool fallback_configured =
          curl_easy_setopt(curl, CURLOPT_URL, fallback_url.c_str()) == CURLE_OK &&
          curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(CONNECT_TIMEOUT_MS, timeout)) == CURLE_OK &&
          curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout) == CURLE_OK;
        result = fallback_configured ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
        got_status = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK;
      }
    }
  }
  if (result != CURLE_OK || !got_status || status != 200 || buffer.received == 0 ||
      (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE))) {
    xsdk_log_write("okladki: %s: HTTPS %s, HTTP %ld", serial.c_str(), curl_easy_strerror(result), status);
    bytes->clear();
    return false;
  }
  bytes->resize(buffer.received);
  return true;
#else
  (void)use_fallback;
  return false;
#endif
}
} // namespace CoverDownload
