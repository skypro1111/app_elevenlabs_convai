# Asterisk ElevenLabs ConvAI Integration

A production-ready Asterisk module that provides real-time bidirectional audio streaming between Asterisk channels and ElevenLabs ConvAI agents via WebSocket connections.

## Features

### Core Capabilities
- **Real-time Conversation AI**: Direct integration with ElevenLabs ConvAI agents
- **Bidirectional Audio Streaming**: Capture caller audio and play agent responses
- **Smart Interruption Handling**: Immediate response to user interruptions with precise audio queue management
- **Voice Activity Detection**: Real-time VAD score monitoring and detailed logging
- **Multiple Agent Support**: Configure multiple ConvAI agents in a single setup
- **Production Ready**: Comprehensive error handling and automatic resource cleanup

### Technical Features
- **WebSocket Protocol**: Secure WSS connections with automatic error recovery
- **Audio Processing**: 20ms frame timing with precise playback synchronization
- **Protocol Compliance**: Full ElevenLabs ConvAI WebSocket protocol implementation
- **Configuration Management**: Hot-reload configuration without restarting Asterisk
- **Debug Capabilities**: Comprehensive logging and CLI commands for troubleshooting
- **Crash Recovery**: Robust session management with automatic cleanup on WebSocket disconnection

### Advanced Features
- **Automatic Call Termination**: Calls end automatically when conversation completes
- **Event-based Interruptions**: Smart audio queue filtering based on event IDs
- **Thread-safe Operations**: Comprehensive mutex protection for all shared resources
- **Memory Management**: Efficient buffer management with automatic cleanup

## Installation

### Dependencies

#### Ubuntu/Debian
```bash
# Update package manager
sudo apt-get update

# Install build dependencies
sudo apt-get install -y \
    build-essential \
    libcurl4-openssl-dev \
    libwebsockets-dev \
    libcjson-dev \
    libssl-dev \
    pkg-config \
    asterisk-dev

# For Ubuntu 20.04+ you may need:
sudo apt-get install -y libwebsockets16
```

#### CentOS 7
```bash
# Enable EPEL repository
sudo yum install -y epel-release

# Install development tools
sudo yum groupinstall -y "Development Tools"

# Install dependencies
sudo yum install -y \
    libcurl-devel \
    openssl-devel \
    pkgconfig \
    asterisk-devel

# Install libwebsockets (may need to build from source on CentOS 7)
sudo yum install -y libwebsockets-devel || {
    echo "Building libwebsockets from source..."
    cd /tmp
    git clone https://github.com/warmcat/libwebsockets.git
    cd libwebsockets
    mkdir build && cd build
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr
    make && sudo make install
}

# Install cJSON
sudo yum install -y libcjson-devel || {
    echo "Building cJSON from source..."
    cd /tmp
    git clone https://github.com/DaveGamble/cJSON.git
    cd cJSON
    make && sudo make install
}
```

### Build and Installation

```bash
# Clone the repository
git clone <repository-url>
cd asterisk_elevenlabs/app_elevenlabs_convai/

# Build the module
make clean
make

# Install to Asterisk modules directory
sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/
# OR for some systems:
# sudo cp app_elevenlabs_convai.so /usr/lib64/asterisk/modules/

# Set proper permissions
sudo chown asterisk:asterisk /usr/lib/asterisk/modules/app_elevenlabs_convai.so
sudo chmod 755 /usr/lib/asterisk/modules/app_elevenlabs_convai.so
```

### Verification
```bash
# Check if the module loads
sudo asterisk -rx "module show like elevenlabs"

# Load the module if not already loaded
sudo asterisk -rx "module load app_elevenlabs_convai"
```

## Configuration

### 1. Create Configuration File

```bash
# Copy example configuration
sudo cp elevenlabs.conf.example /etc/asterisk/elevenlabs.conf
sudo chown asterisk:asterisk /etc/asterisk/elevenlabs.conf
sudo chmod 640 /etc/asterisk/elevenlabs.conf
```

### 2. Configure Agents

Edit `/etc/asterisk/elevenlabs.conf`:

```ini
[general]
debug=false

[sales_agent]
elevenlabs_api_key=sk-your-api-key-here
elevenlabs_get_signed_url_endpoint=https://api.elevenlabs.io/v1/convai/conversation/get-signed-url
elevenlabs_agent_id=agent_0301k2f1kzshe85t9w74t66vz1km
elevenlabs_send_caller_number=true
elevenlabs_audio_frames_buffer_ms=240

[support_agent]
elevenlabs_api_key=sk-your-api-key-here
elevenlabs_get_signed_url_endpoint=https://api.elevenlabs.io/v1/convai/conversation/get-signed-url
elevenlabs_agent_id=agent_another_agent_id_here
elevenlabs_send_caller_number=false
elevenlabs_audio_frames_buffer_ms=300
```

