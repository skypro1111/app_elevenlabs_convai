# Asterisk ElevenLabs ConvAI Integration

A production-ready Asterisk module that provides real-time bidirectional audio streaming between Asterisk channels and ElevenLabs ConvAI agents via WebSocket connections.

## Features

### Core Capabilities
- **Real-time Conversation AI**: Direct integration with ElevenLabs ConvAI agents
- **Bidirectional Audio Streaming**: Capture caller audio and play agent responses
- **Low Latency Processing**: Optimized audio buffering with configurable timing
- **Multiple Agent Support**: Configure multiple ConvAI agents in a single setup
- **Production Ready**: Comprehensive error handling and resource management

### Technical Features
- **WebSocket Protocol**: Secure WSS connections with automatic reconnection
- **Audio Processing**: 20ms frame timing with precise playback synchronization
- **Protocol Compliance**: Full ElevenLabs ConvAI WebSocket protocol implementation
- **Configuration Management**: Hot-reload configuration without restarting Asterisk
- **Debug Capabilities**: Comprehensive logging and CLI commands for troubleshooting

## Quick Start

### 1. Installation

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install libcurl4-openssl-dev libwebsockets-dev libcjson-dev libssl-dev

# Clone and build
cd app_elevenlabs_convai/
make install-deps  # Install dependencies automatically
make               # Build the module
sudo make install  # Install to Asterisk modules directory
```

### 2. Configuration

Copy and configure the ElevenLabs settings:

```bash
sudo cp elevenlabs.conf.example /etc/asterisk/elevenlabs.conf
sudo nano /etc/asterisk/elevenlabs.conf
```

Example configuration:
```ini
[general]
debug=false

[sales_agent]
elevenlabs_api_key=sk-your-api-key-here
elevenlabs_get_signed_url_endpoint=https://api.elevenlabs.io/v1/convai/conversation/get-signed-url
elevenlabs_agent_id=agent_0301k2f1kzshe85t9w74t66vz1km
elevenlabs_send_caller_number=true
elevenlabs_audio_frames_buffer_ms=240
```

### 3. Dialplan Integration

Add to your dialplan (`/etc/asterisk/extensions.conf`):

```ini
[default]
exten => 100,1,Answer()
exten => 100,2,ElevenLabsConvAI(sales_agent)
exten => 100,3,Hangup()

exten => 200,1,Answer()
exten => 200,2,ElevenLabsConvAI(support_agent)
exten => 200,3,Hangup()
```

### 4. Load the Module

```bash
# Load the module in Asterisk CLI
asterisk -rx "module load app_elevenlabs_convai"

# Reload dialplan
asterisk -rx "dialplan reload"
```

## Architecture

### Project Structure
```
asterisk_elevenlabs/
├── app_elevenlabs_convai/          # Main module implementation
│   ├── app_elevenlabs_convai.c     # Core module (1,600+ lines)
│   ├── Makefile                    # Build system
│   ├── elevenlabs.conf.example     # Configuration template
│   └── tests/                      # Test framework
├── app_forkstream/                 # Reference implementation
├── asterisk_context_files/         # Asterisk API headers
├── dependencies.md                 # Library requirements
├── development_plan.md             # Technical specifications
└── CLAUDE.md                       # Development guidance
```

### Core Components

#### WebSocket Integration
- **libwebsockets**: Secure WebSocket client implementation
- **Protocol Handling**: Ping/pong, audio messages, interruption events
- **Connection Management**: Automatic reconnection and error recovery

#### Audio Processing
- **Framehook System**: Real-time audio capture from Asterisk channels
- **Buffer Management**: Configurable audio buffering (200-500ms recommended)
- **Playback Threading**: Precise 20ms timing for natural conversation flow
- **Base64 Encoding**: OpenSSL-based encoding for WebSocket transmission

#### Configuration System
- **Multi-Agent Support**: Configure multiple ConvAI agents
- **Hot Reload**: Update configuration without restarting Asterisk
- **Validation**: Comprehensive parameter validation and error reporting

## Call Flow

1. **Dialplan Execution**: `ElevenLabsConvAI(agent_name)` is called
2. **Configuration Loading**: Agent settings loaded from `elevenlabs.conf`
3. **API Authentication**: HTTP request to ElevenLabs for signed WebSocket URL
4. **WebSocket Connection**: Secure connection established to ConvAI agent
5. **Audio Streaming**: Bidirectional audio streaming begins
   - Incoming audio is buffered and sent to agent as base64 chunks
   - Agent responses are queued and played with 20ms frame timing
6. **Protocol Handling**: Ping/pong and interruption events processed
7. **Session Cleanup**: All resources cleaned up on call termination

## CLI Commands

```bash
# Debug control
asterisk -rx "elevenlabs debug on"   # Enable debug logging
asterisk -rx "elevenlabs debug off"  # Disable debug logging

# Configuration management
asterisk -rx "elevenlabs reload"     # Reload configuration

# Status monitoring
asterisk -rx "elevenlabs show agents"  # Show configured agents
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

## Performance Characteristics

- **Latency**: Sub-50ms audio processing for real-time conversation
- **Throughput**: Support for multiple concurrent conversations
- **Memory Usage**: Efficient buffer management with automatic cleanup
- **CPU Impact**: Minimal overhead with optimized audio processing

## Troubleshooting

### Common Issues

#### Module Loading Fails
```bash
# Check dependencies
make check-deps

# Verify Asterisk modules directory
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

### Debug Logging

Enable verbose logging for troubleshooting:
```bash
asterisk -rx "elevenlabs debug on"
asterisk -rx "core set verbose 4"
tail -f /var/log/asterisk/messages | grep -i elevenlabs
```

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

See [DEV.md](DEV.md) for detailed development instructions including:
- Building from source
- Hot-loading modules during development
- Testing procedures
- Contributing guidelines

## Support

### Documentation
- [dependencies.md](dependencies.md) - Library requirements and installation
- [development_plan.md](development_plan.md) - Technical specifications
- [DEV.md](DEV.md) - Development workflow

### Troubleshooting
1. Check Asterisk logs: `/var/log/asterisk/messages`
2. Enable debug mode: `elevenlabs debug on`
3. Verify API credentials and network connectivity
4. Test with simple dialplan configurations first

## License

GNU General Public License Version 2 (GPL-2.0)

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation.

## Acknowledgments

- Built on the Asterisk telephony framework
- Integrates with ElevenLabs ConvAI platform
- Uses industry-standard libraries: libcurl, libwebsockets, cJSON, OpenSSL