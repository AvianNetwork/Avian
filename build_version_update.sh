#!/bin/bash
# Used to update the build number using the GitHub build number passed as a commandline argument.
echo "GitHub build number provided as $1";
sed -i -e "s/_CLIENT_VERSION_BUILD, 0/_CLIENT_VERSION_BUILD, $1/g" configure.ac
