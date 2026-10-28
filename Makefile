#---------------------------------------------------------------------------------
# Switch Discord Activity - builds the background module, the app and the Tesla
# overlay, packs the SD card zip.
#   make          build all three
#   make dist     out/switch-discord-activity-<version>.zip  (copy its contents to the SD root),
#                 plus the loose files and SHA256SUMS the in-app updater downloads
#   make clean
# Needs devkitPro with switch-dev, switch-curl, switch-libjson-c, switch-sdl2,
# switch-sdl2_ttf, switch-sdl2_image.
#---------------------------------------------------------------------------------
VERSION   := $(shell sed -n 's/.*SWITCHRPC_VERSION "\(.*\)"/\1/p' include/config.h)
TID       := $(shell sed -n 's/.*SYSMODULE_TID 0x\([0-9A-Fa-f]*\)ULL/\1/p' include/config.h)
STAGE     := out/sd
ZIP       := out/switch-discord-activity-$(VERSION).zip

.PHONY: all sysmodule app overlay dist clean

all: sysmodule app overlay

sysmodule:
	@$(MAKE) --no-print-directory -C sysmodule

app:
	@$(MAKE) --no-print-directory -C app

overlay:
	@$(MAKE) --no-print-directory -C overlay

dist: all
	@rm -rf out
	@mkdir -p $(STAGE)/atmosphere/contents/$(TID)/flags $(STAGE)/switch/.overlays
	@cp sysmodule/SwitchRPC.nsp $(STAGE)/atmosphere/contents/$(TID)/exefs.nsp
	@cp sysmodule/toolbox.json $(STAGE)/atmosphere/contents/$(TID)/toolbox.json
	@touch $(STAGE)/atmosphere/contents/$(TID)/flags/boot2.flag
	@cp app/switch-discord-activity.nro $(STAGE)/switch/switch-discord-activity.nro
	@cp overlay/switch-discord-activity.ovl $(STAGE)/switch/.overlays/switch-discord-activity.ovl
	@cd $(STAGE) && zip -qr ../$(notdir $(ZIP)) atmosphere switch
	@cp sysmodule/SwitchRPC.nsp out/exefs.nsp
	@cp app/switch-discord-activity.nro overlay/switch-discord-activity.ovl out/
	@# binary-mode lines ("hash *name"): the 1.1.0 updater only reads those
	@cd out && sha256sum -b exefs.nsp switch-discord-activity.nro switch-discord-activity.ovl $(notdir $(ZIP)) > SHA256SUMS
	@echo "built $(ZIP)"

clean:
	@$(MAKE) --no-print-directory -C sysmodule clean
	@$(MAKE) --no-print-directory -C app clean
	@$(MAKE) --no-print-directory -C overlay clean
	@rm -rf out
