SHELL := /bin/bash
.DEFAULT_GOAL := help

ENV ?= amoled
PIO ?= python3 -m platformio
PORT ?= auto
MONITOR ?= 0
VERSION ?=
DEV := ENV=$(ENV) PORT=$(PORT) VERSION=$(VERSION)

FLASH_ARGS := -e $(ENV)
ifneq ($(PORT),auto)
FLASH_ARGS += -p $(PORT)
endif
ifeq ($(MONITOR),1)
FLASH_ARGS += -m
endif
ifneq ($(strip $(VERSION)),)
FLASH_ARGS += -v $(VERSION)
VERSION_ENV := RSVP_FIRMWARE_VERSION=$(VERSION)
endif

.PHONY: help menu build build-amoled build-bar test flash flash-monitor rsvp monitor ports clean export-web release-check flash-bringup merge-bringup

menu:
	@$(DEV) ./dev.sh menu

help:
	@printf "RSVP Nano helper targets\n"
	@printf "\n"
	@printf "  make menu               Interactive dev console (build/flash/monitor/test)\n"
	@printf "\n"
	@printf "Common:\n"
	@printf "  make build              Build ENV=%s (default: amoled)\n" "$(ENV)"
	@printf "  make build-amoled       Build the Waveshare AMOLED firmware\n"
	@printf "  make build-bar          Build the original bar-board USB MSC firmware\n"
	@printf "  make test               Run native pacing tests\n"
	@printf "  make flash              Build + flash ENV=%s using ./flash.sh\n" "$(ENV)"
	@printf "  make flash-monitor      Build + flash, then open serial monitor\n"
	@printf "  make rsvp               Alias for make flash\n"
	@printf "  make ports              List likely ESP32 serial ports\n"
	@printf "  make monitor            Open PlatformIO serial monitor\n"
	@printf "  make clean              Clean ENV=%s build output\n" "$(ENV)"
	@printf "\n"
	@printf "Variables:\n"
	@printf "  ENV=amoled|waveshare_esp32s3_usb_msc|native_test\n"
	@printf "  PORT=/dev/cu.usbmodem1101 or PORT=auto\n"
	@printf "  PIO='python3 -m platformio' or PIO=/path/to/pio\n"
	@printf "  VERSION=v0.1.0-amoled for clean local build/flash/export labels\n"
	@printf "\n"
	@printf "Examples:\n"
	@printf "  make flash PORT=/dev/cu.usbmodem1101\n"
	@printf "  make build ENV=waveshare_esp32s3_usb_msc\n"

build:
	$(VERSION_ENV) $(PIO) run -e $(ENV)

build-amoled:
	$(VERSION_ENV) $(PIO) run -e amoled

build-bar:
	$(VERSION_ENV) $(PIO) run -e waveshare_esp32s3_usb_msc

test:
	$(PIO) test -e native_test

flash:
	./flash.sh $(FLASH_ARGS)

flash-monitor: MONITOR := 1
flash-monitor: flash

rsvp: flash

monitor:
	@$(DEV) ./dev.sh monitor

ports:
	@printf "Likely ESP32 serial ports:\n"
	@ls -1 /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null || true

clean:
	$(PIO) run -e $(ENV) -t clean

export-web:
ifeq ($(strip $(VERSION)),)
	python3 tools/export_web_firmware.py
else
	python3 tools/export_web_firmware.py --version $(VERSION)
endif

release-check: test build-amoled build-bar

# Compatibility names from the AMOLED bring-up workflow. The bring-up source is
# no longer separate, so these now point at the integrated AMOLED firmware path.
flash-bringup: flash

merge-bringup:
	@printf "Bring-up code is already merged into the amoled env.\n"
	@printf "Use 'make flash' or 'make build-amoled'.\n"
