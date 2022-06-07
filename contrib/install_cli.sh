 #!/usr/bin/env bash

 # Execute this file to install the raven cli tools into your path on OS X

 CURRENT_LOC="$( cd "$(dirname "$0")" ; pwd -P )"
 LOCATION=${CURRENT_LOC%Avian-Qt.app*}

 # Ensure that the directory to symlink to exists
 sudo mkdir -p /usr/local/bin

 # Create symlinks to the cli tools
 sudo ln -s ${LOCATION}/Avian-Qt.app/Contents/MacOS/aviand /usr/local/bin/aviand
 sudo ln -s ${LOCATION}/Avian-Qt.app/Contents/MacOS/avian-cli /usr/local/bin/avian-cli
