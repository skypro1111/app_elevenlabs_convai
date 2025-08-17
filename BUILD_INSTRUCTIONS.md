# Asterisk Build System Integration

Since Asterisk's build system doesn't automatically pick up external library dependencies, you need to manually add the linking flags.

## Method 1: Environment Variables (Recommended)

```bash
# Set the library flags before building
export LIBS="-lcjson -lwebsockets -lcurl -lssl -lcrypto"
export LDFLAGS="-L/usr/local/lib64"

# Then rebuild
cd /home/asterisk/install/asterisk-20.1.0
make clean
make install
```

## Method 2: Direct Make Command

```bash
cd /home/asterisk/install/asterisk-20.1.0
make LIBS="-lcjson -lwebsockets -lcurl -lssl -lcrypto" LDFLAGS="-L/usr/local/lib64" install
```

## Method 3: Asterisk Configuration

```bash
# Add to Asterisk's configure
./configure --with-curl --with-ssl
export LIBS="-lcjson -lwebsockets"
make install
```

## Verification

After building, verify the linking:
```bash
ldd /usr/lib/asterisk/modules/app_elevenlabs_convai.so | grep -E "(cjson|websockets|curl)"
```

You should see:
```
libcjson.so.1 => /usr/local/lib64/libcjson.so.1
libwebsockets.so => /usr/local/lib64/libwebsockets.so
libcurl.so.4 => /usr/lib64/libcurl.so.4
```