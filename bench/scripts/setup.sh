#!/usr/bin/env bash
# Fetches the competitor sorts at pinned commits into bench/third_party. Boost (spreadsort) comes from Homebrew.
set -euo pipefail
cd "$(dirname "$0")/../third_party" 2>/dev/null || { mkdir -p "$(dirname "$0")/../third_party"; cd "$(dirname "$0")/../third_party"; }
pin() {
  local dir=$1 url=$2 rev=$3
  [[ -d $dir ]] || git clone -q "$url" "$dir"
  git -C "$dir" fetch -q origin "$rev" 2>/dev/null || true
  git -C "$dir" checkout -q "$rev"
}
pin ips4o   https://github.com/ips4o/ips4o.git                        08a5b926ee65cef19139057c6bde02bb5542c1cb
pin pdqsort https://github.com/orlp/pdqsort.git                       b1ef26a55cdb60d236a5cb199c4234c704f46726
pin bls     https://github.com/mattiaodorisio/balanced-learned-sort.git ad96e34eb01037b3ce06dac873a136dd90da7805
pin ips2ra  https://github.com/ips4o/ips2ra.git                       83f6b76ca308b277b2acc3a36426a3baa3c9cb73