### 3. Dialplan Integration

Add to your dialplan (`/etc/asterisk/extensions.conf`):

```ini
[default]
; Simple setup - call ends automatically when conversation completes
exten => 100,1,Answer()
exten => 100,2,ElevenLabsConvAI(sales_agent)
; No hangup needed - module handles automatic termination

; Alternative with explicit wait (for testing)
exten => 200,1,Answer()
exten => 200,2,ElevenLabsConvAI(support_agent)
exten => 200,3,Wait(3600)  ; Optional: wait up to 1 hour
exten => 200,4,Hangup()
```

### 4. Load and Test

```bash
# Reload configuration
sudo asterisk -rx "module reload app_elevenlabs_convai"

# Reload dialplan
sudo asterisk -rx "dialplan reload"

# Enable debug for testing
sudo asterisk -rx "elevenlabs debug on"

# Test the setup
# Make a call to extension 100 or 200
```

## Architecture

### Call Flow

1. **Dialplan Execution**: `ElevenLabsConvAI(agent_name)` is called
2. **Configuration Loading**: Agent settings loaded from `elevenlabs.conf`
3. **API Authentication**: HTTP request to ElevenLabs for signed WebSocket URL
4. **WebSocket Connection**: Secure connection established to ConvAI agent
5. **Audio Streaming**: Bidirectional audio streaming begins
   - Incoming audio is buffered and sent to agent as base64 chunks
   - Agent responses are queued and played with 20ms frame timing
6. **Real-time Processing**: 
   - VAD scores monitored and logged in detail
   - Interruption events processed with smart queue management
   - Audio playback can be immediately stopped and filtered by event ID
7. **Session Management**: Application handles conversation in background via framehooks and threads
8. **Automatic Termination**: Call ends automatically when WebSocket closes
9. **Session Cleanup**: All resources cleaned up automatically

### Core Components

#### WebSocket Integration
- **libwebsockets**: Secure WebSocket client implementation  
- **Protocol Handling**: Ping/pong, audio messages, VAD scores, interruption events
- **Connection Management**: Automatic error recovery and resource cleanup
- **Event Processing**: Detailed logging for vad_score and interruption events

#### Audio Processing
- **Framehook System**: Real-time audio capture from Asterisk channels
- **Buffer Management**: Configurable audio buffering (200-500ms recommended)
- **Playback Threading**: Precise 20ms timing with interruption support
- **Base64 Encoding**: OpenSSL-based encoding for WebSocket transmission

#### Interruption Handling
- **Immediate Response**: Interruption flag stops current playback instantly
- **Smart Queue Filtering**: Removes audio chunks with event_id ≤ interruption event_id
- **Thread Synchronization**: Mutex-protected operations prevent race conditions
- **Conversation Flow**: Newer audio (after interruption) continues playing naturally

#### Configuration System
- **Multi-Agent Support**: Configure multiple ConvAI agents
- **Hot Reload**: Update configuration without restarting Asterisk
- **Validation**: Comprehensive parameter validation and error reporting

## CLI Commands

```bash
# Debug control
asterisk -rx "elevenlabs debug on"   # Enable debug logging
asterisk -rx "elevenlabs debug off"  # Disable debug logging

# Configuration management
asterisk -rx "elevenlabs reload"     # Reload configuration

# Status monitoring
asterisk -rx "elevenlabs show agents"  # Show configured agents

# Module management
asterisk -rx "module show like elevenlabs"  # Check module status
asterisk -rx "module load app_elevenlabs_convai"    # Load module
asterisk -rx "module unload app_elevenlabs_convai"  # Unload module
```

## Configuration Reference

### Agent Configuration Parameters

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `elevenlabs_api_key` | Yes | - | ElevenLabs API key from account dashboard |
| `elevenlabs_get_signed_url_endpoint` | Yes | - | API endpoint for WebSocket signed URLs |
| `elevenlabs_agent_id` | Yes | - | Unique ConvAI agent identifier |
| `elevenlabs_send_caller_number` | No | false | Send caller ID to agent as dynamic variable |
| `elevenlabs_audio_frames_buffer_ms` | No | 240 | Audio buffer duration in milliseconds |

### Buffer Duration Guidelines
- **200ms**: Faster response, may cause audio fragmentation
- **240ms**: Recommended default for most use cases  
- **300ms**: More stable for poor network conditions
- **500ms**: Maximum recommended for very unstable connections

