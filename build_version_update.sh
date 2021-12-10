#!/bin/bash
# Used to update the build number using the GitHub build number passed as a commandline argument.
echo "GitHub build number provided as $1";
sed -i -e "s/INSERTBUILDNUMBERHERE/$1/g" configure.ac
