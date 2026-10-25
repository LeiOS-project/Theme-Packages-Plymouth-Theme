# LeiOS Plymouth Theme — build, package, install and test helpers.
# All destructive commands validate their target variables to prevent
# accidental damage if a variable is empty.

PACKAGE_NAME := leios.theme.plymouth-theme
DEB_BUILD_OUTPUT_DIR := deb-build

# Guard macro: abort if a variable is empty.
require_var = $(if $(strip $1),,$(error Required variable is empty: $2))

.PHONY: all clean distclean package install update test

PKG_CONFIG ?= pkg-config

all: package

# Safe cleanup used by debhelper: only removes generated files that are not
# the final build output directory. That directory is kept because this target
# is called by dh_auto_clean while the package is still being built.
clean:
	@:$(call require_var,$(DEB_BUILD_OUTPUT_DIR),DEB_BUILD_OUTPUT_DIR)
	dh_clean || true
	find . -name '*.deb' -delete
	find . -name '*.changes' -delete
	find . -name '*.buildinfo' -delete
	find . -name '*.dsc' -delete
	find . -name '*.tar.xz' -delete
	find . -name '*.tar.gz' -delete

# Full cleanup including build output and APT repo directory.
distclean: clean
	@:$(call require_var,$(DEB_BUILD_OUTPUT_DIR),DEB_BUILD_OUTPUT_DIR)
	rm -rf "$(DEB_BUILD_OUTPUT_DIR)"

package:
	@:$(call require_var,$(PACKAGE_NAME),PACKAGE_NAME)
	@:$(call require_var,$(DEB_BUILD_OUTPUT_DIR),DEB_BUILD_OUTPUT_DIR)
	mkdir -p "$(DEB_BUILD_OUTPUT_DIR)"
	dpkg-buildpackage -us -uc -b
	mv ../$(PACKAGE_NAME)_*.deb ../$(PACKAGE_NAME)_*.changes ../$(PACKAGE_NAME)_*.buildinfo "$(DEB_BUILD_OUTPUT_DIR)/" 2>/dev/null || true

install: package
	@:$(call require_var,$(PACKAGE_NAME),PACKAGE_NAME)
	@:$(call require_var,$(DEB_BUILD_OUTPUT_DIR),DEB_BUILD_OUTPUT_DIR)
	sudo dpkg -i $(DEB_BUILD_OUTPUT_DIR)/$(PACKAGE_NAME)_*.deb || \
		(sudo apt-get install -f -y && sudo dpkg -i $(DEB_BUILD_OUTPUT_DIR)/$(PACKAGE_NAME)_*.deb)

update: package
	@:$(call require_var,$(PACKAGE_NAME),PACKAGE_NAME)
	@:$(call require_var,$(DEB_BUILD_OUTPUT_DIR),DEB_BUILD_OUTPUT_DIR)
	sudo dpkg -i $(DEB_BUILD_OUTPUT_DIR)/$(PACKAGE_NAME)_*.deb || \
		(sudo apt-get install -f -y && sudo dpkg -i $(DEB_BUILD_OUTPUT_DIR)/$(PACKAGE_NAME)_*.deb)

# The theme's Plymouth plugin, for the test target (debian/rules builds the
# packaged one).
plugin/leios.so: plugin/plugin.c
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC -shared -o $@ $< \
		$$($(PKG_CONFIG) --cflags --libs ply-splash-graphics) -lm

