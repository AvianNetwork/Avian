<h1 align="center">
Avian Network [AVN]  
<br/><br/>
<img src="./src/qt/res/icons/avian.png" alt="Avian" width="300"/>
</h1>

<div align="center">

[![Avian](https://img.shields.io/badge/Avian-Network-blue.svg)](https://avn.network)
[![Downloads](https://img.shields.io/github/downloads/AvianNetwork/Avian/total)](https://avn.network)
[![C/C++ CI](https://github.com/AvianNetwork/Avian/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/AvianNetwork/Avian/actions/workflows/c-cpp.yml)

</div>

# What is Avian?

Avian Network is a proof-of-work secured blockchain designed
for efficient and interoperable asset management. 
The assets can be automated using Avian Flight Plans allowing the creation of decentralized applications. 
The network prioritizes usability, automation, and low fees to make asset minting and management
simple, affordable, and secure. The network's economy runs on AVN, our
native coin that can be mined on a dual algorithm setup using either GPUs
or CPUs.

For more information, as well as an immediately useable, binary version of
the Avian Core software, see https://avn.network

# License ⚖️
Avian Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

# Development

Avian is open source and community driven. The development process is publicly visible and anyone can contribute.

### Branches

There are **2** types of branches:

  - master: *Stable*, contains the code of the latest version.
  - dev: *Unstable*, contains new code for planned releases. Format: `<version>`, Example: `4.1`

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/AvianNetwork/Avian/tags) are created
regularly to indicate new official, stable release versions of Avian Core.

### Contributing

If you find a bug with this software, please report it using the issue system.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

### Running on Testnet

Testnet is up and running and available to use during development. It is recommended to run the testnet using the `-maxtipage` parameter in order to connect to the test network if there has been no recently mined blocks.

Use this command to initially start `aviand` on the testnet. <code>./aviand -testnet -maxtipage=259200</code>

### Running on Mainnet

Use this command to start `aviand` (CLI) on the mainnet.
<code>./aviand</code>

Use this command to start `avian-qt` (GUI) on the mainnet.
<code>./avian-qt</code>

# Building

#### Linux

```shell
# The following packages are needed (Debian-based distros):
sudo apt-get install build-essential pkg-config libc6-dev m4 g++-multilib autoconf libtool ncurses-dev unzip git python python-zmq zlib1g-dev wget libcurl4-gnutls-dev bsdmainutils automake curl
```

```shell
# Clone Avian repo
git clone https://github.com/AvianNetwork/Avian
cd Avian

# Build Avian Core
./autogen.sh
./configure
# -j8 = using 8 threads for the compilation - replace 8 with number of threads you want to use
make -j8

# Note: This can take some time.
```

#### Windows

Use a debian cross-compilation setup OR *mingw-w64* for windows and run:

```shell
# If using linux cross-build (Debian-based distros)
sudo apt-get install build-essential pkg-config libc6-dev m4 g++-multilib autoconf libtool ncurses-dev unzip git python python-zmq zlib1g-dev wget libcurl4-gnutls-dev bsdmainutils automake curl cmake mingw-w64

# Clone Avian repo
git clone https://github.com/AvianNetwork/Avian
cd Avian

# Build Avian Core
./autogen.sh
./configure
# -j8 = using 8 threads for the compilation - replace 8 with number of threads you want to use
make -j8

# Note: This can take some time.
```

#### OSX **(Native)**
Ensure you have [brew](https://brew.sh) and Command Line Tools installed.

```shell
# Install brew
/usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"

# Install Xcode, opens a pop-up window to install CLT without installing the entire Xcode package
xcode-select --install 

# Update brew and install dependencies
brew update
brew upgrade
brew install autoconf autogen automake
brew install binutils
brew install protobuf
brew install coreutils
brew install wget

# Clone Avian repo
git clone https://github.com/AvianNetwork/Avian
cd Avian

# Build Avian Core
./autogen.sh
./configure
# -j8 = using 8 threads for the compilation - replace 8 with number of threads you want to use
make -j8

# Note: This can take some time.
```

# Avian History 
*Note: This is orignal description for when Avian was called RVL (Ravencoin Lite)*

Avian is a digital peer-to-peer network for the facilitation of asset transfer.

Having started development on August 12th of 2021, and active on mainnet since September 1st, Avian (AVN) is a fork of Ravencoin Lite (RVL) which is a fork of Ravencoin Classic (RVC), aimed primarily at bringing the means of development back into the hands of the community after RVC had been abandoned by its creators. With the RVC GitHub locked, and software in disrepair, RVL sought to improve upon the existing foundations by implementing the necessary updates and bug fixes needed to bring the original x16r fork of Ravencoin Classic up to par with modern cryptocurrencies.

This project is being spearheaded by a small group of enthusiasts, representing the interests of the actual community, as opposed to the original fork, which was created and maintained by Chinese ASIC manufacturers looking to make a return on their machines after AVN switched to the x16rv2 algorithm. As such, we are always seeking people wishing to contribute their experience and know-how to the development of Avian.
Our original goal was to stick to the x16r algorithm as previously declared by the RVC project, however, we were aware of the potential threat that ASICs pose to the network, and looked to mitigate it while staying true to our roots. All existing bugs have been fixed, and asset functionality is being developed. We implemented a secondary CPU algorithm, somewhat akin to Myriad (XMY) called MinotaurX which was developed by LitecoinCash (LCC), to help decentralize the mining of AVN without resorting to replacing the primary algorithm.

I would like to reiterate that there are no professional developers on the dev team as of this date; we are merely a group of dedicated community members who got tired of waiting around for change and took matters into their own hands. As such, the speed of development will greatly depend on the number of contributors. Let us come together as a community and help give AVN the wings it needs to soar high into the sky!
Thank you to the Bitcoin developers and the rest of the open-source community for your hard work in the crypto space.
The Avian project is launched based on the hard work and continuous effort of over 400 Bitcoin developers who made over 14,000 commits over the life to date of the Bitcoin project. We are eternally grateful to you for your efforts and diligence in making a secure network and for your support of free and open-source software development. The Avian experiment is made on the foundation you built and we will continue to expand upon the foundations laid.
