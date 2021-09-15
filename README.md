RavencoinLite Core integration/staging tree
=====================================

[![C/C++ CI](https://github.com/RavencoinLite/RavencoinLite/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/RavencoinLite/RavencoinLite/actions/workflows/c-cpp.yml)

https://ravencoinlite.org

What is RavencoinLite?
----------------

RavencoinLite is an experimental digital currency that enables instant payments to
anyone, anywhere in the world. RavencoinLite uses peer-to-peer technology to operate
with no central authority: managing transactions and issuing money are carried
out collectively by the network. RavencoinLite Core is the name of open source
software which enables the use of this currency.

For more information, as well as an immediately useable, binary version of
the RavencoinLite Core software, see https://ravencoinlite.org

License
-------

RavencoinLite Core is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/RavencoinLite/tags/RavencoinLite/tags) are created
regularly to indicate new official, stable release versions of RavencoinLite Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

Testnet is now up and running and available to use during development. There is an issue when connecting to the testnet that requires the use of the -maxtipage parameter in order to connect to the test network initially. After the initial launch the -maxtipage parameter is not required.

Use this command to initially start ravend on the testnet. <code>./ravend -testnet -maxtipage=259200</code>


Running on Mainnet
-------
Use this command to start ravend on the mainnet.
<code>./ravend -seednode=dnsseed.ravencoinlite.org</code>

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python, that are run automatically on the build server.
These tests can be run (if the [test dependencies](/test) are installed) with: `test/functional/test_runner.py`


### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.


About RavencoinLite
----------------
A digital peer to peer network for the facilitation of asset transfer.

Having started development on August 12th of 2021, and active on mainnet since September 1st, Ravencoin Lite (RVL) is a fork of Ravencoin Classic (RVC), aimed primarily at bringing the means of development back into the hands of the community after RVC has been abandoned by its creators. With the RVC github locked, and software in disrepair, RVL seeks to improve upon the existing foundations by implementing the necessary updates and bug fixes need to bring the original x16r fork of Ravencoin up to par with modern cryptocurrencies. 

This project is being spearheaded by a small group of enthusiasts, representing the interests of the actual RVC community, as opposed to the original fork, which was created and maintained by Chinese ASIC manufacturers looking to make a return on their machines after RVN switched to the x16rv2 algorithm. As such, we are always seeking for people wishing to contribute their experience and knowhow to the development of Ravencoin Lite.

Our goal is to stick to the x16r algorithm as previously declared by the RVC project, however, we are aware of the potential threat that ASICs pose to the network, and are looking to mitigate it while staying true to our roots. Once all existing bugs have been fixed, and asset functionality restored, we aim to implement a secondary CPU algorithm, somewhat akin to Myriad (XMY), in order to help decentralize the mining of RVL without resorting to replacing the primary algo.

I would like to reiterate that there are no professional developers on the dev team as of this date; we are merely a group of dedicated community members who got tired of waiting around for change and took matters into their own hands. As such, the speed of development will greatly depend on the amount of contributors. Let us come together as a community, and make the restoration of RVC a reality!

Thank you to the Bitcoin developers.

The RavencoinLite project is launched based on the hard work and continuous effort of over 400 Bitcoin developers who made over 14,000 commits over the life to date of the Bitcoin project. We are eternally grateful to you for your efforts and diligence in making a secure network and for their support of free and open source software development.  The RavencoinLite experiment is made on the foundation you built.


Abstract
----------------
RavencoinLite aims to implement a blockchain which is optimized specifically for the use case of transferring assets such as securities from one holder to another. Based on the extensive development and testing of Bitcoin, RavencoinLite is built on a fork of the Bitcoin code. Key changes include a faster block reward time and a change in the number, but not weighed distribution schedule, of coins. RavencoinLite is free and open source and will be issued and mined transparently with no pre-mine, developer allocation or any other similar set aside. RavencoinLite is intended to prioritize user control, privacy and censorship resistance and be jurisdiction agnostic while allowing simple optional additional features for users based on need.



A blockchain is a ledger showing the value of something and allowing it to be transferred to someone else. Of all the possible uses for blockchains, the reporting of who owns what is one of the core uses of the technology.  This is why the first and most successful use case for blockchain technology to date has been Bitcoin.

The success of the Ethereum ERC 20 token shows the demand for tokenized assets that use another blockchain.  Tokens offer many advantages to traditional shares or other participation mechanisms such as faster transfer, possibly increased user control and censorship resistance and reduction or elimination of the need for trusted third parties.

Bitcoin also has the capability of serving as the rails for tokens by using projects such as Omnilayer, RSK or Counterparty. However, neither Bitcoin nor Ethereum was specifically designed for facilitating ownership of other assets.

RavencoinLite is designed to be a use case specific blockchain designed to efficiently handle one specific function: the transfer of assets from one party to another., while providing a secure network for transactions.

Bitcoin is and always should be focused on its goals of being a better form of money. Bitcoin developers will unlikely prioritize improvements or features which are specifically beneficial to the facilitation of token transfers.  One goal of the RavencoinLite project is to see if a use case specific blockchain and development effort can create code which can either improve existing structures like Bitcoin or provide advantages for specific use cases.

In the new global economy, borders and jurisdictions will be less relevant as more assets are tradable and trade across borders is increasingly frictionless. In an age where people can move significant amounts of wealth instantly using Bitcoin, global consumers will likely demand the same efficiency for their securities and similar asset holdings.

For such a global system to work it will need to be independent of regulatory jurisdictions.  This is not due to ideological belief but practicality: if the rails for blockchain asset transfer are not censorship resistance and jurisdiction agnostic, any given jurisdiction may be in conflict with another.  In legacy systems, wealth was generally confined in the jurisdiction of the holder and therefore easy to control based on the policies of that jurisdiction. Because of the global nature of blockchain technology any protocol level ability to control wealth would potentially place jurisdictions in conflict and will not be able to operate fairly.  
