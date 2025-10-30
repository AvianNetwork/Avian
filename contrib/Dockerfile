FROM amd64/ubuntu:22.04 AS base

EXPOSE 8766/tcp
EXPOSE 8767/tcp

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    bash wget net-tools libminiupnpc17 \
    libevent-2.1 libevent-pthreads-2.1 \
    libboost-system1.74.0 libboost-filesystem1.74.0 libboost-chrono1.74.0 \
    libboost-program-options1.74.0 libboost-thread1.74.0 \
    libzmq5 && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

FROM base AS build

# Install build dependencies (gcc/g++ 11 is default in 22.04)
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential libtool autotools-dev automake \
    pkg-config libssl-dev libevent-dev bsdmainutils python3 \
    libboost-system-dev libboost-filesystem-dev libboost-chrono-dev \
    libboost-program-options-dev libboost-test-dev libboost-thread-dev \
    libzmq3-dev libminiupnpc-dev git && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Copy source dir
COPY . /home/avian/build/Avian/
WORKDIR /home/avian/build/Avian
# Build db4 from source
WORKDIR /home/avian/build/Avian/contrib
RUN ./install_db4.sh ../../

# Return to main source dir
WORKDIR /home/avian/build/Avian

# Build Aviancore
RUN ./autogen.sh && \
    ./configure --disable-tests --enable-sse2 \
        BDB_LIBS="-L/home/avian/build/db4/lib -ldb_cxx-4.8" \
        BDB_CFLAGS="-I/home/avian/build/db4/include" \
        --with-gui=no \
        CXXFLAGS="-std=c++17" \
        LDFLAGS="-std=c++17" && \
    make -j$(nproc) V=1

FROM base AS final

# Add our service account user
RUN useradd -ms /bin/bash avian && \
	mkdir /var/lib/avian && \
	chown avian:avian /var/lib/avian && \
	ln -s /var/lib/avian /home/avian/.avian && \
	chown -h avian:avian /home/avian/.avian

VOLUME /var/lib/avian

# Copy the compiled binaries from the build stage
COPY --from=build /home/avian/build/Avian/src/aviand /usr/local/bin/aviand
COPY --from=build /home/avian/build/Avian/src/avian-cli /usr/local/bin/avian-cli

WORKDIR /home/avian
USER avian

CMD ["/usr/local/bin/aviand", "-datadir=/var/lib/avian", "-printtoconsole", "-onlynet=ipv4"]