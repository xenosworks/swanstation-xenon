#pragma once

#include <string>
#include <vector>

namespace CoverDownload
{
// Bez bibliotek HTTPS lub pliku CA pobieranie pozostaje wyłączone.
bool Available();
bool IsValidSerial(const std::string& serial);
// Zwraca wyłącznie jednoznaczną nazwę PNG z ograniczonej mapy TSV, bez dopasowania przybliżonego.
std::string FallbackTitle(const std::string& serial, const std::string& mapping);

// Wynik wymaga walidacji i dekodowania przed zapisaniem w katalogu covers.
// Libretro jest sprawdzane wyłącznie po HTTP 404 źródła podstawowego i przy use_fallback.
// Flaga anulowania jest współdzielona z workerem przez operacje __atomic.
bool FetchBytes(const std::string& serial, std::vector<unsigned char>* bytes, bool use_fallback = true,
  const bool* cancel = nullptr);
} // namespace CoverDownload
