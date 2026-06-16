Avian Core
=============

Setup
---------------------
Avian Core is the Avian Network client and it builds the backbone of the network. It downloads and, by default, stores the entire history of Avian transactions, which requires several hundred gigabytes or more of disk space. Depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to several days or more.

To download Avian Core, visit [github.com/AvianNetwork/Avian](https://github.com/AvianNetwork/Avian/releases).

Running
---------------------
The following are some helpful notes on how to run Bitcoin Core on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/avian-qt` (GUI) or
- `bin/aviand` (headless)
- `bin/avian` (wrapper command)

The `avian` command supports subcommands like `avian gui`, `avian node`, and `avian rpc` exposing different functionality. Subcommands can be listed with `avian help`.

### Windows

Unpack the files into a directory, and then run avian-qt.exe.

### macOS

Drag Avian Core to your applications folder, and then run Avian Core.

### Need Help?

* See the documentation on [GitHub](https://github.com/AvianNetwork/Avian) for help and more information.
* Ask for help on the [Avian Network Discord](https://discord.gg/aviannetwork).

Building
---------------------
The following are developer notes on how to build Avian Core on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows-msvc.md)
- [FreeBSD Build Notes](build-freebsd.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)

Development
---------------------
The Avian repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Process](release-process.md)
- [Source Code Documentation (External Link)](https://doxygen.bitcoincore.org/) (upstream Bitcoin Core)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)
- [Internal Design Docs](design/)

### Resources
* Discuss on the [Avian Network Discord](https://discord.gg/aviannetwork).
* Open issues on [GitHub](https://github.com/AvianNetwork/Avian/issues).

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [avian.conf Configuration File](avian-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [Managing Wallets](managing-wallets.md)
- [Multisig Tutorial](multisig-tutorial.md)
- [Offline Signing Tutorial](offline-signing-tutorial.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Transaction Relay Policy](policy/README.md)
- [Web UI](webui.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
