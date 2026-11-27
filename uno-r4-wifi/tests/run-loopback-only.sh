#!/usr/bin/env bash
set -euo pipefail
# The original node's announcer may try its default bootstrap addresses. Give
# this test only loopback; no public or lab interface is available in the child.
exec unshare -Urn bash -c 'ip link set lo up; exec "$@"' loopback-test "$@"
