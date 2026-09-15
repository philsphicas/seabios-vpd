# SPDX-License-Identifier: MIT
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
include config.mk

WORK := .build
DIST := dist
PATCHES := patches/0001-tpm-platform-authorization.patch \
    patches/0002-certificate-vpd.patch
HEADERS := platform/compat-fw-cfg.h platform/cert-vpd.h
export WORK DIST
export PYTHONDONTWRITEBYTECODE := 1
PYTHON ?= python3
CC := gcc

.PHONY: all bios test check smoke dist clean
all: bios

$(WORK):
	mkdir -p "$@"

$(WORK)/upstream.tar: config.mk | $(WORK)
	@if test -f upstream.tar; then \
	  sha256sum -c upstream.sha256; \
	  cp upstream.tar "$@"; \
	else \
	  git init -q "$(WORK)/git"; \
	  git -C "$(WORK)/git" fetch -q --depth=1 "$(UPSTREAM_URL)" "$(UPSTREAM_COMMIT)"; \
	  test "$$(git -C "$(WORK)/git" rev-parse FETCH_HEAD)" = "$(UPSTREAM_COMMIT)"; \
	  git -C "$(WORK)/git" archive --format=tar "$(UPSTREAM_COMMIT)" > "$@.tmp"; \
	  mv "$@.tmp" "$@"; \
	fi

$(WORK)/seabios/.config: $(WORK)/upstream.tar $(PATCHES) $(HEADERS) config/seabios.config Makefile
	rm -rf -- "$(WORK)/seabios"
	mkdir -p "$(WORK)/seabios"
	tar -xf "$(WORK)/upstream.tar" -C "$(WORK)/seabios"
	@for patch in $(PATCHES); do \
	  patch --batch --forward --fuzz=0 -p1 -d "$(WORK)/seabios" -i "$(CURDIR)/$$patch"; \
	done
	cp $(HEADERS) "$(WORK)/seabios/src/fw/"
	@printf '%s\n' "$(UPSTREAM_VERSION)" > "$(WORK)/seabios/.version"
	cp config/seabios.config "$@"
	+$(MAKE) -C "$(WORK)/seabios" PYTHON="$(PYTHON)" olddefconfig

bios: $(WORK)/seabios/.config
	+version=$$(bash scripts/version.sh); \
	  $(MAKE) -C "$(WORK)/seabios" PYTHON="$(PYTHON)" CC="$(CC)" \
	    EXTRAVERSION="-seabios-vpd-$$version"
	cp "$(WORK)/seabios/out/bios.bin" "$(WORK)/bios-vpd.bin"

test: $(WORK)/upstream.tar $(PATCHES)
	CC="$(CC)" bash tests/tpm-prepboot.sh
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py'
	mkdir -p "$(WORK)/tests"
	tar -xf "$(WORK)/upstream.tar" -C "$(WORK)/tests" src/std/acpi.h src/types.h src/romfile.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I"$(WORK)/tests/src" \
	  -c tests/fw-cfg-header.c -o "$(WORK)/tests/fw-cfg-header.o"
	$(CC) -std=gnu11 -O2 -g -fno-pie -no-pie -Wall -Wextra -Werror \
	  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
	  -I"$(WORK)/tests/src" tests/publisher.c -o "$(WORK)/tests/publisher"
	"$(WORK)/tests/publisher"

check: test bios
smoke: bios
	bash tests/smoke.sh
dist: bios
	bash scripts/package.sh
clean:
	rm -rf -- .build dist
