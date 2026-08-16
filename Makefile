# LeiOS Plymouth Theme — build, package, install and test helpers.
# All destructive commands validate their target variables to prevent
# accidental damage if a variable is empty.

PACKAGE_NAME := leios.theme.plymouth-theme
DEB_BUILD_OUTPUT_DIR := deb-build

# Guard macro: abort if a variable is empty.
require_var = $(if $(strip $1),,$(error Required variable is empty: $2))

.PHONY: all clean distclean package install update

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
