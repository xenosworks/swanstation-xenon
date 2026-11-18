# Zależności pobierania okładek

Stan weryfikacji: 17.11.2026. Repozytoria zależności są lokalnie w ignorowanym `work/`.

| Zależność | Wersja / commit | Źródło |
| --- | --- | --- |
| curl-xenon | curl 8.23.0, `a6b70bd06820e1c625f47fe0e592e08c5daaa6b4` | https://github.com/xenosworks/curl-xenon |
| mbedtls-xenon | Mbed TLS 3.6.7, `591b04adbec4bc5582649601f4407506a610162b` | https://github.com/xenosworks/mbedtls-xenon |
| mbedtls-framework | `dde0c4a0e448a0552f18817dcea633bb851fd288` | submoduł mbedtls-xenon |

Budowanie wymaga poprawionego toolchaina xsdk dla PowerPC, w szczególności poprawki utrzymującej LR w 32 bitach. Port używa TLS 1.2 oraz IPv4. Nie wymaga własnościowego XDK Microsoftu. Szczegóły: `docs/README-xsdk.md` w obu zależnościach.

## Konfiguracja kompilacji

Domyślne `-DCOVERART_HTTPS=ON` dołącza HTTPS, jeśli istnieją obie zależności. Ścieżki można podać przez `-DCURL_XENON_DIR=...` oraz `-DMBEDTLS_XENON_DIR=...`. Brak zależności lub `-DCOVERART_HTTPS=OFF` pozostawia obsługę lokalnych okładek. W tym wariancie kod nie deklaruje możliwości automatycznego pobierania.

Mbed TLS wymaga zainicjowanego submodułu `framework`. Aby odtworzyć analizowany zestaw, pobierz wymienione repozytoria do `work/`, przełącz każde na commit z tabeli, a w Mbed TLS wykonaj `git submodule update --init --recursive`.

CMake kopiuje `mbedtls-xenon/programs/xsdk/cacert.pem` obok budowanego XEX. Ten plik trzeba dołączyć do paczki i umieścić obok `swanstation.xex` na konsoli. Jest to zbiór CA Mozilla dystrybuowany przez curl, opisany w dokumentacji portu jako stan z 25.09.2026, na MPL 2.0. Należy zachować nagłówek i informacje o pochodzeniu pliku. Bez CA lub przy nieprawidłowym zegarze konsoli pobieranie pozostaje niedostępne albo kończy się błędem weryfikacji, bez blokowania gry.

## Zachowanie modułu

`CoverDownload::FetchBytes()` obsługuje wyłącznie identyfikator w formacie cztery wielkie litery, myślnik i pięć cyfr. Odrzuca ścieżki, sekwencje `..`, NUL oraz znaki sterujące. Pobiera najwyżej 2 MiB z PSX Covers, ze sprawdzaniem hosta i łańcucha certyfikatów. Połączenie ma limit 5 sekund, cała operacja 8 sekund; przekierowania są ograniczone do dwóch i wyłącznie do HTTPS. Moduł nie zapisuje plików na dysku. Walidacja obrazu, zapis tymczasowy i publikacja należą do `CoverArt`.

Moduł serializuje inicjalizację, użycie i zwalnianie curl. Błąd, brak CA lub brak bibliotek daje pusty wynik; worker nie ponawia automatycznie tej samej gry w pętli. Wyłącznie HTTP 404 źródła podstawowego pozwala sprawdzić źródło zapasowe. Oba transfery dzielą jeden budżet ośmiu sekund; fallback nie dostaje nowego pełnego timeoutu.

Callback postępu odczytuje atomową flagę anulowania i przerywa transfer, gdy użytkownik uruchamia grę albo zamyka picker. Pętla UI czeka nieblokująco na zakończenie workera; samo anulowanie może wymagać najbliższego wywołania callbacku curl.

## Źródła obrazów i ograniczenia

Źródło podstawowe: `https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/<SERIAL>.jpg`, zgodnie z [instrukcją PSX Covers](https://github.com/xlenore/psx-covers).

[Libretro PlayStation](https://github.com/libretro-thumbnails/Sony_-_PlayStation/tree/master/Named_Boxarts) używa pełnych tytułów PNG, z regionem i edycją. Plik `data/cover_fallback.tsv` zawiera 8710 jednoznacznych powiązań serial–nazwa PNG. Powstał z bazy [libretro-database](https://github.com/libretro/libretro-database), `metadat/redump/Sony - PlayStation.dat`, wersja 2026.11.14, przez dokładne przecięcie nazw z istniejącymi plikami `Named_Boxarts`, po wymaganej przez Libretro zamianie niedozwolonych znaków na `_`. Nie ma dopasowania przybliżonego ani zmiany wielkości liter.

Odrzucono wszystkie 265 seriali, dla których baza wskazuje więcej niż jeden różny tytuł, również różne rewizje tego samego tytułu. 1361 pozostałych jednoznacznych seriali nie znalazło dokładnej nazwy grafiki i również nie trafiło do mapy. Przykładowo `SCUS-94503` wskazuje w bazie dwa tytuły i nie będzie zgadywany. Osobne numery dysków pozostają częścią pełnej nazwy; regionów ani wydań nie łączymy.

Przy `cover_fallback_source=0` mapa nie jest nawet otwierana. Przy włączonej opcji mapa jest czytana wyłącznie w workerze, z limitem 2 MiB i 1024 bajtów na linię. Parser odrzuca niepoprawne wpisy, ścieżki i duplikat serialu o różnych tytułach. Nazwa obrazu jest kodowana jako pojedynczy segment URL przez `curl_easy_escape()`. Fallback korzysta z przypiętej rewizji repozytorium grafik; błąd TLS, timeout ani HTTP 500 źródła podstawowego nie uruchamiają fallbacku.

Metadane mapy pozostają osobnym zasobem CC-BY-SA-4.0, zgodnie z licencją repozytorium bazy. Licencja znajduje się w `docs/licenses/libretro-metadata.txt`; źródła, atrybucja, opis ekstrakcji, commity obu repozytoriów i SHA-256 są w `data/cover_fallback.source.json`. Kompilacja i paczka muszą kopiować oba pliki obok XEX jako `cover_fallback.tsv` oraz `cover_fallback.source.json`. Nie należy przypisywać im licencji GPL kodu ani traktować tej licencji jako licencji grafik.

Sprawdzenie publicznych endpointów GitHub `/license` dla obu baz grafik zwróciło 404. Nie potwierdzono ogólnej licencji na redystrybucję kolekcji; paczka aplikacji nie zawiera tych obrazów. Licencje curl i Mbed TLS dotyczą bibliotek, a nie okładek gier.

## Sprawdzenie na komputerze

Uruchom `bash tests/xenon/test_cover_download.sh` w Linux/WSL z G++ obsługującym C++17 oraz lokalnymi nagłówkami curl-xenon i xsdk. Test sprawdza seriale, odrzucanie ścieżek, mapę wydania, odrzucanie niejednoznaczności i tryb bez sieci. Drugi test zastępuje transport curl, aby sprawdzić HTTPS, weryfikację peer/host, fallback wyłącznie po HTTP 404, wyłączenie flagą, wspólny timeout i anulowanie. Żaden test nie nawiązuje połączeń. Nie potwierdza to komunikacji HTTPS na Xboxie; ten test wymaga uruchomienia prawdziwego XEX z poprawnym CA i zegarem.
