# Linux / macOS / MSYS2 build. On Windows with Visual Studio use CMakeLists.txt.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-label
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

COMMON = src/bios.c src/font8x8.c src/image.c

all: build/paratrooper

build/paratrooper: src/game.c src/main_sdl.c $(COMMON) src/cpu.h src/machine.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ src/game.c src/main_sdl.c $(COMMON) $(SDL_LIBS)

# Verification builds (see tools/verify.py)
build/paratrooper_test: src/game.c src/main_test.c $(COMMON) src/cpu.h src/machine.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/game.c src/main_test.c $(COMMON)

build/libbios_harness.so: src/main_test.c $(COMMON) src/cpu.h src/machine.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DHARNESS -shared -fPIC -o $@ src/main_test.c $(COMMON)

# Regenerate game.c / image.c from the original binary
translate: original/PARA.COM tools/translate.py
	python3 tools/translate.py original/PARA.COM src

clean:
	rm -rf build
