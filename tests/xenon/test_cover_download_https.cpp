// Zastępuje transport curl w teście; nie łączy się z siecią ani z konsolą.
#include "cover_download.h"

#include <curl/curl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#undef curl_easy_setopt
#undef curl_easy_getinfo

namespace
{
struct FakeTransport
{
  CURLcode primary_result = CURLE_HTTP_RETURNED_ERROR;
  long primary_status = 404;
  long status = 0;
  long timeout = 0;
  long peer = 0;
  long host = 0;
  int calls = 0;
  int escapes = 0;
  std::string url;
  std::string protocols;
  std::string redirects;
  std::string ca;
  std::vector<std::string> urls;
  std::vector<long> timeouts;
  curl_write_callback writer = nullptr;
  void* body = nullptr;
  curl_xferinfo_callback progress = nullptr;
  void* progress_data = nullptr;
} fake;

void Require(bool condition, const char* message)
{
  if (!condition) {
    fprintf(stderr, "BLAD HTTPS: %s\n", message);
    exit(EXIT_FAILURE);
  }
}

void WriteFile(const char* name, const char* contents)
{
  FILE* file = fopen(name, "wb");
  Require(file != nullptr, "nie mozna utworzyc pliku testowego");
  Require(fwrite(contents, 1, strlen(contents), file) == strlen(contents), "blad zapisu testu");
  Require(fclose(file) == 0, "blad zamkniecia testu");
}

void Reset()
{
  fake = FakeTransport{};
}
} // namespace

extern "C" {
CURLcode curl_global_init(long) { return CURLE_OK; }
void curl_global_cleanup() {}
CURL* curl_easy_init() { return reinterpret_cast<CURL*>(&fake); }
void curl_easy_cleanup(CURL*) {}
void xsdk_log_write(const char*, ...) {}
const char* curl_easy_strerror(CURLcode) { return "testowy transport"; }
void curl_free(void* pointer) { free(pointer); }

char* curl_easy_escape(CURL*, const char* text, int length)
{
  ++fake.escapes;
  std::string encoded;
  constexpr char HEX[] = "0123456789ABCDEF";
  for (int index = 0; index < length; ++index) {
    const unsigned char value = static_cast<unsigned char>(text[index]);
    if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.') {
      encoded.push_back(static_cast<char>(value));
    } else {
      encoded.push_back('%');
      encoded.push_back(HEX[value >> 4]);
      encoded.push_back(HEX[value & 15]);
    }
  }
  char* result = static_cast<char*>(malloc(encoded.size() + 1));
  Require(result != nullptr, "brak pamieci testu");
  memcpy(result, encoded.c_str(), encoded.size() + 1);
  return result;
}

CURLcode curl_easy_setopt(CURL*, CURLoption option, ...)
{
  va_list arguments;
  va_start(arguments, option);
  switch (option) {
    case CURLOPT_URL: fake.url = va_arg(arguments, const char*); break;
    case CURLOPT_PROTOCOLS_STR: fake.protocols = va_arg(arguments, const char*); break;
    case CURLOPT_REDIR_PROTOCOLS_STR: fake.redirects = va_arg(arguments, const char*); break;
    case CURLOPT_CAINFO: fake.ca = va_arg(arguments, const char*); break;
    case CURLOPT_TIMEOUT_MS: fake.timeout = va_arg(arguments, long); break;
    case CURLOPT_SSL_VERIFYPEER: fake.peer = va_arg(arguments, long); break;
    case CURLOPT_SSL_VERIFYHOST: fake.host = va_arg(arguments, long); break;
    case CURLOPT_WRITEFUNCTION: fake.writer = va_arg(arguments, curl_write_callback); break;
    case CURLOPT_WRITEDATA: fake.body = va_arg(arguments, void*); break;
    case CURLOPT_XFERINFOFUNCTION: fake.progress = va_arg(arguments, curl_xferinfo_callback); break;
    case CURLOPT_XFERINFODATA: fake.progress_data = va_arg(arguments, void*); break;
    default: break;
  }
  va_end(arguments);
  return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL*, CURLINFO information, ...)
{
  Require(information == CURLINFO_RESPONSE_CODE, "nieoczekiwane zapytanie testu");
  va_list arguments;
  va_start(arguments, information);
  *va_arg(arguments, long*) = fake.status;
  va_end(arguments);
  return CURLE_OK;
}

