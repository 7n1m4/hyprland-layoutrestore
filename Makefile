# Builds layoutrestore.so against the installed Hyprland headers.
#
# Requires the headers for the Hyprland build you want to load into:
#   hyprpm update            (or: make installheaders in the hyprland tree)
#   pkg-config --cflags hyprland
#
# The plugin links against no Hyprland library: the Hyprland executable exports
# its symbols, so they resolve against it when the plugin is dlopen'd.

CXXFLAGS = -shared -fPIC -g -std=c++26 -Wno-c++11-narrowing -O2
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland hyprutils hyprlang hyprgraphics aquamarine hyprcursor`
LIBS     = `pkg-config --libs pangocairo`

SRC    = main.cpp src/LayoutModel.cpp
TARGET = layoutrestore.so

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC) globals.hpp src/LayoutModel.hpp
# shellcheck disable=SC2086
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) -o $@ $(LIBS)

test: $(SRC) tests/plugins/LayoutModel.cpp
# shellcheck disable=SC2086
	$(CXX) -std=c++26 -I/usr/include tests/plugins/LayoutModel.cpp -lgtest -lgtest_main -pthread -o /tmp/layoutrestore-tests
	/tmp/layoutrestore-tests
	rm -f /tmp/layoutrestore-tests

clean:
	rm -f ./$(TARGET)
