#!/bin/bash
#
# This script sets up a development environment on Ubuntu 18.04. Feel free to
# use these as a starting point for your distribution of choice.

set -eu

REPO_ROOT=$(cd "$(dirname $0)"/..; pwd)

EXPECTED_RELEASE="bionic|focal"
RELEASE=$(lsb_release --codename | awk '{print $2}')

if echo "${RELEASE}" | grep -q -e "${EXPECTED_RELEASE}" > /dev/null;
then
  echo "This script was intended for ${EXPECTED_RELEASE} (you have ${RELEASE}) exiting!" >&2
  exit 1
fi

sudo bash -x "${REPO_ROOT}/.github/workflows/setup_ubuntu.sh" --no-setup-toolchain-variants
"${REPO_ROOT}/.github/workflows/setup_conan.sh"

# make clang-{tidy,format}-10 the default
sudo update-alternatives --verbose --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-10 90
sudo update-alternatives --verbose --install /usr/bin/clang-format clang-format /usr/bin/clang-format-10 90


cat <<EOF
OK, your system is almost ready to build Katana. Next:

export PATH=\$PATH:\$HOME/.local/bin  # To get conan into your PATH
cd $REPO_ROOT
mkdir build; cd build
conan install -if . $REPO_ROOT/config --build=missing
CC=gcc CXX=g++ cmake -DCMAKE_BUILD_TYPE=Release $REPO_ROOT -DCMAKE_TOOLCHAIN_FILE=conan_paths.cmake
make                 # Feel free to use -j or similar.

(You should be able to copy and paste the whole script above into your terminal.)
EOF

# --Azure cli  https://docs.microsoft.com/en-us/cli/azure/install-azure-cli-apt
# curl -sL https://aka.ms/InstallAzureCLIDeb | sudo bash
