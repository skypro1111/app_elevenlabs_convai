#
# Makefile for app_elevenlabs_convai module
#
# This is a standalone Makefile for development and testing purposes
# In production, the module would be integrated into Asterisk's build system
#

CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -fPIC -shared
ASTERISK_INCLUDE_DIR=../asterisk_context_files

# External library dependencies for ElevenLabs integration
LIBS=-lcurl -lwebsockets -lcjson -lssl -lcrypto -lpthread
LDFLAGS=-shared $(LIBS)

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
check-deps:
	@echo "Checking for required libraries..."
	@pkg-config --exists libcurl || (echo "ERROR: libcurl-dev not found. Install with: sudo apt-get install libcurl4-openssl-dev" && exit 1)
	@pkg-config --exists libwebsockets || (echo "ERROR: libwebsockets-dev not found. Install with: sudo apt-get install libwebsockets-dev" && exit 1)
	@pkg-config --exists libcjson || (echo "ERROR: libcjson-dev not found. Install with: sudo apt-get install libcjson-dev" && exit 1)
	@pkg-config --exists openssl || (echo "ERROR: openssl-dev not found. Install with: sudo apt-get install libssl-dev" && exit 1)
	@echo "All required libraries found!"

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
		sudo yum install -y libcurl-devel libwebsockets-devel libcjson-devel openssl-devel; \
	elif command -v dnf >/dev/null 2>&1; then \
		sudo dnf install -y libcurl-devel libwebsockets-devel libcjson-devel openssl-devel; \
	elif command -v brew >/dev/null 2>&1; then \
		brew install curl libwebsockets cjson openssl; \
	else \
		echo "Package manager not detected. Please install manually:"; \
		echo "  - libcurl development headers"; \
		echo "  - libwebsockets development headers"; \
		echo "  - libcjson development headers"; \
		echo "  - OpenSSL development headers"; \
	fi

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