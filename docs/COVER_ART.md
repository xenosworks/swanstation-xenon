# Okładki gier w porcie Xbox 360

## Zachowanie

Panel płyty w pickerze wyświetla lokalną okładkę zaznaczonej gry. Lista, istniejące
przypisania pada oraz menu pauzy zachowują swoje działanie. Brak prawidłowej grafiki
pozostawia dotychczasową płytę CD. START, wcześniej nieużywany w pickerze, ponawia
pobieranie brakujących okładek.

Identyfikator jest odczytywany przez istniejące `CDImage::Open` i
`System::GetGameCodeForImage`, także dla CHD. Nie modyfikujemy obrazów gier
i nie dopasowujemy niejednoznacznych nazw na podstawie podobieństwa.
Skan przy wejściu do listy wykrywa nowe gry.

## Cache i płynność

Pliki są przechowywane w `game:\covers\<SERIAL>.jpg` lub `.png`. Limit pliku
wynosi 2 MiB, a obrazu 2048×2048. Wymiary są sprawdzane przed dekodowaniem;
poprawnie zdekodowany obraz pobrany przez HTTPS jest zapisywany przez plik tymczasowy.

Worker obsługuje jedno zadanie naraz, priorytet ma bieżące zaznaczenie. Znany
serial nie czeka ponownie 15 klatek. Sześć podglądów do 512×512 w RAM zajmuje
najwyżej 6 MiB danych pikseli, a cztery sąsiednie okładki są przygotowywane
z lokalnego dysku. Prefetch nie pobiera dodatkowych grafik z sieci.
Trafienie w RAM nie czeka na kończące się żądanie HTTP.

Nieaktualny wynik nie zmienia grafiki nowej gry. Pozostaje pojedyncza tekstura GPU,
zwalniana po fence; worker i RAM kończą pracę przed uruchomieniem emulacji.
Pierwsza nieznana gra lub skok poza bufor może nadal wymagać odczytu i dekodowania.
Nowe skanowanie unieważnia indeks seriali, aby nie użyć starego wyniku po wymianie pliku.

## HTTPS i źródła

Podstawowe źródło to PSX Covers. Wyłącznie po HTTP 404 można użyć Libretro,
ze sprawdzoną mapą 8710 dokładnych powiązań serial–nazwa. Wszystkie 265 seriali
o wielu różnych tytułach zostały pominięte. Mapa pozostaje osobnym plikiem
CC-BY-SA-4.0, z atrybucją i przypiętymi źródłami w `data/cover_fallback.source.json`.
Obrazy okładek nie są dołączane do źródeł ani do proponowanej zmiany.

Wymagane są publiczne porty curl-xenon i mbedtls-xenon oraz poprawny zegar konsoli.
Sprawdzane są certyfikaty i nazwa hosta; URL i przekierowania pozostają HTTPS.
Oba źródła dzielą osiem sekund, a worker ogranicza pobieranie do jednego żądania
na sekundę. Błąd TLS, brak CA lub sieci pozostawia lokalną okładkę albo CD.
Szczegóły wersji i licencji bibliotek: [COVER_DEPENDENCIES.md](COVER_DEPENDENCIES.md).

## Kompilacja

Istniejąca konfiguracja xsdk i imgui-xenon pozostaje wymagana. Dodatkowo:

```bash
set -euo pipefail
cmake -S . -B build-xenon -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$XSDK_DIR/cmake/xsdk.cmake" \
  -DXSDK_TOOLCHAIN="$XSDK_TOOLCHAIN" \
  -DIMGUI_XENON_DIR="$IMGUI_XENON_DIR" \
  -DCURL_XENON_DIR="$CURL_XENON_DIR" \
  -DMBEDTLS_XENON_DIR="$MBEDTLS_XENON_DIR" \
  -DCMAKE_BUILD_TYPE=Release -DCOVERART_HTTPS=ON
cmake --build build-xenon --target swanstation
```

Zmienne wskazują lokalne checkouty zależności. W Mbed TLS należy zainicjować
submoduł `framework`. Bez obu bibliotek lub przy `COVERART_HTTPS=OFF` działają
wyłącznie lokalne okładki. CMake kopiuje CA i pliki mapy obok XEX; trzeba je
przenieść razem z aplikacją. Zmiana nie narzuca nowej wersji, nazwy ani Title ID.

## Ustawienia

Istniejący płaski `settings.txt` jest rozszerzony o:

```text
cover_enabled=1
cover_auto_download=1
cover_download_on_startup=1
cover_fallback_source=1
```

Zapis trybu analogowego zachowuje nieznane linie i nowe opcje. `games_root`
opcjonalnie wskazuje katalog gier; domyślnie pozostaje `game:\games`.
`bios_directory` w `host.txt` opcjonalnie wskazuje katalog odczytu BIOS-u.
Nie zmienia katalogu systemowego ani `game:\saves`, więc cache i nowe zapisy
nie trafiają do cudzej instalacji. Testować w osobnym folderze, zachowując oryginał.

## Testy i granice potwierdzenia

```bash
set -euo pipefail
bash tests/test_cover_art.sh
SANITIZE=1 bash tests/test_cover_art.sh
bash tests/xenon/test_cover_download.sh
```

Skrypty przyjmują `XSDK_DIR`, `IMGUI_XENON_DIR`, `CURL_XENON_DIR` i `CXX`.
Bez tych zmiennych używają checkoutów w `work/`. Wymagane są G++ z C++17 i Python 3.
Dodany workflow wykonuje te testy na hoście bez konsoli i bez komercyjnego SDK.

Testy obejmują JPG/PNG, uszkodzenia i limity, cache offline, proporcje,
anulowanie, generacje, fence GPU, bufor RAM, lokalny prefetch oraz zachowanie
ustawień. Transport testowy sprawdza certyfikaty/host, HTTPS, fallback wyłącznie
po 404, niejednoznaczność, flagę wyłączenia i wspólny limit czasu.

Na rzeczywistym Xbox 360 RGH/Aurora potwierdzono start, listę gier, pobranie
i zachowanie 230 okładek po zamknięciu aplikacji. Pobranie czterech próbek
potwierdziło CRC32. Ostatnia poprawka bufora RAM przeszła testy hosta i kompilację
PowerPC, lecz ocena jej płynności na konsoli pozostaje niepotwierdzona.
Rozgrywka, karty pamięci, długa sesja i restart offline wymagają dalszych testów.
