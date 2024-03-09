CC = ${CROSS_COMPILE}gcc
CXX = ${CROSS_COMPILE}g++

CCACHE = ccache
CFLAGS = -Wall -Wextra -Wno-unused-parameter -O2 -DNO_OPENSSL=1
CXXFLAGS = $(CFLAGS) -std=c++20
LDFLAGS = -lrt
LIBS = -limp -lalog -lsysutils -lliveMedia -lgroupsock -lBasicUsageEnvironment -lUsageEnvironment -lconfig++ -lfreetype -lz

LIBIMP_INC_DIR = ./include

SRC_DIR = ./src
OBJ_DIR = ./obj
BIN_DIR = ./bin

SOURCES = $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(wildcard $(SRC_DIR)/*.cpp)) \
          $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(wildcard $(SRC_DIR)/*.c))

$(info $(OBJECTS))

TARGET = $(BIN_DIR)/prudynt

ifndef commit_tag
commit_tag=$(shell git rev-parse --short HEAD)
endif

VERSION_FILE = $(LIBIMP_INC_DIR)/version.hpp

$(VERSION_FILE): $(SRC_DIR)/version.tpl.hpp
	@if  ! grep "$(commit_tag)" version.h >/dev/null 2>&1 ; then \
	echo "update version.h" ; \
	sed 's/COMMIT_TAG/"$(commit_tag)"/g' src/version.tpl.hpp > include/version.hpp ; \
	fi

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CCACHE) $(CXX) $(CXXFLAGS) -I$(LIBIMP_INC_DIR) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CCACHE) $(CC) $(CFLAGS) -I$(LIBIMP_INC_DIR) -c $< -o $@

$(TARGET): $(OBJECTS) $(VERSION_FILE)
	@mkdir -p $(@D)
	$(CCACHE) $(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

.PHONY: all clean

all: $(TARGET)

clean:
	rm -rf $(OBJ_DIR)
	rm -f $(LIBIMP_INC_DIR)/version.hpp

distclean: clean
	rm -rf $(BIN_DIR)
