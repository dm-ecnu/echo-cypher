#!/bin/sh
# Apply the ECHO hook to the vendored base (idempotent: skips when already applied).
set -e
cd "$(dirname "$0")/.."
if grep -q "ECHO PATCH" vendor/SubgraphMatching/matching/EvaluateQuery.cpp; then
  echo "patch already applied"
  exit 0
fi
patch -p1 -d vendor/SubgraphMatching < patch/EvaluateQuery.LFTJ.patch
