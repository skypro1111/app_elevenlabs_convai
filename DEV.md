# Development Guide - Asterisk ElevenLabs ConvAI Module

This guide provides detailed instructions for developing, building, and deploying the ElevenLabs ConvAI module without restarting Asterisk.

## Quick Development Workflow

### Hot-loading Module (No Asterisk Restart Required)

```bash
# 1. Build the module
cd app_elevenlabs_convai/
make

# 2. Unload existing module (if loaded)
asterisk -rx "module unload app_elevenlabs_convai"

# 3. Copy new module
sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/

# 4. Load the new module
asterisk -rx "module load app_elevenlabs_convai"

# 5. Reload dialplan (if changed)
asterisk -rx "dialplan reload"
```

### One-liner for rapid development:
```bash
make && sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/ && asterisk -rx "module reload app_elevenlabs_convai"
```

## Development Environment Setup

### Prerequisites

#### System Dependencies
```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install build-essential gcc make
sudo apt-get install libcurl4-openssl-dev libwebsockets-dev libcjson-dev libssl-dev

# CentOS/RHEL/Fedora
sudo dnf install gcc make
sudo dnf install libcurl-devel libwebsockets-devel libcjson-devel openssl-devel

# macOS (with Homebrew)
brew install curl libwebsockets cjson openssl
```

#### Asterisk Development Headers
For real Asterisk development, install Asterisk development packages:
```bash
# Ubuntu/Debian
sudo apt-get install asterisk-dev

# CentOS/RHEL
sudo yum install asterisk-devel
```

### Project Structure
```
asterisk_elevenlabs/
├── app_elevenlabs_convai/              # Main development directory
│   ├── app_elevenlabs_convai.c         # Core module source (1,600+ lines)
│   ├── Makefile                        # Build system with dev targets
│   ├── elevenlabs.conf.example         # Configuration template
│   └── tests/                          # Test framework directory
├── app_forkstream/                     # Reference implementation
├── asterisk_context_files/             # Mock Asterisk headers for standalone dev
└── dependencies.md                     # Library documentation
```

## Building the Module

### Standard Build
```bash
cd app_elevenlabs_convai/
make
```

### Development Build (with debug symbols)
```bash
make debug
```

### Release Build (optimized)
```bash
make release
```

### Check Dependencies
```bash
make check-deps
```

### Clean Build
```bash
make clean
make
```

## Development Workflow

### 1. Code Development Cycle

```bash
# Edit source code
nano app_elevenlabs_convai.c

# Quick build and test
make debug

# Deploy to Asterisk
sudo make install

# Reload module in Asterisk
asterisk -rx "module reload app_elevenlabs_convai"

# Test functionality
asterisk -rx "elevenlabs show agents"
```

### 2. Configuration Development

```bash
# Edit configuration template
nano elevenlabs.conf.example

# Copy to Asterisk configuration directory
sudo cp elevenlabs.conf.example /etc/asterisk/elevenlabs.conf

# Edit with real credentials
sudo nano /etc/asterisk/elevenlabs.conf

# Reload configuration without restarting
asterisk -rx "elevenlabs reload"
```

### 3. Hot Module Reloading

The module supports hot-reloading without restarting Asterisk:

```bash
# Method 1: Module reload (preserves existing sessions)
asterisk -rx "module reload app_elevenlabs_convai"

# Method 2: Unload and load (terminates existing sessions)
asterisk -rx "module unload app_elevenlabs_convai"
asterisk -rx "module load app_elevenlabs_convai"

# Method 3: Force reload (recommended for development)
asterisk -rx "module unload app_elevenlabs_convai" && \
asterisk -rx "module load app_elevenlabs_convai"
```

## Testing and Debugging

### Local Testing Setup

#### 1. Mock ElevenLabs Server (for development)
```bash
# Start mock server for testing
make mock-server
```

#### 2. Configuration Testing
```bash
# Test configuration parsing
make test

# Test with real ElevenLabs API
make test-live
```

