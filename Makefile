#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2017 OmniTI Computer Consulting, Inc.  All rights reserved.
# Copyright 2018 OmniOS Community Edition (OmniOSce) Association.
#

VERSION?=$(shell awk '$$1 == "OmniOS" { print $$3 }' /etc/release)
ifeq ($(shell zonename),global)
BUILDSEND=rpool/kayak_image
else
BUILDSEND="$(shell zfs list -H -o name /)/kayak_image"
endif

ZFSCREATE:=$(shell \
	zfs list -H -o name $(BUILDSEND) 2>/dev/null || \
	    zfs create -o mountpoint=/kayak_image $(BUILDSEND) \
)

BUILDSEND_MP=$(shell zfs get -o value -H mountpoint $(BUILDSEND))
DESTDIR=$(BUILDSEND_MP)

all:

PKGDIRS=build src/include src bin etc data lib installer sample
IMG_FILES=corner.png tail_bg_v1.png OmniOS_logo_medium.png tail_bg_v2.png
PKGFILES=Makefile README.md

TFTP_FILES=\
	$(DESTDIR)/tftpboot/kayak/miniroot.gz \
	$(DESTDIR)/tftpboot/kayak/miniroot.gz.hash \
	$(DESTDIR)/tftpboot/boot/grub/menu.lst \
	$(DESTDIR)/tftpboot/boot/loader.conf.local \
	$(DESTDIR)/tftpboot/boot/loader.rc \
	$(DESTDIR)/tftpboot/boot/forth \
	$(DESTDIR)/tftpboot/boot/defaults \
	$(DESTDIR)/tftpboot/boot/platform/i86pc/kernel/amd64/unix \
	$(DESTDIR)/tftpboot/pxeboot \
	$(DESTDIR)/tftpboot/pxegrub

WEB_FILES=$(DESTDIR)/var/kayak/kayak/$(VERSION).zfs.bz2

$(DESTDIR)/tftpboot/boot/loader.conf.local:	etc/loader.conf.local
	sed -e 's/@VERSION@/$(VERSION)/' $< > $@

$(DESTDIR)/tftpboot/boot/grub/menu.lst:	sample/menu.lst.000000000000
	sed -e 's/@VERSION@/$(VERSION)/' $< > $@

# Files from proto

$(DESTDIR)/tftpboot/pxegrub:	$(BUILDSEND_MP)/root/boot/grub/pxegrub
	cp -p $< $@

$(DESTDIR)/tftpboot/pxeboot:	$(BUILDSEND_MP)/root/boot/pxeboot
	cp -p $< $@

$(DESTDIR)/tftpboot/boot/loader.rc:	$(BUILDSEND_MP)/root/boot/loader.rc
	cp -p $< $@

$(DESTDIR)/tftpboot/boot/forth:	$(BUILDSEND_MP)/root/boot/forth
	cp -rp $< $@

$(DESTDIR)/tftpboot/boot/defaults:	$(BUILDSEND_MP)/root/boot/defaults
	cp -rp $< $@

$(DESTDIR)/tftpboot/boot/platform/i86pc/kernel/amd64/unix:	$(BUILDSEND_MP)/root/platform/i86pc/kernel/amd64/unix
	cp -p $< $@

$(DESTDIR)/var/kayak/kayak/$(VERSION).zfs.bz2:	$(BUILDSEND_MP)/kayak_$(VERSION).zfs.bz2
	cp -p $< $@

$(DESTDIR)/tftpboot/kayak/miniroot.gz:	$(BUILDSEND_MP)/miniroot.gz
	cp -p $< $@

$(DESTDIR)/tftpboot/kayak/miniroot.gz.hash:	$(BUILDSEND_MP)/miniroot.gz
	digest -a sha1 $< > $@

######################################################################
# More involved targets - creation of miniroot.gz & zfs image

$(BUILDSEND_MP)/kayak_$(VERSION).zfs.bz2:	build/build_zfs_send
	@test -d "$(BUILDSEND_MP)" || (echo "$(BUILDSEND) missing" && false)
	./$< -d $(BUILDSEND) $(VERSION)

