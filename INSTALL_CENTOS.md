# CentOS/RHEL Installation Guide

This guide provides specific instructions for installing the ElevenLabs ConvAI module on CentOS 7/8 and RHEL systems.

## Quick Installation

```bash
# Install basic dependencies and build tools
sudo yum install -y epel-release
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake wget libcurl-devel openssl-devel

# Use the automated installer
make install-deps

# Build the module
make

# Install to Asterisk
sudo make install
```

## Manual Installation (Step by Step)

### 1. Install Base Dependencies

```bash
# Add EPEL repository
sudo yum install -y epel-release

# Install development tools
sudo yum groupinstall -y "Development Tools"

# Install available packages
sudo yum install -y cmake wget libcurl-devel openssl-devel
```

### 2. Install libwebsockets from Source

```bash
# Download and build libwebsockets
cd /tmp
wget https://github.com/warmcat/libwebsockets/archive/v4.3.2.tar.gz -O libwebsockets.tar.gz
tar -xzf libwebsockets.tar.gz
cd libwebsockets-4.3.2

# Build and install
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DLWS_WITH_SSL=ON
make
sudo make install
sudo ldconfig
```

### 3. Install cJSON from Source

```bash
# Download and build cJSON
cd /tmp
wget https://github.com/DaveGamble/cJSON/archive/v1.7.16.tar.gz -O cjson.tar.gz
tar -xzf cjson.tar.gz
cd cJSON-1.7.16

# Build and install
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DENABLE_CJSON_UTILS=On -DENABLE_CJSON_TEST=Off
make
sudo make install
sudo ldconfig
```

### 4. Update Library Path

```bash
# Add /usr/local/lib to library path
echo '/usr/local/lib' | sudo tee /etc/ld.so.conf.d/local.conf
sudo ldconfig

# Verify libraries are found
ldconfig -p | grep -E "(libwebsockets|libcjson)"
```

### 5. Build the Module

```bash
cd app_elevenlabs_convai/

# Check dependencies
make check-deps

# Build the module
make

# Install to Asterisk
sudo make install
```

## Troubleshooting CentOS Issues

### Library Not Found During Build

```bash
# Check if libraries are properly installed
ls -la /usr/local/lib/libwebsockets*
ls -la /usr/local/lib/libcjson*
ls -la /usr/local/include/libwebsockets.h
ls -la /usr/local/include/cjson/cJSON.h

# Update library cache
sudo ldconfig

# Check library path
echo $LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

### CMake Version Issues

If you get CMake version errors:

```bash
# Install newer CMake from source
cd /tmp
wget https://github.com/Kitware/CMake/releases/download/v3.20.0/cmake-3.20.0.tar.gz
tar -xzf cmake-3.20.0.tar.gz
cd cmake-3.20.0
./bootstrap
make
sudo make install
```

### SSL/TLS Issues

```bash
# Make sure OpenSSL development headers are installed
sudo yum install -y openssl-devel

# Check OpenSSL version
openssl version

# If you need newer OpenSSL, compile from source:
cd /tmp
wget https://www.openssl.org/source/openssl-1.1.1k.tar.gz
tar -xzf openssl-1.1.1k.tar.gz
cd openssl-1.1.1k
./config --prefix=/usr/local/ssl --openssldir=/usr/local/ssl shared zlib
make
sudo make install
```

### Compilation Errors

```bash
# If you get include path errors, try:
export C_INCLUDE_PATH=/usr/local/include:$C_INCLUDE_PATH
export CPLUS_INCLUDE_PATH=/usr/local/include:$CPLUS_INCLUDE_PATH

# For library linking errors:
export LIBRARY_PATH=/usr/local/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# Then rebuild
make clean
make
```

## CentOS 7 Specific Notes

### GCC Version
CentOS 7 comes with GCC 4.8. For better compatibility:

```bash
# Install newer GCC from devtoolset
sudo yum install -y centos-release-scl
sudo yum install -y devtoolset-9-gcc devtoolset-9-gcc-c++

# Enable newer GCC for current session
scl enable devtoolset-9 bash

# Or permanently add to your profile
echo 'source /opt/rh/devtoolset-9/enable' >> ~/.bashrc
```

### SystemD Services
For CentOS 7 with systemd:

```bash
# Restart Asterisk service
sudo systemctl restart asterisk

# Check service status
sudo systemctl status asterisk

# View logs
sudo journalctl -u asterisk -f
```

## Verification

```bash
# Test that all libraries are found
ldd /usr/lib/asterisk/modules/app_elevenlabs_convai.so

# Load the module in Asterisk
asterisk -rx "module load app_elevenlabs_convai"

# Check module status
asterisk -rx "module show like elevenlabs"

# Test configuration
asterisk -rx "elevenlabs show agents"
```

## Alternative: RPM Packages

If building from source is problematic, you can try installing from alternative repositories:

```bash
# Try PowerTools repository (CentOS 8)
sudo dnf config-manager --set-enabled PowerTools
sudo dnf install libwebsockets-devel

# Or try RPM Fusion
sudo yum install -y https://download1.rpmfusion.org/free/el/rpmfusion-free-release-7.noarch.rpm
sudo yum install libwebsockets-devel
```

## Production Deployment on CentOS

```bash
# Create deployment script
cat > deploy_centos.sh << 'EOF'
#!/bin/bash
set -e

echo "Building ElevenLabs ConvAI module for CentOS..."

# Build with release flags
make clean
make release

# Backup existing module
if [ -f /usr/lib/asterisk/modules/app_elevenlabs_convai.so ]; then
    sudo cp /usr/lib/asterisk/modules/app_elevenlabs_convai.so \
           /usr/lib/asterisk/modules/app_elevenlabs_convai.so.bak.$(date +%Y%m%d_%H%M%S)
fi

# Install new module
sudo cp app_elevenlabs_convai.so /usr/lib/asterisk/modules/

# Reload module
asterisk -rx "module reload app_elevenlabs_convai"

echo "Deployment complete!"
EOF

chmod +x deploy_centos.sh
```

This guide should help you successfully install and run the ElevenLabs ConvAI module on CentOS systems.