#### 3. Integration Testing
```bash
# Run all tests
make test-all

# Full test suite including integration
make test-full
```

### Debugging

#### Enable Debug Logging
```bash
# Enable module debug
asterisk -rx "elevenlabs debug on"

# Increase Asterisk verbosity
asterisk -rx "core set verbose 4"

# Monitor logs in real-time
tail -f /var/log/asterisk/messages | grep -i elevenlabs
```

#### Debug Build with GDB
```bash
# Build with debug symbols
make debug

# Run Asterisk with GDB (in development environment)
sudo gdb asterisk
(gdb) run -c
```

#### Memory Leak Detection
```bash
# Build with memory debugging
CFLAGS="-fsanitize=address -g" make

# Run with valgrind
sudo valgrind --leak-check=full asterisk -c
```

### Development Testing Workflow

#### 1. Unit Testing
```bash
# Test configuration parsing
./test_config

# Test WebSocket functionality
./test_websocket

# Test audio processing
./test_audio
```

#### 2. Integration Testing
```bash
# Test with mock server
make mock-server &
./test_integration --mock

# Test with real ElevenLabs API
./test_integration --live
```

#### 3. Load Testing
```bash
# Generate multiple concurrent calls
asterisk -rx "originate Local/100@default application ElevenLabsConvAI(sales_agent)"
```

## Deployment Procedures

### Development Deployment (Hot-reload)

```bash
#!/bin/bash
# dev-deploy.sh - Quick deployment script

echo "Building module..."
make clean && make debug

if [ $? -eq 0 ]; then
    echo "Deploying to Asterisk..."
    sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/
    
    echo "Reloading module..."
    asterisk -rx "module reload app_elevenlabs_convai"
    
    echo "Checking module status..."
    asterisk -rx "module show like elevenlabs"
    
    echo "Deployment complete!"
else
    echo "Build failed!"
    exit 1
fi
```

### Production Deployment

```bash
#!/bin/bash
# prod-deploy.sh - Production deployment script

echo "Building release version..."
make clean && make release

if [ $? -eq 0 ]; then
    echo "Backing up existing module..."
    sudo cp /usr/lib/asterisk/modules/app_elevenlabs_convai.so \
           /usr/lib/asterisk/modules/app_elevenlabs_convai.so.bak
    
    echo "Installing new module..."
    sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/
    
    echo "Reloading module..."
    asterisk -rx "module reload app_elevenlabs_convai"
    
    echo "Verifying deployment..."
    asterisk -rx "elevenlabs show agents"
    
    echo "Production deployment complete!"
else
    echo "Build failed!"
    exit 1
fi
```

### Configuration Deployment

```bash
# Deploy configuration changes
sudo cp elevenlabs.conf.example /etc/asterisk/elevenlabs.conf

# Edit with production values
sudo nano /etc/asterisk/elevenlabs.conf

# Hot-reload configuration
asterisk -rx "elevenlabs reload"

# Verify configuration
asterisk -rx "elevenlabs show agents"
```

## Advanced Development

### Adding New Features

#### 1. Code Structure
```c
// Follow existing patterns in app_elevenlabs_convai.c

// Add forward declarations
static int new_feature_function(struct elevenlabs_session *session);

// Implement function before main application function
static int new_feature_function(struct elevenlabs_session *session)
{
    // Implementation here
    return 0;
}

// Add to cleanup if needed
static void cleanup_session(struct elevenlabs_session *session)
{
    // Add cleanup for new feature
}
```

#### 2. Configuration Parameters
```c
// Add to agent_config structure
struct agent_config {
    // Existing fields...
    int new_parameter;
};

// Add parsing in load_config()
else if (strcmp(var->name, "new_parameter") == 0) {
    agent_configs[i].new_parameter = atoi(var->value);
}
```