# Safe Plymouth preview: copies the theme into /usr/share/plymouth/themes/leios
# and its plugin into Plymouth's plugin directory temporarily, runs plymouthd on the active virtual console for a few seconds,
# then restores/cleans everything up. This does NOT modify
# /etc/plymouth/plymouthd.conf, /usr/share/plymouth/plymouthd.defaults, GRUB,
# or the initramfs. Plymouth only has DRM/frame-buffer renderers here, so it
# cannot be previewed inside Xephyr; the active TTY will briefly show the splash.
test: plugin/leios.so
	@:$(call require_var,$(PACKAGE_NAME),PACKAGE_NAME)
	@echo "Checking test dependencies..."
	@command -v plymouthd >/dev/null 2>&1 || { echo "ERROR: plymouthd not found"; exit 1; }
	@command -v plymouth >/dev/null 2>&1 || { echo "ERROR: plymouth client not found"; exit 1; }
	@echo ""
	@echo "WARNING: Plymouth only has DRM/frame-buffer renderers on this system,"
	@echo "         so it cannot be previewed inside Xephyr. This target will"
	@echo "         briefly take over the active virtual console to show the"
	@echo "         splash. No config files or initramfs will be changed."
	@echo "         Press Ctrl+C within 3 seconds to abort."
	@echo ""
	@sleep 3
	@set -e; \
	TESTDIR="/usr/share/plymouth/themes/leios"; \
	PLUGIN="$$(plymouth --get-splash-plugin-path)leios.so"; \
	BACKUP=""; PLUGIN_BACKUP=""; \
	if [ -e "$$TESTDIR" ]; then \
		BACKUP="/tmp/leios-plymouth-theme-backup.$$$$(date +%s)"; \
		echo "Backing up existing $$TESTDIR to $$BACKUP ..."; \
		sudo mv "$$TESTDIR" "$$BACKUP"; \
	fi; \
	if [ -e "$$PLUGIN" ]; then \
		PLUGIN_BACKUP="/tmp/leios-plymouth-plugin-backup.$$$$(date +%s).so"; \
		echo "Backing up existing $$PLUGIN to $$PLUGIN_BACKUP ..."; \
		sudo mv "$$PLUGIN" "$$PLUGIN_BACKUP"; \
	fi; \
	cleanup() { \
		echo ""; \
		echo "Cleaning up..."; \
		sudo plymouth --quit 2>/dev/null || true; \
		sleep 0.5; \
		if [ -n "$$BACKUP" ] && [ -e "$$BACKUP" ]; then \
			sudo rm -rf "$$TESTDIR" 2>/dev/null || true; \
			sudo mv "$$BACKUP" "$$TESTDIR"; \
		elif [ -e "$$TESTDIR" ]; then \
			sudo rm -rf "$$TESTDIR"; \
		fi; \
		if [ -n "$$PLUGIN_BACKUP" ] && [ -e "$$PLUGIN_BACKUP" ]; then \
			sudo mv "$$PLUGIN_BACKUP" "$$PLUGIN"; \
		else \
			sudo rm -f "$$PLUGIN"; \
		fi; \
	}; \
	trap cleanup EXIT INT TERM; \
	echo "Copying theme to $$TESTDIR and plugin to $$PLUGIN ..."; \
	sudo mkdir -p "$$TESTDIR"; \
	sudo cp -a src/* "$$TESTDIR/"; \
	sudo install -m 0644 plugin/leios.so "$$PLUGIN"; \
	echo "Starting plymouthd on the active TTY ..."; \
	CURRENT_TTY=$$(fgconsole 2>/dev/null || echo "1"); \
	sudo plymouthd --debug --no-daemon --ignore-serial-consoles \
		--theme=leios --tty="/dev/tty$$CURRENT_TTY" & \
	PPID=$$!; \
	sleep 1; \
	echo "Showing splash ..."; \
	sudo plymouth --show-splash; \
	sleep 1; \
	sudo plymouth message --text="LeiOS Plymouth theme test"; \
	sleep 2; \
	sudo plymouth message --text="Mounting root filesystem..."; \
	sleep 2; \
	sudo plymouth message --text="Loading LeiOS desktop..."; \
	sleep 3; \
	sudo plymouth --quit; \
	wait $$PPID 2>/dev/null || true; \
	echo ""; \
	echo "Test complete. Theme files cleaned up."
