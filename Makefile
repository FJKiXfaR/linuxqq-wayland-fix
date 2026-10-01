# linuxqq-wayland-screenshare-fix
#
#   make
#   make install DESTDIR=... PREFIX=/usr
#
# LIBEXECDIR 下放注入库；启动脚本在安装时写入它的绝对路径。

NAME       := linuxqq-wayland-screenshare-fix
VERSION    ?= $(or $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//'),0.0.0)

PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/$(NAME)
DATADIR    ?= $(PREFIX)/share
DOCDIR     ?= $(DATADIR)/doc/$(NAME)

CC         ?= cc
CFLAGS     ?= -O2 -g
PKG_CONFIG ?= pkg-config

DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags gio-unix-2.0 libpulse libpipewire-0.3)
DEP_LIBS   := $(shell $(PKG_CONFIG) --libs gio-unix-2.0)

CMD        := linuxqq-wayland-native-screenshare-fix
LIB        := libqq-wl-portal.so
OBJS       := src/qq-wl-portal.o src/dlsym_trampoline.o

all: $(LIB) $(CMD)

src/qq-wl-portal.o: src/qq-wl-portal.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra -Wno-nonnull-compare \
	    -DQQWL_VERSION='"$(VERSION)"' $(DEP_CFLAGS) -c -o $@ $<

src/dlsym_trampoline.o: src/dlsym_trampoline.S
	$(CC) $(CPPFLAGS) $(ASFLAGS) -fPIC -c -o $@ $<

# 不链接 libpulse：只在 broadcast-core 已加载 libpulse 时才用到它，见源码注释。
$(LIB): $(OBJS)
	$(CC) $(LDFLAGS) -shared -Wl,-z,defs -o $@ $(OBJS) $(DEP_LIBS) -ldl -Wl,--allow-shlib-undefined

$(CMD): $(CMD).in
	sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@
	chmod +x $@

install: all
	install -Dm755 $(LIB)               $(DESTDIR)$(LIBEXECDIR)/$(LIB)
	install -Dm755 $(CMD)               $(DESTDIR)$(BINDIR)/$(CMD)
	install -Dm644 $(CMD).desktop       $(DESTDIR)$(DATADIR)/applications/$(CMD).desktop
	install -Dm644 README.md            $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 LICENSE              $(DESTDIR)$(DATADIR)/licenses/$(NAME)/LICENSE

clean:
	rm -f $(OBJS) $(LIB) $(CMD)

.PHONY: all install clean

print-version:
	@echo $(VERSION)

.PHONY: print-version
