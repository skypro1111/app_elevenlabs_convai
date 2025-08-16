#
# Makefile for app_elevenlabs_convai module
#
# This is a standalone Makefile for development and testing purposes
# In production, the module would be integrated into Asterisk's build system
#

CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -fPIC -shared -I/usr/local/include

# Try to find Asterisk headers in standard locations
ASTERISK_INCLUDE_DIR=$(shell if [ -d "/usr/include/asterisk" ]; then echo "/usr/include"; elif [ -d "../asterisk_context_files" ]; then echo "../asterisk_context_files"; else echo "/usr/local/include/asterisk"; fi)

# External library dependencies for ElevenLabs integration
LIBS=-lcurl -lwebsockets -lcjson -lssl -lcrypto -lpthread
LDFLAGS=-shared -L/usr/local/lib $(LIBS)

# Check for required libraries
CHECK_LIBS=curl libwebsockets cjson openssl

# Module name
MODULE=app_elevenlabs_convai.so
SOURCE=app_elevenlabs_convai.c
TEST_PROGRAM=test_config
TEST_SOURCE=tests/test_config.c
WEBSOCKET_TEST=test_websocket
WEBSOCKET_SOURCE=tests/test_websocket.c
AUDIO_TEST=test_audio
AUDIO_SOURCE=tests/test_audio.c
INTEGRATION_TEST=test_integration
INTEGRATION_SOURCE=tests/test_integration.c

# Default target
all: check-deps $(MODULE)

# Check for required development libraries
check-deps: check-asterisk-headers
	@echo "Checking for required libraries..."
	@pkg-config --exists libcurl || (echo "ERROR: libcurl development headers not found." && echo "Install with: sudo yum install libcurl-devel" && exit 1)
	@pkg-config --exists openssl || (echo "ERROR: openssl development headers not found." && echo "Install with: sudo yum install openssl-devel" && exit 1)
	@if pkg-config --exists libwebsockets; then \
		echo "libwebsockets found"; \
	elif [ -f /usr/local/lib/pkgconfig/libwebsockets.pc ]; then \
		echo "libwebsockets found (local install)"; \
	elif [ -f /usr/local/include/libwebsockets.h ]; then \
		echo "libwebsockets headers found"; \
	else \
		echo "ERROR: libwebsockets not found. Run: make install-libwebsockets"; \
		exit 1; \
	fi
	@if pkg-config --exists libcjson; then \
		echo "cJSON found"; \
	elif [ -f /usr/local/lib/pkgconfig/libcjson.pc ]; then \
		echo "cJSON found (local install)"; \
	elif [ -f /usr/local/include/cjson/cJSON.h ]; then \
		echo "cJSON headers found"; \
	else \
		echo "ERROR: cJSON not found. Run: make install-cjson"; \
		exit 1; \
	fi
	@echo "All required libraries found!"

# Check for Asterisk headers
check-asterisk-headers:
	@echo "Checking for Asterisk headers..."
	@echo "Using include directory: $(ASTERISK_INCLUDE_DIR)"
	@if [ -f "$(ASTERISK_INCLUDE_DIR)/asterisk.h" ] || [ -f "$(ASTERISK_INCLUDE_DIR)/asterisk/asterisk.h" ]; then \
		echo "Asterisk headers found"; \
	else \
		echo "ERROR: Asterisk headers not found!"; \
		echo ""; \
		echo "Options to fix this:"; \
		echo "1. Install Asterisk development headers:"; \
		echo "   sudo yum install asterisk-devel"; \
		echo ""; \
		echo "2. Or copy the asterisk_context_files directory from the project"; \
		echo ""; \
		echo "3. Or specify a custom path:"; \
		echo "   make ASTERISK_INCLUDE_DIR=/path/to/asterisk/include"; \
		echo ""; \
		exit 1; \
	fi

# Build the module
$(MODULE): $(SOURCE)
	$(CC) $(CFLAGS) -I$(ASTERISK_INCLUDE_DIR) $(SOURCE) -o $(MODULE) $(LDFLAGS)

# Build configuration test program
$(TEST_PROGRAM): $(TEST_SOURCE)
	$(CC) -Wall -Wextra -std=c99 -I$(ASTERISK_INCLUDE_DIR) $(TEST_SOURCE) -lcjson -o $(TEST_PROGRAM)

# Build WebSocket test program
$(WEBSOCKET_TEST): $(WEBSOCKET_SOURCE)
	$(CC) -Wall -Wextra -std=c99 $(WEBSOCKET_SOURCE) -lwebsockets -lcjson -lssl -lcrypto -o $(WEBSOCKET_TEST)

# Build audio processing test program
$(AUDIO_TEST): $(AUDIO_SOURCE)
	$(CC) -Wall -Wextra -std=c99 $(AUDIO_SOURCE) -lssl -lcrypto -o $(AUDIO_TEST)

# Build integration test program
$(INTEGRATION_TEST): $(INTEGRATION_SOURCE)
	$(CC) -Wall -Wextra -std=c99 $(INTEGRATION_SOURCE) $(LIBS) -o $(INTEGRATION_TEST)

# Run configuration parsing tests
test: $(TEST_PROGRAM)
	./$(TEST_PROGRAM)

# Run WebSocket connection tests
test-websocket: $(WEBSOCKET_TEST)
	./$(WEBSOCKET_TEST)

