cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_asha)
target_sources(app PRIVATE src/main.c src/indicator.c src/audio.c src/asha.c src/packet_timing.c codec/g722_encode.c)
target_sources_ifdef(CONFIG_USB_ASHA_SONNET3 app PRIVATE src/sonnet_mix.c)
target_include_directories(app PRIVATE codec codec/include)

# Two independent G.722 encoders must finish in a single 20 ms connection interval.
set_source_files_properties(codec/g722_encode.c src/audio.c PROPERTIES
  COMPILE_OPTIONS "-O3;-funroll-loops;-fno-tree-loop-distribute-patterns")

if(CONFIG_USB_ASHA_SONNET3)
  zephyr_code_relocate(FILES codec/g722_encode.c LOCATION DSPCODE_TEXT)
  zephyr_code_relocate(FILES codec/g722_encode.c LOCATION RAM_RODATA)
  zephyr_code_relocate(FILES src/audio.c FILTER ".*taps.*" LOCATION RAM_RODATA)
  zephyr_code_relocate(FILES src/audio.c FILTER ".*sonnet_decimate.*" LOCATION DSPCODE_TEXT)
endif()