#### 3. CLI Commands
```c
// Add new CLI command
static char *handle_elevenlabs_new_command(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
    // Implementation
    return CLI_SUCCESS;
}

// Register in cli_elevenlabs array
static struct ast_cli_entry cli_elevenlabs[] = {
    // Existing commands...
    AST_CLI_DEFINE(handle_elevenlabs_new_command, "New command description"),
};
```

### Performance Optimization

#### Memory Optimization
```bash
# Profile memory usage
valgrind --tool=massif asterisk -c

# Analyze memory leaks
valgrind --leak-check=full --show-leak-kinds=all asterisk -c
```

#### CPU Optimization
```bash
# Profile CPU usage
perf record -g asterisk -c
perf report

# Monitor real-time performance
top -p $(pidof asterisk)
```

### Code Quality

#### Static Analysis
```bash
# Run static analysis
cppcheck --enable=all app_elevenlabs_convai.c

# Check for security issues
flawfinder app_elevenlabs_convai.c
```

#### Code Formatting
```bash
# Format code (following Asterisk style)
astyle --style=linux --indent=tab app_elevenlabs_convai.c
```

## Troubleshooting Development Issues

### Build Issues

#### Missing Dependencies
```bash
# Check what's missing
make check-deps

# Install automatically
make install-deps
```

#### Compilation Errors
```bash
# Verbose build output
make VERBOSE=1

# Check specific library linking
gcc -v -lcurl -lwebsockets -lcjson -lssl -lcrypto
```

### Runtime Issues

#### Module Loading Fails
```bash
# Check module dependencies
ldd /usr/lib/asterisk/modules/app_elevenlabs_convai.so

# Check Asterisk modules directory
ls -la /usr/lib/asterisk/modules/app_elevenlabs_convai.so

# Check Asterisk logs
tail -f /var/log/asterisk/messages | grep -i "elevenlabs\|error\|warning"
```

#### Configuration Issues
```bash
# Test configuration syntax
asterisk -rx "elevenlabs reload"

# Show loaded configuration
asterisk -rx "elevenlabs show agents"

# Debug configuration loading
asterisk -rx "elevenlabs debug on"
asterisk -rx "elevenlabs reload"
```

#### WebSocket Connection Issues
```bash
# Test API connectivity
curl -v -H "xi-api-key: YOUR_API_KEY" \
  "https://api.elevenlabs.io/v1/convai/conversation/get-signed-url?agent_id=YOUR_AGENT_ID"

# Check firewall/network
telnet api.elevenlabs.io 443

# Monitor network traffic
tcpdump -i any host api.elevenlabs.io
```

## Performance Monitoring

### Module Metrics
```bash
# Check active sessions
asterisk -rx "core show channels"

# Monitor memory usage
asterisk -rx "core show sysinfo"

# Check module status
asterisk -rx "module show like elevenlabs"
```

### Audio Quality Monitoring
```bash
# Enable detailed logging
asterisk -rx "elevenlabs debug on"
asterisk -rx "core set verbose 5"

# Monitor audio frames
tail -f /var/log/asterisk/messages | grep -i "frame\|audio"
```

## Contributing

### Code Standards
- Follow Asterisk coding style (tabs, not spaces)
- All code and comments in English
- Use static functions for module-internal functions
- Declare variables at beginning of blocks
- Check all system call return values
- No memory leaks - every `ast_calloc` needs `ast_free`

### Commit Guidelines
```bash
# Make changes
git add app_elevenlabs_convai.c

# Commit with descriptive message
git commit -m "Add support for dynamic buffer sizing

- Allow runtime configuration of buffer size
- Add CLI command to modify buffer parameters
- Improve audio quality for varying network conditions"
```

### Testing Before Commit
```bash
# Full test suite
make test-all

# Memory leak check
valgrind --leak-check=full ./test_integration

# Code quality check
cppcheck app_elevenlabs_convai.c
```

This development guide ensures efficient development workflow with minimal disruption to running Asterisk systems while maintaining code quality and performance.