$(BUILDSEND_MP)/miniroot.gz:	build/build_miniroot
	if test -n "`zfs list -H -t snapshot $(BUILDSEND)/root@fixup 2>/dev/null`"; then \
	  VERSION=$(VERSION) DEBUG=$(DEBUG) ./$< $(BUILDSEND) fixup ; \
	else \
	  VERSION=$(VERSION) DEBUG=$(DEBUG) ./$< $(BUILDSEND) begin ; \
	fi

tftp-dirs:
	mkdir -p $(DESTDIR)/tftpboot/boot/grub
	mkdir -p $(DESTDIR)/tftpboot/boot/platform/i86pc/kernel/amd64
	mkdir -p $(DESTDIR)/tftpboot/kayak

server-dirs:
	mkdir -p $(DESTDIR)/var/kayak/kayak
	mkdir -p $(DESTDIR)/var/kayak/css
	mkdir -p $(DESTDIR)/var/kayak/img
	mkdir -p $(DESTDIR)/usr/share/kayak
	mkdir -p $(DESTDIR)/var/kayak/log
	mkdir -p $(DESTDIR)/var/svc/manifest/network
	mkdir -p $(DESTDIR)/var/svc/method

# Rebuilding the anonymous dtrace configuration file requires root in a
# zone with dtrace permissions. Provide no dependencies and it won't be
# rebuilt automatically.
# This is only used in debug mode to build a list of all files
# accessed in order to build the miniroot exclusion list. These days we
# just tend to add files by hand as required.
etc/anon.dtrace.conf:
	dtrace -A -q -s etc/anon.d -o $@.tmp
	cat /kernel/drv/dtrace.conf $@.tmp > $@
	rm $@.tmp

######################################################################
# Binaries to build from source

BINS=bin/takeover-console bin/ipcalc bin/dialog bin/passutil bin/mount_media \
	 etc/kbd.list

bin/takeover-console:	src/takeover-console.c
	gcc -o $@ $<

bin/passutil:	src/passutil.c
	gcc -o $@ $<

bin/mount_media:	src/mount_media.c
	gcc -o $@ $< -ldevinfo

bin/zpool_patch:	src/zpool_patch.c
	gcc -m64 -g -Isrc/include -o $@ $< -lnvpair -lzfs -lzpool

bin/ipcalc:	build/build_ipcalc
	./build/build_ipcalc

bin/dialog:	build/build_dialog
	./build/build_dialog

# Not a binary but a data file generated from key tables
etc/kbd.list: /usr/share/lib/keytables/type_6/kbd_layouts
	grep = /usr/share/lib/keytables/type_6/kbd_layouts | cut -d= -f1 \
		> etc/kbd.list

bins: $(BINS)

clean:
	rm -f $(BINS)

######################################################################
# Install targets (see README.md)

install-tftp:	tftp-dirs $(TFTP_FILES)

install-web:	server-dirs $(WEB_FILES)

install-iso:	bins install-tftp install-web
	BUILDSEND_MP=$(BUILDSEND_MP) VERSION=$(VERSION) ./build/build_iso

install-usb:	install-iso
	./build/build_usb $(BUILDSEND_MP)/$(VERSION).iso \
	    $(BUILDSEND_MP)/$(VERSION).usb-dd

install-package:	bins tftp-dirs server-dirs
	for dir in $(PKGDIRS); do \
		mkdir -p $(DESTDIR)/usr/share/kayak/$$dir; \
		cp $$dir/* $(DESTDIR)/usr/share/kayak/$$dir/; \
	done
	for file in $(PKGFILES); do \
		cp $$file $(DESTDIR)/usr/share/kayak/$$file; \
	done
	for file in $(IMG_FILES); do \
		cp http/img/$$file $(DESTDIR)/var/kayak/img/$$file; \
	done
	\
	cp http/css/land.css $(DESTDIR)/var/kayak/css/land.css
	\
	cp smf/svc-kayak $(DESTDIR)/var/svc/method/svc-kayak
	chmod a+x $(DESTDIR)/var/svc/method/svc-kayak
	cp smf/kayak.xml $(DESTDIR)/var/svc/manifest/network/kayak.xml

