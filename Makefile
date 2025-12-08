# SQLite S-Expression Extension Makefile
#
# Usage:
#   make              - Build for current platform
#   make test         - Run tests
#   make debug        - Build with debug symbols
#   make clean        - Remove build artifacts
#   make install      - Install to system
#
# Cross-compilation:
#   make TARGET_OS=windows    - Build for Windows (requires mingw-w64)
#   make TARGET_OS=macos      - Build for macOS (requires osxcross or native)
#   make TARGET_OS=linux      - Build for Linux
#
# Variables:
#   CC          - C compiler (default: auto-detected)
#   SQLITE_INC  - SQLite header location
#   PREFIX      - Installation prefix (default: /usr/local)

# Source files
SRCDIR = src
OBJDIR = obj
TESTDIR = test

SRCS = $(SRCDIR)/sexp_parser.c \
       $(SRCDIR)/sexp_io.c \
       $(SRCDIR)/sexp_ops.c \
       $(SRCDIR)/sqlite_sexp.c

# Detect host OS if TARGET_OS not specified
ifndef TARGET_OS
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        TARGET_OS = macos
    else ifeq ($(UNAME_S),Linux)
        TARGET_OS = linux
    else ifneq (,$(findstring MINGW,$(UNAME_S)))
        TARGET_OS = windows
    else ifneq (,$(findstring MSYS,$(UNAME_S)))
        TARGET_OS = windows
    else ifneq (,$(findstring CYGWIN,$(UNAME_S)))
        TARGET_OS = windows
    else
        TARGET_OS = linux
    endif
endif

# Common warning flags - comprehensive and strict
WARN_FLAGS = -Wall -Wextra -Werror \
             -Wstrict-prototypes -Wmissing-prototypes \
             -Wold-style-definition -Wmissing-declarations \
             -Wpointer-arith -Wcast-qual -Wcast-align \
             -Wshadow -Wwrite-strings -Wformat=2 \
             -Wundef -Wredundant-decls -Wnested-externs \
             -Wno-unused-parameter

# Platform-specific settings
ifeq ($(TARGET_OS),macos)
    # macOS settings
    CC ?= clang
    EXT = dylib
    CFLAGS = $(WARN_FLAGS) -O2 -fPIC -std=c99
    LDFLAGS = -dynamiclib -undefined dynamic_lookup
    SQLITE_INC ?= /usr/include
    # For Homebrew SQLite on Apple Silicon
    ifneq (,$(wildcard /opt/homebrew/opt/sqlite/include))
        SQLITE_INC = /opt/homebrew/opt/sqlite/include
    endif
    # For Homebrew SQLite on Intel Mac
    ifneq (,$(wildcard /usr/local/opt/sqlite/include))
        SQLITE_INC = /usr/local/opt/sqlite/include
    endif
    TARGET = sexp.$(EXT)
    INSTALL_CMD = install
    MKDIR_CMD = mkdir -p

else ifeq ($(TARGET_OS),windows)
    # Windows settings (using MinGW-w64)
    CC ?= x86_64-w64-mingw32-gcc
    EXT = dll
    CFLAGS = $(WARN_FLAGS) -O2 -std=c99
    LDFLAGS = -shared -static-libgcc
    # Export symbols for DLL
    LDFLAGS += -Wl,--export-all-symbols
    SQLITE_INC ?= /usr/include
    TARGET = sexp.$(EXT)
    # Windows-specific: create import library
    LDFLAGS += -Wl,--out-implib,libsexp.a
    # For native Windows builds
    ifeq ($(OS),Windows_NT)
        CC = gcc
        MKDIR_CMD = if not exist $(OBJDIR) mkdir $(OBJDIR)
        RM_CMD = if exist $(OBJDIR) rmdir /s /q $(OBJDIR) & if exist $(TARGET) del $(TARGET) & if exist libsexp.a del libsexp.a
    else
        MKDIR_CMD = mkdir -p
        RM_CMD = rm -rf $(OBJDIR) $(TARGET) libsexp.a
    endif
    INSTALL_CMD = install

else
    # Linux settings (default)
    CC ?= cc
    EXT = so
    CFLAGS = $(WARN_FLAGS) -O2 -fPIC -std=c99
    LDFLAGS = -shared
    SQLITE_INC ?= /usr/include
    TARGET = sexp.$(EXT)
    INSTALL_CMD = install
    MKDIR_CMD = mkdir -p
endif

# Object files
OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Default target
.PHONY: all
all: $(TARGET)

# Link
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Compile
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/sexp.h | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(SQLITE_INC) -c -o $@ $<

# Create object directory
$(OBJDIR):
ifeq ($(TARGET_OS),windows)
ifeq ($(OS),Windows_NT)
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
else
	@mkdir -p $(OBJDIR)
