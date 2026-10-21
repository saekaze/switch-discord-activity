#---------------------------------------------------------------------------------
# Switch Discord Activity - builds the background module and the app, packs the SD card zip.
#   make          build both
#   make dist     out/switch-discord-activity-<version>.zip  (copy its contents to the SD root)
#   make clean
# Needs devkitPro with switch-dev, switch-curl, switch-libjson-c, switch-sdl2,
# switch-sdl2_ttf, switch-sdl2_image.
#---------------------------------------------------------------------------------
VERSION   := $(shell sed -n 's/.*SWITCHRPC_VERSION "\(.*\)"/\1/p' include/config.h)
TID       := $(shell sed -n 's/.*SYSMODULE_TID 0x\([0-9A-Fa-f]*\)ULL/\1/p' include/config.h)
STAGE     := out/sd
ZIP       := out/switch-discord-activity-$(VERSION).zip

.PHONY: all sysmodule app dist clean

all: sysmodule app

sysmodule:
	@$(MAKE) --no-print-directory -C sysmodule

app:
	@$(MAKE) --no-print-directory -C app

dist: all
	@rm -rf $(STAGE) $(ZIP)
	@mkdir -p $(STAGE)/atmosphere/contents/$(TID)/flags $(STAGE)/switch
	@cp sysmodule/SwitchRPC.nsp $(STAGE)/atmosphere/contents/$(TID)/exefs.nsp
	@cp sysmodule/toolbox.json $(STAGE)/atmosphere/contents/$(TID)/toolbox.json
	@touch $(STAGE)/atmosphere/contents/$(TID)/flags/boot2.flag
	@cp app/switch-discord-activity.nro $(STAGE)/switch/switch-discord-activity.nro
	@cd $(STAGE) && zip -qr ../$(notdir $(ZIP)) atmosphere switch
	@echo "built $(ZIP)"

clean:
	@$(MAKE) --no-print-directory -C sysmodule clean
	@$(MAKE) --no-print-directory -C app clean
	@rm -rf out
