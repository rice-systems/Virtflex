-include $(XEN_ROOT)/config/Paths.mk

# Path Programs
CMAKE               := /usr/bin/cmake
FETCHER             := /usr/bin/wget -c -O

# A debug build of stubdom? //FIXME: Someone make this do something
debug               := y

STUBDOM_TARGETS     :=  grub xenstore vtpm vtpmmgr ioemu
STUBDOM_BUILD       :=  pv-grub xenstore-stubdom vtpm-stubdom vtpmmgr-stubdom ioemu-stubdom
STUBDOM_INSTALL     :=  install-grub install-xenstore install-vtpm install-vtpmmgr install-ioemu

ZLIB_VERSION        := 1.2.3
ZLIB_URL            := $(XEN_EXTFILES_URL)

LIBPCI_VERSION      := 2.2.9
LIBPCI_URL          := $(XEN_EXTFILES_URL)

NEWLIB_VERSION      := 1.16.0
NEWLIB_URL          := $(XEN_EXTFILES_URL)

LWIP_VERSION        := 1.3.0
LWIP_URL            := $(XEN_EXTFILES_URL)

GRUB_VERSION        := 0.97
GRUB_URL            := $(XEN_EXTFILES_URL)

OCAML_VERSION       := 4.02.0
OCAML_URL           := http://caml.inria.fr/pub/distrib/ocaml-4.02

GMP_VERSION         := 4.3.2
GMP_URL             := $(XEN_EXTFILES_URL)

POLARSSL_VERSION    := 1.1.4
POLARSSL_URL        := $(XEN_EXTFILES_URL)

TPMEMU_VERSION      := 0.7.4
TPMEMU_URL          := $(XEN_EXTFILES_URL)
