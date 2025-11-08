# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "bootloader\\bootloader.bin"
  "bootloader\\bootloader.elf"
  "bootloader\\bootloader.map"
  "config\\sdkconfig.cmake"
  "config\\sdkconfig.h"
  "decoded_3826495.webp.S"
  "esp-idf\\esptool_py\\flasher_args.json.in"
  "esp-idf\\mbedtls\\x509_crt_bundle"
  "flash_app_args"
  "flash_bootloader_args"
  "flash_project_args"
  "flasher_args.json"
  "idea11-ionic-connections-128p-L92-b809.webp.S"
  "ldgen_libraries"
  "ldgen_libraries.in"
  "ms-black-dragon-64p-L82.webp.S"
  "p3a.bin"
  "p3a.map"
  "project_elf_src_esp32p4.c"
  "r3-artificer-32p-L55.webp.S"
  "x509_crt_bundle.S"
  )
endif()
