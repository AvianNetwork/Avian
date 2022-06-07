#!/usr/bin/env python3
# Copyright (c) 2017-2019 The Bitcoin Core developers
# Copyright (c) 2017-2020 The Raven Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test loadblock option

Test the option to start a node with the option loadblock which loads
a serialized blockchain from a file (usually called bootstrap.dat).
To generate that file this test uses the helper scripts available
in contrib/linearize.
"""

import configparser
import os
import subprocess
import sys
import tempfile
import urllib
from test_framework.test_framework import AvianTestFramework
from test_framework.util import assert_equal, wait_until

class LoadblockTest(AvianTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def run_test(self):
        self.nodes[1].setnetworkactive(state=False)
        self.nodes[0].generate(100)

        # Parsing the url of our node to get settings for config file
        data_dir = self.nodes[0].datadir
        node_url = urllib.parse.urlparse(self.nodes[0].url)
        cfg_file = os.path.join(data_dir, "linearize.cfg")
        bootstrap_file = os.path.join(self.options.tmpdir, "bootstrap.dat")
        genesis_block = self.nodes[0].getblockhash(0)
        blocks_dir = os.path.join(data_dir, "regtest", "blocks")
        hash_list = tempfile.NamedTemporaryFile(dir=data_dir,
                                                mode='w',
                                                delete=False,
                                                encoding="utf-8")

        self.log.info("Create linearization config file")
        with open(cfg_file, "a", encoding="utf-8") as cfg:
            cfg.write("datadir={}\n".format(data_dir))
            cfg.write("rpcuser={}\n".format(node_url.username))
            cfg.write("rpcpassword={}\n".format(node_url.password))
            cfg.write("port={}\n".format(node_url.port))
            cfg.write("host={}\n".format(node_url.hostname))
            cfg.write("output_file={}\n".format(bootstrap_file))
            cfg.write("max_height=100\n")
            cfg.write("netmagic=43524f57\n")
            cfg.write("input={}\n".format(blocks_dir))
            cfg.write("genesis={}\n".format(genesis_block))
            cfg.write("hashlist={}\n".format(hash_list.name))

        # Get the configuration file to find src and linearize
        config = configparser.ConfigParser()
        if not self.options.configfile:
            self.options.configfile = os.path.abspath(os.path.join(os.path.dirname(__file__), "../config.ini"))
        config.read_file(open(self.options.configfile))
        base_dir = config["environment"]["SRCDIR"]
        linearize_dir = os.path.join(base_dir, "contrib", "linearize")

        self.log.info("Run linearization of block hashes")
        linearize_hashes_file = os.path.join(linearize_dir, "linearize-hashes.py")
        subprocess.run([sys.executable, linearize_hashes_file, cfg_file],
                       stdout=hash_list,
                       check=True)

        self.log.info("Run linearization of block data")
        linearize_data_file = os.path.join(linearize_dir, "linearize-data.py")
        subprocess.run([sys.executable, linearize_data_file, cfg_file],
                       check=True)

        self.log.info("Restart second, unsynced node with bootstrap file")
        self.stop_node(1)
        self.start_node(1, ["-loadblock=" + bootstrap_file])
        wait_until(lambda: self.nodes[1].getblockcount() == 100, err_msg="Wait for block count == 100")

        assert_equal(self.nodes[1].getblockchaininfo()['blocks'], 100)
        assert_equal(self.nodes[0].getbestblockhash(), self.nodes[1].getbestblockhash())


if __name__ == '__main__':
    LoadblockTest().main()
