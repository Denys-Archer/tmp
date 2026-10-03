CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -pthread

TARGET := bin/bavovna
ARCHIVE := bin/bavovna.tar.xz
MAIN_OBJECT := bin/main.o
CHECK_OBJECT := bin/check.o
DOWNLOADER_OBJECT := bin/downloader.o
CHECK_LIBRARY := bin/libbavovna-check.so
DOWNLOADER_LIBRARY := bin/libbavovna-downloader.so
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib/bavovna
SYSCONFDIR ?= /etc
CONFIG := bavovna-config.txt

.PHONY: all compress install clean

all: $(TARGET)

bin:
	mkdir -p bin

$(MAIN_OBJECT): main.cpp check.hpp | bin
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(CHECK_OBJECT): check.cpp check.hpp downloader.hpp | bin
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(DOWNLOADER_OBJECT): downloader.cpp downloader.hpp | bin
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

$(DOWNLOADER_LIBRARY): $(DOWNLOADER_OBJECT)
	$(CXX) -shared -Wl,-soname,libbavovna-downloader.so $(LDFLAGS) $(LDLIBS) -o $@ $^

$(CHECK_LIBRARY): $(CHECK_OBJECT) $(DOWNLOADER_LIBRARY)
	$(CXX) -shared -Wl,-soname,libbavovna-check.so -Wl,-rpath,'$$ORIGIN' $(LDFLAGS) $(LDLIBS) \
		-Lbin -o $@ $(CHECK_OBJECT) -lbavovna-downloader

$(TARGET): $(MAIN_OBJECT) $(CHECK_LIBRARY) $(DOWNLOADER_LIBRARY)
	$(CXX) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN:$$ORIGIN/../lib/bavovna' -Lbin -o $@ $(MAIN_OBJECT) \
		-lbavovna-check -lbavovna-downloader $(LDLIBS)

compress: $(TARGET) $(CHECK_LIBRARY) $(DOWNLOADER_LIBRARY)
	tar -cJf $(ARCHIVE) $(TARGET) $(CHECK_LIBRARY) $(DOWNLOADER_LIBRARY)

install: $(TARGET) $(CHECK_LIBRARY) $(DOWNLOADER_LIBRARY)
	install -Dm755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/bavovna"
	install -Dm755 "$(CHECK_LIBRARY)" "$(DESTDIR)$(LIBDIR)/libbavovna-check.so"
	install -Dm755 "$(DOWNLOADER_LIBRARY)" "$(DESTDIR)$(LIBDIR)/libbavovna-downloader.so"
	install -Dm644 "$(CONFIG)" "$(DESTDIR)$(SYSCONFDIR)/$(CONFIG)"

clean:
	rm -rf bin
