#!/bin/bash
# Used to update the build number using the GitHub build number passed as a commandline argument.
echo "GitHub build number provided as $1";
BUILD="3.0.$1"
echo "Setting build to $BUILD";
sed -i -e "s/3.0.INSERTVERSIONHERE/$BUILD/g" src/config/avian-config.h