# Run audio processing tests
test-audio: $(AUDIO_TEST)
	./$(AUDIO_TEST)

# Run integration tests
test-integration: $(INTEGRATION_TEST)
	./$(INTEGRATION_TEST)

# Run all tests
test-all: test test-websocket test-audio
	@echo "All basic tests completed successfully!"

# Run comprehensive test suite including integration test
test-full: test-all test-integration
	@echo "Full test suite completed successfully!"

# Start mock ElevenLabs server for testing
mock-server:
	@echo "Starting mock ElevenLabs server on localhost:8080..."
	@echo "Press Ctrl+C to stop"
	python3 tests/mock_elevenlabs_server.py

# Test with real ElevenLabs API (requires valid configuration)
test-live:
	@echo "Testing with live ElevenLabs API..."
	@echo "Make sure elevenlabs.conf.example has valid credentials"
	./$(INTEGRATION_TEST) --live

# Clean built files
clean:
	rm -f $(MODULE) $(TEST_PROGRAM) $(WEBSOCKET_TEST) $(AUDIO_TEST) $(INTEGRATION_TEST)

# Install (requires proper permissions)
install: $(MODULE)
	cp $(MODULE) /usr/lib/asterisk/modules/

# Install example configuration
install-config:
	@if [ ! -f /etc/asterisk/elevenlabs.conf ]; then \
		echo "Installing example configuration..."; \
		cp elevenlabs.conf.example /etc/asterisk/elevenlabs.conf; \
		echo "Please edit /etc/asterisk/elevenlabs.conf with your API credentials"; \
	else \
		echo "Configuration file already exists at /etc/asterisk/elevenlabs.conf"; \
	fi

# Install dependencies (for development)
install-deps:
	@echo "Installing required development libraries..."
	@if command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev libwebsockets-dev libcjson-dev libssl-dev; \
	elif command -v yum >/dev/null 2>&1; then \
		echo "Installing dependencies for CentOS/RHEL..."; \
		sudo yum install -y epel-release; \
		sudo yum install -y libcurl-devel openssl-devel; \
		echo "Installing libwebsockets from source..."; \
		$(MAKE) install-libwebsockets; \
		echo "Installing cJSON from source..."; \
		$(MAKE) install-cjson; \
	elif command -v dnf >/dev/null 2>&1; then \
		echo "Installing dependencies for Fedora/CentOS 8+..."; \
		sudo dnf install -y epel-release; \
		sudo dnf install -y libcurl-devel openssl-devel; \
		echo "Installing libwebsockets from source..."; \
		$(MAKE) install-libwebsockets; \
		echo "Installing cJSON from source..."; \
		$(MAKE) install-cjson; \
	elif command -v brew >/dev/null 2>&1; then \
		brew install curl libwebsockets cjson openssl; \
	else \
		echo "Package manager not detected. Please install manually:"; \
		echo "  - libcurl development headers"; \
		echo "  - libwebsockets development headers"; \
		echo "  - libcjson development headers"; \
		echo "  - OpenSSL development headers"; \
	fi

# Install libwebsockets from source (for CentOS/RHEL)
install-libwebsockets:
	@echo "Building libwebsockets from source..."
	cd /tmp && \
	wget -O libwebsockets.tar.gz https://github.com/warmcat/libwebsockets/archive/v4.3.2.tar.gz && \
	tar -xzf libwebsockets.tar.gz && \
	cd libwebsockets-4.3.2 && \
	mkdir build && cd build && \
	cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DLWS_WITH_SSL=ON && \
	make && sudo make install && \
	sudo ldconfig

# Install cJSON from source (for CentOS/RHEL)
install-cjson:
	@echo "Building cJSON from source..."
	cd /tmp && \
	wget -O cjson.tar.gz https://github.com/DaveGamble/cJSON/archive/v1.7.16.tar.gz && \
	tar -xzf cjson.tar.gz && \
	cd cJSON-1.7.16 && \
	mkdir build && cd build && \
	cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_CJSON_UTILS=On -DENABLE_CJSON_TEST=Off && \
	make && sudo make install && \
	sudo ldconfig

# Development targets
debug: CFLAGS += -g -DDEBUG -O0
debug: $(MODULE)

release: CFLAGS += -O2 -DNDEBUG
release: $(MODULE)

# Help target
help:
	@echo "Available targets:"
	@echo "  all              - Build the module (default)"
	@echo "  check-deps       - Check for required libraries"
	@echo "  debug            - Build with debug symbols"
	@echo "  release          - Build optimized release version"
	@echo "  test             - Run configuration tests"
	@echo "  test-websocket   - Run WebSocket tests"
	@echo "  test-audio       - Run audio processing tests"
	@echo "  test-integration - Run integration tests"
	@echo "  test-all         - Run all basic tests"
	@echo "  test-full        - Run comprehensive test suite"
	@echo "  mock-server      - Start mock ElevenLabs server"
	@echo "  test-live        - Test with real ElevenLabs API"
	@echo "  clean            - Clean built files"
	@echo "  install          - Install module to Asterisk"
	@echo "  install-config   - Install example configuration"
	@echo "  install-deps     - Install development dependencies"
	@echo "  help             - Show this help message"

.PHONY: all clean install test test-websocket test-audio test-integration test-all test-full mock-server test-live check-deps debug release install-config install-deps help