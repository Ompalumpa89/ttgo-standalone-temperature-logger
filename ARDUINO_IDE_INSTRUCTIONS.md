# Correct Arduino IDE use

This package is structured so Arduino IDE can compile each version correctly.

Open ONE of these folders in Arduino IDE:

1. Standard version:
   ttgo_temp_logger_standard/ttgo_temp_logger_standard.ino

2. Powerbank version:
   ttgo_temp_logger_powerbank/ttgo_temp_logger_powerbank.ino

Do not copy README text, GitHub text, file paths or ZIP contents into an Arduino sketch.

Do not place both .ino files in the same Arduino sketch folder. Arduino IDE compiles all .ino files in the open sketch folder together.
