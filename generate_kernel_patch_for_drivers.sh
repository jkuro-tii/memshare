#!/usr/bin/env bash

set -euo pipefail

[[ ! -d linux-6.2 ]] && git clone https://github.com/torvalds/linux.git --branch v6.4 --depth 1

pushd linux
  git reset --hard
  git clean -xdf

  cp -Rv ../drivers/char/virtio_pmem ./drivers/char
  cat ../drivers/char/Makefile >> drivers/char/Makefile
  cat ../drivers/nvdimm/Makefile >> drivers/nvdimm/Makefile

  git add .
  git commit -m "Memory sharing driver"
  git format-patch -k -1 -o ..
popd