endif
else
	@mkdir -p $(OBJDIR)
endif

# Clean
.PHONY: clean
clean:
ifeq ($(TARGET_OS),windows)
ifeq ($(OS),Windows_NT)
	@if exist $(OBJDIR) rmdir /s /q $(OBJDIR)
	@if exist $(TARGET) del $(TARGET)
	@if exist libsexp.a del libsexp.a
else
	rm -rf $(OBJDIR) $(TARGET) libsexp.a
endif
else
	rm -rf $(OBJDIR) $(TARGET)
endif

# Test (only works on native platform)
.PHONY: test
test: $(TARGET)
	sqlite3 < $(TESTDIR)/test_basic.sql

# Debug build
.PHONY: debug
debug: CFLAGS += -g -DDEBUG -O0
debug: clean all

# AddressSanitizer build
# Note: -shared-libasan is required for dynamically loaded extensions
# The host process (sqlite3) must LD_PRELOAD the ASAN runtime
# At runtime, LD_LIBRARY_PATH must include the ASAN library directory
.PHONY: asan
asan: CC = clang
asan: CFLAGS += -g -O1 -fsanitize=address -shared-libasan -fno-omit-frame-pointer -DDEBUG
asan: LDFLAGS += -fsanitize=address -shared-libasan
asan: clean all

# MemorySanitizer build (requires clang)
.PHONY: msan
msan: CC = clang
msan: CFLAGS += -g -O1 -fsanitize=memory -fno-omit-frame-pointer -DDEBUG
msan: LDFLAGS += -fsanitize=memory
msan: clean all

# UndefinedBehaviorSanitizer build
.PHONY: ubsan
ubsan: CC = clang
ubsan: CFLAGS += -g -O1 -fsanitize=undefined -fno-omit-frame-pointer -DDEBUG
ubsan: LDFLAGS += -fsanitize=undefined
ubsan: clean all

# Installation
PREFIX ?= /usr/local
.PHONY: install
install: $(TARGET)
ifeq ($(TARGET_OS),macos)
	$(INSTALL_CMD) -d $(PREFIX)/lib/sqlite3
	$(INSTALL_CMD) -m 755 $(TARGET) $(PREFIX)/lib/sqlite3/
else ifeq ($(TARGET_OS),linux)
	$(INSTALL_CMD) -d $(PREFIX)/lib/sqlite3
	$(INSTALL_CMD) -m 755 $(TARGET) $(PREFIX)/lib/sqlite3/
else ifeq ($(TARGET_OS),windows)
	@echo "For Windows, copy $(TARGET) to your SQLite extensions directory"
endif

# Cross-compilation targets
.PHONY: linux macos windows all-platforms

linux:
	$(MAKE) TARGET_OS=linux

macos:
	$(MAKE) TARGET_OS=macos

windows:
	$(MAKE) TARGET_OS=windows

all-platforms: clean
	$(MAKE) TARGET_OS=linux TARGET=sexp-linux.so
	$(MAKE) clean-objs
	$(MAKE) TARGET_OS=macos TARGET=sexp-macos.dylib
	$(MAKE) clean-objs
	$(MAKE) TARGET_OS=windows TARGET=sexp-windows.dll

.PHONY: clean-objs
clean-objs:
	rm -rf $(OBJDIR)

# Benchmark
.PHONY: benchmark
benchmark: $(TARGET)
	@echo "Running benchmark..."
	@python3 test/benchmark_report.py 2>/dev/null || sqlite3 < test/benchmark.sql

# Help
.PHONY: help
help:
	@echo "SQLite S-Expression Extension Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all            - Build for current/target platform (default)"
	@echo "  test           - Run test suite"
	@echo "  benchmark      - Run JSONB vs SEXP benchmark"
	@echo "  debug          - Build with debug symbols"
	@echo "  clean          - Remove build artifacts"
	@echo "  install        - Install to PREFIX (default: /usr/local)"
	@echo "  linux          - Build for Linux"
	@echo "  macos          - Build for macOS"
	@echo "  windows        - Build for Windows (requires mingw-w64)"
	@echo "  all-platforms  - Build for all platforms"
	@echo "  help           - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  TARGET_OS      - Target OS: linux, macos, windows"
	@echo "  CC             - C compiler"
	@echo "  SQLITE_INC     - SQLite header directory"
	@echo "  PREFIX         - Installation prefix"
	@echo ""
	@echo "Current settings:"
	@echo "  TARGET_OS      = $(TARGET_OS)"
	@echo "  CC             = $(CC)"
	@echo "  TARGET         = $(TARGET)"
	@echo "  SQLITE_INC     = $(SQLITE_INC)"
