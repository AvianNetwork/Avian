
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
sudo apt-get install libqt5gui5 libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler libqrencode-dev
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


download the db 4.8 folder () and place it in src alongside RavencoinLite


`./autogen.sh`

`export BDB_PREFIX=$HOME/src/db4`

`./configure BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" BDB_CFLAGS="-I${BDB_PREFIX}/include" --prefix=/usr/local`

`make -j8`  # 8 for 8 cpu threads, adjust to fit your setup


afterword:
----------------------------

run GUI wallet using `src/qt/raven-qt`

run ravend and raven-cli using `src/ravend` and `src/raven-cli` respectively

`sudo make install` does not work, at least on my machine, so you will have to run everything using the above commands until a fix is found
