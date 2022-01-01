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

Avian is an experimental digital currency that enables instant payments to
anyone, anywhere in the world. Avian uses peer-to-peer technology to operate
with no central authority: managing transactions and issuing money are carried
out collectively by the network. Avian Core is the name of open source
software which enables the use of this currency.

For more information, as well as an immediately useable, binary version of
the Avian Core software, see https://avn.network

# License ⚖️
Avian Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

# Development Process

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/AvianNetwork/Avian/tags) are created
regularly to indicate new official, stable release versions of Avian Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

# Testing

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

Testnet is now up and running and available to use during development. There is an issue when connecting to the testnet that requires the use of the -maxtipage parameter in order to connect to the test network initially. After the initial launch the -maxtipage parameter is not required.

Use this command to initially start aviand on the testnet. <code>./aviand -testnet -maxtipage=259200</code>


# Running on Mainnet

Use this command to start aviand on the mainnet.
<code>./aviand -seednode=dnsseed.avian.info</code>

# Building

#### Linux

```shell
#The following packages are needed:
sudo apt-get install build-essential pkg-config libc6-dev m4 g++-multilib autoconf libtool ncurses-dev unzip git python python-zmq zlib1g-dev wget libcurl4-gnutls-dev bsdmainutils automake curl
```

```shell
git clone https://github.com/AvianNetwork/Avian
cd avian
./autogen.sh
./configure
# -j8 = using 8 threads for the compilation - replace 8 with number of threads you want to use
make -j8
#This can take some time.
```

#### OSX (Cross-compile using Linux)

Before start, read the following docs: [depends](https://github.com/bitcoin/bitcoin/blob/master/depends/README.md) and [macdeploy](https://github.com/bitcoin/bitcoin/blob/master/contrib/macdeploy/README.md).

Install dependencies:
```
sudo apt-get install curl librsvg2-bin libtiff-tools bsdmainutils cmake imagemagick libcap-dev libz-dev libbz2-dev python3-setuptools libtinfo5 xorriso
```

Place prepared SDK file `Xcode-11.3.1-11C505-extracted-SDK-with-libcxx-headers.tar.gz` in repo root, run `./zcutil/build-mac-cross.sh` in the terminal to build.

#### OSX (Native)
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
cd avian
./autogen.sh
./configure
# -j8 = using 8 threads for the compilation - replace 8 with number of threads you want to use
make -j8
# This can take some time.
```

#### Windows
Use a debian cross-compilation setup OR mingw for windows and run:
```shell
# if using linux cross-build
sudo apt-get install build-essential pkg-config libc6-dev m4 g++-multilib autoconf libtool ncurses-dev unzip git python python-zmq zlib1g-dev wget libcurl4-gnutls-dev bsdmainutils automake curl cmake mingw-w64
git clone https://github.com/AvianNetwork/Avian
cd avian
./autogen.sh
./configure
# -j8 = using 8 threads for the compilation - replace 8 with number of threads you want to use
make -j8
#This can take some time.
```

# About Avian

A digital peer to peer network for the facilitation of asset transfer.

Having started development on August 12th of 2021, and active on mainnet since September 1st, Avian Lite (RVL) is a fork of Avian Classic (RVC), aimed primarily at bringing the means of development back into the hands of the community after RVC has been abandoned by its creators. With the RVC github locked, and software in disrepair, RVL seeks to improve upon the existing foundations by implementing the necessary updates and bug fixes need to bring the original x16r fork of Avian up to par with modern cryptocurrencies. 

This project is being spearheaded by a small group of enthusiasts, representing the interests of the actual RVC community, as opposed to the original fork, which was created and maintained by Chinese ASIC manufacturers looking to make a return on their machines after AVN switched to the x16rv2 algorithm. As such, we are always seeking for people wishing to contribute their experience and knowhow to the development of Avian Lite.

Our goal is to stick to the x16r algorithm as previously declared by the RVC project, however, we are aware of the potential threat that ASICs pose to the network, and are looking to mitigate it while staying true to our roots. Once all existing bugs have been fixed, and asset functionality restored, we aim to implement a secondary CPU algorithm, somewhat akin to Myriad (XMY), in order to help decentralize the mining of RVL without resorting to replacing the primary algo.

I would like to reiterate that there are no professional developers on the dev team as of this date; we are merely a group of dedicated community members who got tired of waiting around for change and took matters into their own hands. As such, the speed of development will greatly depend on the amount of contributors. Let us come together as a community, and make the restoration of RVC a reality!

Thank you to the Bitcoin developers.

The Avian project is launched based on the hard work and continuous effort of over 400 Bitcoin developers who made over 14,000 commits over the life to date of the Bitcoin project. We are eternally grateful to you for your efforts and diligence in making a secure network and for their support of free and open source software development.  The Avian experiment is made on the foundation you built.


# Abstract

Avian aims to implement a blockchain which is optimized specifically for the use case of transferring assets such as securities from one holder to another. Based on the extensive development and testing of Bitcoin, Avian is built on a fork of the Bitcoin code. Key changes include a faster block reward time and a change in the number, but not weighed distribution schedule, of coins. Avian is free and open source and will be issued and mined transparently with no pre-mine, developer allocation or any other similar set aside. Avian is intended to prioritize user control, privacy and censorship resistance and be jurisdiction agnostic while allowing simple optional additional features for users based on need.



A blockchain is a ledger showing the value of something and allowing it to be transferred to someone else. Of all the possible uses for blockchains, the reporting of who owns what is one of the core uses of the technology.  This is why the first and most successful use case for blockchain technology to date has been Bitcoin.

The success of the Ethereum ERC 20 token shows the demand for tokenized assets that use another blockchain.  Tokens offer many advantages to traditional shares or other participation mechanisms such as faster transfer, possibly increased user control and censorship resistance and reduction or elimination of the need for trusted third parties.

Bitcoin also has the capability of serving as the rails for tokens by using projects such as Omnilayer, RSK or Counterparty. However, neither Bitcoin nor Ethereum was specifically designed for facilitating ownership of other assets.

Avian is designed to be a use case specific blockchain designed to efficiently handle one specific function: the transfer of assets from one party to another., while providing a secure network for transactions.

Bitcoin is and always should be focused on its goals of being a better form of money. Bitcoin developers will unlikely prioritize improvements or features which are specifically beneficial to the facilitation of token transfers.  One goal of the Avian project is to see if a use case specific blockchain and development effort can create code which can either improve existing structures like Bitcoin or provide advantages for specific use cases.

In the new global economy, borders and jurisdictions will be less relevant as more assets are tradable and trade across borders is increasingly frictionless. In an age where people can move significant amounts of wealth instantly using Bitcoin, global consumers will likely demand the same efficiency for their securities and similar asset holdings.

For such a global system to work it will need to be independent of regulatory jurisdictions.  This is not due to ideological belief but practicality: if the rails for blockchain asset transfer are not censorship resistance and jurisdiction agnostic, any given jurisdiction may be in conflict with another.  In legacy systems, wealth was generally confined in the jurisdiction of the holder and therefore easy to control based on the policies of that jurisdiction. Because of the global nature of blockchain technology any protocol level ability to control wealth would potentially place jurisdictions in conflict and will not be able to operate fairly.  