## Logging and Monitoring

### VAD Score Logging
When debug is enabled, you'll see detailed VAD information:
```
=== VAD SCORE MESSAGE ===
VAD Score: 0.847
Timestamp: 2024-08-17T16:07:45.123Z
HIGH voice activity detected (score=0.847)
=== END VAD SCORE ===
```

### Interruption Event Logging
Detailed interruption processing information:
```
=== INTERRUPTION MESSAGE ===
Event ID: 46
User interrupted AI speech - stopping playback
Processing interruption for event_id=46 - clearing older audio
Removing audio chunk with event_id=42 (≤46)
Removing audio chunk with event_id=45 (≤46)
Interruption processing complete: removed 2 audio chunks with event_id ≤ 46
Remaining audio chunks in queue: 1
=== END INTERRUPTION ===
```

## Troubleshooting

### Common Issues

#### Module Loading Fails
```bash
# Check dependencies
ldd /usr/lib/asterisk/modules/app_elevenlabs_convai.so

# Check Asterisk logs
tail -f /var/log/asterisk/messages | grep -i elevenlabs

# Verify module file
ls -la /usr/lib/asterisk/modules/app_elevenlabs_convai.so
```

#### WebSocket Connection Issues
```bash
# Enable debug logging
asterisk -rx "elevenlabs debug on"

# Check configuration
asterisk -rx "elevenlabs show agents"

# Test API connectivity
curl -H "xi-api-key: YOUR_API_KEY" \
  "https://api.elevenlabs.io/v1/convai/conversation/get-signed-url?agent_id=YOUR_AGENT_ID"
```

#### Audio Quality Issues
- Increase buffer duration: `elevenlabs_audio_frames_buffer_ms=300`
- Check network latency to ElevenLabs servers
- Monitor Asterisk logs for frame processing warnings
- Ensure proper μ-law audio format support

#### Crash Issues (Resolved)
The module includes comprehensive crash protection:
- Automatic session cleanup on WebSocket disconnection
- Thread-safe resource management
- Proper channel reference counting
- Graceful handling of interrupted connections

### Debug Logging

Enable comprehensive logging:
```bash
# Enable module debug
asterisk -rx "elevenlabs debug on"

# Enable Asterisk verbose logging
asterisk -rx "core set verbose 4"

# Monitor logs in real-time
tail -f /var/log/asterisk/messages | grep -E "(elevenlabs|ElevenLabs)"

# Filter for specific events
tail -f /var/log/asterisk/messages | grep -E "(VAD SCORE|INTERRUPTION|WebSocket)"
```

## Performance Characteristics

- **Latency**: Sub-50ms audio processing for real-time conversation
- **Throughput**: Support for multiple concurrent conversations
- **Memory Usage**: Efficient buffer management with automatic cleanup
- **CPU Impact**: Minimal overhead with optimized audio processing
- **Crash Resistance**: Robust error handling and automatic recovery

## API Requirements

### ElevenLabs Account Setup
1. Create account at [elevenlabs.io](https://elevenlabs.io)
2. Create ConvAI agents in the dashboard
3. Generate API key with ConvAI access
4. Note agent IDs for configuration

### Network Requirements
- Outbound HTTPS (443) access to `api.elevenlabs.io`
- Outbound WSS (443) access to ElevenLabs WebSocket servers
- Stable internet connection (recommended: <100ms latency)

## Development

### Building from Source
```bash
# Development build with debug info
make clean
make DEBUG=1

# Hot-reload during development
make && sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/ && asterisk -rx "module reload app_elevenlabs_convai"
```

### Testing
```bash
# Test build
make test

# Memory leak testing
valgrind --tool=memcheck --leak-check=full asterisk -vvv
```

## Support

### Documentation
- [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) - Detailed build instructions
- [dependencies.md](../dependencies.md) - Library requirements and installation
- [development_plan.md](../development_plan.md) - Technical specifications

### Troubleshooting Steps
1. Check Asterisk logs: `/var/log/asterisk/messages`
2. Enable debug mode: `elevenlabs debug on`
3. Verify API credentials and network connectivity
4. Test with simple dialplan configurations first
5. Check module loading: `module show like elevenlabs`

## License

GNU General Public License Version 2 (GPL-2.0)

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation.

## Acknowledgments

- Built on the Asterisk telephony framework
- Integrates with ElevenLabs ConvAI platform
- Uses industry-standard libraries: libcurl, libwebsockets, cJSON, OpenSSL
- Implements robust crash protection and resource management