CURLcode curl_easy_perform(CURL*)
{
  Require(fake.peer == 1 && fake.host == 2, "wylaczona weryfikacja TLS");
  Require(fake.protocols == "https" && fake.redirects == "https", "dozwolono protokol inny niz HTTPS");
  Require(fake.ca == "game:\\cacert.pem", "brak ustalonego CA");
  Require(fake.timeout > 0 && fake.timeout <= 8000, "przekroczono budzet czasu");
  fake.urls.push_back(fake.url);
  fake.timeouts.push_back(fake.timeout);
  ++fake.calls;
  if (fake.progress && fake.progress(fake.progress_data, 0, 0, 0, 0))
    return CURLE_ABORTED_BY_CALLBACK;
  if (fake.calls == 1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    fake.status = fake.primary_status;
    return fake.primary_result;
  }
  fake.status = 200;
  char payload[] = "obraz testowy";
  return fake.writer(payload, 1, sizeof payload, fake.body) == sizeof payload ? CURLE_OK : CURLE_WRITE_ERROR;
}
} // extern "C"

int main()
{
  WriteFile("game:\\cacert.pem", "CA testu transportu");
  WriteFile("game:\\cover_fallback.tsv", "SLUS-00402\tTekken 3 (USA).png\n");
  std::vector<unsigned char> bytes;

  Require(CoverDownload::FetchBytes("SLUS-00402", &bytes, true), "brak fallbacku po 404");
  Require(fake.calls == 2 && fake.escapes == 1, "nie wykonano dwoch zrodel i kodowania URL");
  Require(fake.urls[1].find("/Named_Boxarts/Tekken%203%20%28USA%29.png") != std::string::npos,
      "fallback nie uzywa dokladnego zakodowanego tytulu");
  Require(fake.timeouts[1] < fake.timeouts[0], "drugi transfer dostal nowy pelny budzet czasu");
  Require(!bytes.empty(), "nie zwrocono wyniku zapasowego zrodla");

  Reset();
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes, false), "zignorowano wylaczenie fallbacku");
  Require(fake.calls == 1 && fake.escapes == 0 && bytes.empty(), "flaga false uruchomila fallback");

  Reset();
  fake.primary_result = CURLE_PEER_FAILED_VERIFICATION;
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes, true), "blad certyfikatu zmienil zrodlo");
  Require(fake.calls == 1, "fallback uruchomiony po bledzie TLS");

  Reset();
  fake.primary_status = 500;
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes, true), "HTTP500 zmienil zrodlo");
  Require(fake.calls == 1, "fallback uruchomiony bez HTTP404");

  Reset();
  WriteFile("game:\\cover_fallback.tsv", "SLUS-00402\tTekken 3 (USA).png\nSLUS-00402\tInna edycja.png\n");
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes, true), "zgadnieto niejednoznaczna okladke");
  Require(fake.calls == 1, "niejednoznacznosc spowodowala pobieranie");

  Reset();
  bool cancelled = true;
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes, true, &cancelled), "zignorowano anulowanie");
  Require(fake.calls == 0, "anulowana operacja dotarla do transportu");

  Require(remove("game:\\cacert.pem") == 0, "nie usunieto CA testu");
  Require(remove("game:\\cover_fallback.tsv") == 0, "nie usunieto mapy testu");
  puts("OK: TLS, HTTPS, fallback tylko po 404, flaga, niejednoznacznosc, wspolny budzet i anulowanie");
  return EXIT_SUCCESS;
}
