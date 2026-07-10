# Cross-platform GNU Makefile for the Ride Booking System
# Outputs all binaries to build/

CXX ?= g++
BUILD_DIR ?= build

CXXFLAGS ?= -std=c++17 -Wall -Wextra
LDFLAGS ?=
LDLIBS ?=

ifeq ($(OS),Windows_NT)
  EXE := .exe
  LDLIBS += -lws2_32
  MKDIR_BUILD = if not exist "$(subst /,\,$(BUILD_DIR))" mkdir "$(subst /,\,$(BUILD_DIR))"
  RM_BUILD = if exist "$(subst /,\,$(BUILD_DIR))" rmdir /S /Q "$(subst /,\,$(BUILD_DIR))"
else
  EXE :=
  CXXFLAGS += -pthread
  LDLIBS += -pthread
  MKDIR_BUILD = mkdir -p "$(BUILD_DIR)"
  RM_BUILD = rm -rf "$(BUILD_DIR)"
endif

SERVER := $(BUILD_DIR)/ride_server$(EXE)
DRIVER_CLIENT := $(BUILD_DIR)/driver_client$(EXE)
PASSENGER_CLIENT := $(BUILD_DIR)/passenger_client$(EXE)

.PHONY: all server clients clean run help

all: $(SERVER) $(DRIVER_CLIENT) $(PASSENGER_CLIENT)

server: $(SERVER)

clients: $(DRIVER_CLIENT) $(PASSENGER_CLIENT)

$(BUILD_DIR):
	$(MKDIR_BUILD)

$(SERVER): main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(DRIVER_CLIENT): driver_client.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(PASSENGER_CLIENT): passenger_client.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: $(SERVER)
	$(SERVER)

clean:
	$(RM_BUILD)

help:
	@echo "Targets:"
	@echo "  make all      Build server and clients into build/"
	@echo "  make server   Build only the backend server"
	@echo "  make clients  Build driver and passenger clients"
	@echo "  make run      Build and run the backend server"
	@echo "  make clean    Remove build/"
	@echo ""
	@echo "Outputs:"
	@echo "  $(SERVER)"
	@echo "  $(DRIVER_CLIENT)"
	@echo "  $(PASSENGER_CLIENT)"
