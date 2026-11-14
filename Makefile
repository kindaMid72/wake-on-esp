# make setup  : one-time host config (needs sudo: dialout group + USB wake rule)
# make flash  : build + upload
# make test   : send a wake message via ntfy
# make monitor
-include .env
export

# Find PlatformIO: PATH first, then next to conda, then the pip-installed copy.
# Override with: make PIO=/path/to/platformio build
PIO ?= $(shell for c in pio platformio "$$(dirname "$$(dirname "$$(command -v conda 2>/dev/null)")")/bin/platformio" "$$HOME/.platformio/penv/bin/pio"; do p=$$(command -v "$$c" 2>/dev/null) && { echo "$$p"; break; }; done)
PIO_PY ?= $(firstword $(wildcard $(HOME)/.platformio/penv/bin/python) python3)
ifeq ($(strip $(PIO)),)
$(error PlatformIO not found: install it or run `make PIO=/path/to/platformio <target>`)
endif

.PHONY: setup flash test monitor build
setup:
	sudo usermod -aG dialout $$USER
	sudo scripts/enable-wake-linux.sh
build:
	$(PIO) run
flash:
	-$(PIO_PY) scripts/enter-bootloader.py
	$(PIO) run -t upload
	-$(PIO_PY) $(HOME)/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --after watchdog-reset read-mac >/dev/null 2>&1
monitor:
	$(PIO) device monitor
test:
	curl -s -d "wake" https://$(NTFY_HOST)/$(NTFY_TOPIC)
