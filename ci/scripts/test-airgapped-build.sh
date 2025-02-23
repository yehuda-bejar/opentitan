#!/bin/bash
# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

set -ex

# Prefetch bazel airgapped dependencies.
util/prep-bazel-airgapped-build.sh -f

# Remove the airgapped network namespace.
remove_airgapped_netns() {
  sudo ip netns delete airgapped
}
trap remove_airgapped_netns EXIT

# Set up a network namespace named "airgapped" with access to loopback.
sudo ip netns add airgapped
sudo ip netns exec airgapped ip addr add 127.0.0.1/8 dev lo
sudo ip netns exec airgapped ip link set dev lo up

# Enter the network namespace and perform a build test.
sudo ip netns exec airgapped sudo -u "$USER" bash -c \
  "export BAZEL_BITSTREAMS_CACHE=$(pwd)/bazel-airgapped/bitstreams-cache;
  export BITSTREAM=--offline;
  export BAZEL_PYTHON_WHEELS_REPO=$(pwd)/bazel-airgapped/ot_python_wheels;
  bazel-airgapped/bazel build \
    --distdir=bazel-airgapped/bazel-distdir \
    --repository_cache=bazel-airgapped/bazel-cache \
    //sw/device/tests:uart_smoketest_signed_sim_dv;"
exit 0
