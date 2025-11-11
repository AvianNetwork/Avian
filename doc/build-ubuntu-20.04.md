# Building Avian Core on Ubuntu 20.04

Ubuntu 20.04 requires some additional configuration to build Avian Core, particularly for the depends system which needs GCC 10 for proper compatibility. Note that Ubuntu 20.04 standard support has ended; consider upgrading to a newer Ubuntu LTS version for ongoing security updates.

## Prerequisites

Install required build dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
    autoconf \
    automake \
    autotools-dev \
    binutils \
    bsdmainutils \
    build-essential \
    ca-certificates \
    curl \
    g++-10 \
    gcc-10 \
    git \
    gnupg \
    libtool \
    pkg-config \
    python3 \
    rename \
    xkb-data \
    zip \
    bison
```

## Building from Source

### 1. Clone the Repository

```bash
git clone https://github.com/AvianNetwork/Avian.git
cd Avian
```

### 2. Build Dependencies

Ubuntu 20.04's default GCC version is insufficient for the depends system. Use GCC 10 explicitly:

```bash
CC=gcc-10 CXX=g++-10 make -C depends HOST=x86_64-linux-gnu -j"$(nproc)"
```

This step downloads and compiles all required dependencies (Qt 5.12.11, Boost, OpenSSL, etc.). It may take 10-20 minutes depending on your system.

### 3. Run Autotools

```bash
./autogen.sh
```

### 4. Configure the Build

```bash
CONFIG_SITE=$PWD/depends/x86_64-linux-gnu/share/config.site ./configure --prefix=/ \
    --disable-ccache \
    --disable-maintainer-mode \
    --disable-dependency-tracking \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    --disable-bench \
    --disable-gui-tests \
    --disable-tests \
    --enable-sse2 \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g" \
    LDFLAGS="-static-libstdc++"
```

### 5. Compile

```bash
make -j"$(nproc)"
```

This will compile the daemon (`aviand`), wallet RPC tool (`avian-cli`), and GUI wallet (`avian-qt`).

### 6. Optional: Build a Distribution Package

```bash
make install DESTDIR=$(pwd)/install
```

---

## Building Without Wallet Support

If you only need the daemon without wallet functionality (but with GUI):

```bash
./autogen.sh

CONFIG_SITE=$PWD/depends/x86_64-linux-gnu/share/config.site ./configure --prefix=/ \
    --disable-ccache \
    --disable-maintainer-mode \
    --disable-dependency-tracking \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    --disable-bench \
    --disable-gui-tests \
    --disable-tests \
    --disable-wallet \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g" \
    LDFLAGS="-static-libstdc++"

make -j"$(nproc)"
```

---

## Building Without GUI

If you only need the daemon and CLI tools without the GUI (but with wallet functionality):

### Build Dependencies (without Qt)

Building without the GUI skips Qt compilation, which significantly reduces build time:

```bash
CC=gcc-10 CXX=g++-10 make -C depends HOST=x86_64-linux-gnu -j"$(nproc)" NO_QT=1
```

### Configure and Compile

```bash
./autogen.sh

CONFIG_SITE=$PWD/depends/x86_64-linux-gnu/share/config.site ./configure --prefix=/ \
    --disable-ccache \
    --disable-maintainer-mode \
    --disable-dependency-tracking \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    --disable-bench \
    --disable-gui-tests \
    --disable-tests \
    --with-gui=no \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g" \
    LDFLAGS="-static-libstdc++"

make -j"$(nproc)"
```

This builds the daemon (`aviand`) and CLI tool (`avian-cli`) with wallet support but skips Qt GUI compilation.

---

## Building Daemon Only (No Wallet, No GUI)

If you only need a node without wallet or GUI functionality:

### Build Dependencies (without Qt)

```bash
CC=gcc-10 CXX=g++-10 make -C depends HOST=x86_64-linux-gnu -j"$(nproc)" NO_QT=1
```

### Configure and Compile

```bash
./autogen.sh

CONFIG_SITE=$PWD/depends/x86_64-linux-gnu/share/config.site ./configure --prefix=/ \
    --disable-ccache \
    --disable-maintainer-mode \
    --disable-dependency-tracking \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    --disable-bench \
    --disable-gui-tests \
    --disable-tests \
    --disable-wallet \
    --with-gui=no \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g" \
    LDFLAGS="-static-libstdc++"

make -j"$(nproc)"
```

---

## Cross-Compiling for ARM64 (aarch64)

To build for ARM64 systems:

```bash
CC=gcc-10 CXX=g++-10 make -C depends HOST=aarch64-linux-gnu -j"$(nproc)"

CONFIG_SITE=$PWD/depends/aarch64-linux-gnu/share/config.site ./configure --prefix=/ \
    --disable-ccache \
    --disable-maintainer-mode \
    --disable-dependency-tracking \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    --disable-bench \
    --disable-gui-tests \
    --disable-tests \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g" \
    LDFLAGS="-static-libstdc++"

make -j"$(nproc)"
```

---

## Cross-Compiling for ARM32 (armhf)

To build for ARM 32-bit systems:

```bash
apt-get install -y g++-arm-linux-gnueabihf gcc-arm-linux-gnueabihf

CC=gcc-10 CXX=g++-10 make -C depends HOST=arm-linux-gnueabihf -j"$(nproc)"

CONFIG_SITE=$PWD/depends/arm-linux-gnueabihf/share/config.site ./configure --prefix=/ \
    --disable-ccache \
    --disable-maintainer-mode \
    --disable-dependency-tracking \
    --enable-glibc-back-compat \
    --enable-reduce-exports \
    --disable-bench \
    --disable-gui-tests \
    --disable-tests \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g" \
    LDFLAGS="-static-libstdc++"

make -j"$(nproc)"
```

---

## Testing Your Build

### Test the Daemon

```bash
./src/aviand --version
```

### Test the CLI Tool

```bash
./src/avian-cli --version
```

### Test the GUI (if built)

```bash
./src/qt/avian-qt &
```

---

## Troubleshooting

### "gcc-10: command not found"
Ensure you installed `gcc-10` and `g++-10`:
```bash
sudo apt-get install -y gcc-10 g++-10
```

### Build fails with "Qt5Core not found"
The depends system should have built Qt. If this error persists, try:
```bash
rm -rf depends/built
CC=gcc-10 CXX=g++-10 make -C depends HOST=x86_64-pc-linux-gnu -j"$(nproc)"
```

### Out of disk space during depends build
The depends system can use 5-10GB of disk space. Ensure you have sufficient space:
```bash
df -h
```

### Build hangs or takes very long
This is normal for the first build. The depends system downloads and compiles many libraries. Use `-j$(nproc)` for parallel builds, which significantly speeds up the process.

---

## Next Steps

- See [INSTALL.md](../INSTALL.md) for general installation instructions
- See [README.md](../README.md) for usage documentation
- Join the community on GitHub Discussions for questions and support

