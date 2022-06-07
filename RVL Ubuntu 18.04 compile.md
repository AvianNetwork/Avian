
Note
---------------------

This is the method I use to compile the RVL repo on my laptop natively running Ubuntu 18.04.

Your mileage may, and quite likely will, vary.

dependencies
====================

basic depends:
----------------------------

`
sudo apt install 
build-essential
libssl-dev
libboost-chrono-dev
libboost-filesystem-dev
libboost-program-options-dev
libboost-system-dev
libboost-thread-dev
libboost-test-dev
qtbase5-dev
qttools5-dev
bison
libexpat1-dev
libdbus-1-dev
libfontconfig-dev
libfreetype6-dev
libice-dev
libsm-dev
libx11-dev
libxau-dev
libxext-dev
libevent-dev
libxcb1-dev
libxkbcommon-dev
libminiupnpc-dev
libprotobuf-dev
libqrencode-dev
xcb-proto
x11proto-xext-dev
x11proto-dev
xtrans-dev
zlib1g-dev
libczmq-dev
autoconf
automake
libtool
protobuf-compiler
`

boost library:
----------------------------

`
sudo apt-get install libboost-all-dev
`

qt depends (for GUI wallet):
----------------------------

`
sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler libqrencode-dev libcurl4-openssl-dev
`

optional depends:
----------------------------

`
sudo apt-get install libminiupnpc-dev libzmq3-dev
`

compile process
====================

Start in $HOME


`mkdir src`

`cd src`

`git clone https://github.com/RavencoinLite/RavencoinLite`

`cd RavencoinLite`

`RAVEN_ROOT=$(pwd)`

`BDB_PREFIX="${RAVEN_ROOT}/db4"`

`mkdir -p $BDB_PREFIX`

`wget 'http://download.oracle.com/berkeley-db/db-4.8.30.NC.tar.gz'`

`echo '12edc0df75bf9abd7f82f821795bcee50f42cb2e5f76a6a281b85732798364ef  db-4.8.30.NC.tar.gz' | sha256sum -c`

`tar -xzvf db-4.8.30.NC.tar.gz`

`cd db-4.8.30.NC/build_unix/`

`../dist/configure --enable-cxx --disable-shared --with-pic --prefix=$BDB_PREFIX`

`make install`

`cd $RAVEN_ROOT`

`./autogen.sh`

`./configure LDFLAGS="-L${BDB_PREFIX}/lib/" CPPFLAGS="-I${BDB_PREFIX}/include/" # (other args...)`


`make -j8`  # 8 for 8 cpu threads, adjust to fit your setup


afterword:
----------------------------

run GUI wallet using `src/qt/avian-qt`

run aviand and avian-cli using `src/aviand` and `src/avian-cli` respectively

optional:
----------------------------

`sudo make install` # if you want to install the relevant binaries to /usr/local/bin 
