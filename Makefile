# make setup  : one-time host config (needs sudo: dialout group + USB wake rule)
# make flash  : build + upload
# make test   : send a wake message via ntfy
# make monitor
-include .env
export

.PHONY: setup flash test monitor build
setup:
	sudo usermod -aG dialout $$USER
	sudo scripts/enable-wake-linux.sh
build:
	pio run
flash:
	pio run -t upload
monitor:
	pio device monitor
test:
	curl -s -d "wake" https://$(NTFY_HOST)/$(NTFY_TOPIC)
