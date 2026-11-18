#include "cover_download.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>
#include <fstream>
#include <iterator>

namespace
{
void Require(bool condition, const char* message)
{
  if (!condition) {
    fprintf(stderr, "BLAD: %s\n", message);
    exit(EXIT_FAILURE);
  }
}
} // namespace

int main(int argc, char** argv)
{
  for (const char* serial : {"SLUS-00402", "SCES-02105", "SLPS-00001", "SLES-00001"})
    Require(CoverDownload::IsValidSerial(serial), "prawidlowy serial odrzucony");

  for (const char* serial : {"", "../SLUS-1", "SLUS-00402/../x", "SLUS-00402\r\n", "SLUS%00402", "slus-00402",
         "SLUS-0040", "SLUS-004020", "SLUS-00A02", "SLU/-00402", "SLUS_00402"})
    Require(!CoverDownload::IsValidSerial(serial), "nieprawidlowy serial zaakceptowany");
  Require(!CoverDownload::IsValidSerial(std::string("SLUS-00402\0x", 12)), "serial z NUL zaakceptowany");
  Require(CoverDownload::FallbackTitle("SLUS-00402", "SLUS-00402\tTekken 3 (USA).png\n") ==
      "Tekken 3 (USA).png", "brak dokladnego dopasowania TSV");
  Require(CoverDownload::FallbackTitle("SLUS-00402", "SLUS-00402\tTekken 3 (USA).png\n"
      "SLUS-00402\tInna edycja.png\n").empty(), "zaakceptowano niejednoznaczny serial");
  Require(CoverDownload::FallbackTitle("SLUS-00402", "SLUS-00402\t../okladka.png\n").empty(),
      "zaakceptowano sciezke w mapie");
  Require(CoverDownload::FallbackTitle("SLUS-00402", "SLUS-00402\tGra.jpg\n").empty(),
      "zaakceptowano niewlasciwy format mapy");
  Require(CoverDownload::FallbackTitle("SLUS-00402", std::string(2 * 1024 * 1024 + 1, 'x')).empty(),
      "zaakceptowano zbyt duza mape");
  Require(CoverDownload::FallbackTitle("SLUS-00402", "SLUS-00402\tGra.png\tdodatek\n").empty(),
      "zaakceptowano dodatkowa kolumne");
  Require(argc == 2, "podaj plik mapy do sprawdzenia");
  std::ifstream file(argv[1], std::ios::binary);
  Require(file.good(), "brak mapy wydania");
  const std::string mapping((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  Require(CoverDownload::FallbackTitle("SLUS-00402", mapping) == "Tekken 3 (USA).png", "zla mapa Tekken 3");
  Require(CoverDownload::FallbackTitle("SCUS-94900", mapping) == "Crash Bandicoot (USA).png", "zla mapa Crash");
  Require(CoverDownload::FallbackTitle("SCUS-94503", mapping).empty(), "nie usunieto niejednoznacznego serialu");
  Require(CoverDownload::FallbackTitle("SLPS-00830", mapping).empty(), "nie usunieto niejednoznacznej rewizji");

  // Wariant bez bibliotek sieciowych musi zachować działanie offline bez pozornych danych.
  Require(!CoverDownload::Available(), "test wymaga kompilacji bez HTTPS");
  std::vector<unsigned char> bytes{1, 2, 3};
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes), "wariant offline zwrocil okladke");
  Require(bytes.empty(), "pozostawiono stare dane po bledzie pobierania");
  Require(!CoverDownload::FetchBytes("../x", &bytes), "sciezka przeszla walidacje");
  Require(!CoverDownload::FetchBytes("SLUS-00402", nullptr), "zaakceptowano pusty wskaznik");
  bool cancelled = true;
  bytes = {1, 2};
  Require(!CoverDownload::FetchBytes("SLUS-00402", &bytes, true, &cancelled), "zignorowano anulowanie");
  Require(bytes.empty(), "anulowanie pozostawilo stare dane");
  puts("OK: seriale, jednoznaczna mapa Libretro, odrzucanie sciezek i tryb offline");
  return EXIT_SUCCESS;
}
