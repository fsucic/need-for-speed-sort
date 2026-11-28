#!/usr/bin/env bash
# Downloads the SOSD real-world datasets used in the NFS Sort paper (books, fb, osm, wiki).
# Format: 8-byte little-endian count, then that many uint64 keys. Skips files whose md5 already matches.
set -euo pipefail
cd "$(dirname "$0")/../data"

fetch() {
  local file=$1 md5=$2 url=$3
  if [[ -f $file && $(md5 -q "$file") == "$md5" ]]; then echo "$file ok (cached)"; return; fi
  echo "downloading $file"
  curl -fsSL -A agent-local --retry 5 --retry-all-errors "$url" | zstd -dq -o "$file.part" -f
  [[ $(md5 -q "$file.part") == "$md5" ]] || { echo "$file checksum mismatch" >&2; exit 1; }
  mv "$file.part" "$file"
  echo "$file ok"
}

# Run in parallel: Dataverse serves ~2 MB/s per stream, Dropbox ~17 MB/s.
fetch wiki_ts_200M_uint64 4f1402b1c476d67f77d2da4955432f7d "https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A/SVN8PI" &
fetch fb_200M_uint64 3b0f820caa0d62150e87ce94ec989978 "https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A/EATHF7" &
fetch books_800M_uint64 8708eb3e1757640ba18dcd3a0dbb53bc "https://www.dropbox.com/s/y2u3nbanbnbmg7n/books_800M_uint64.zst?dl=1" &
fetch osm_cellids_800M_uint64 70670bf41196b9591e07d0128a281b9a "https://www.dropbox.com/s/j1d4ufn4fyb4po2/osm_cellids_800M_uint64.zst?dl=1" &
failed=0; for job in $(jobs -p); do wait "$job" || failed=1; done
[[ $failed == 0 ]] || { echo "some downloads failed" >&2; exit 1; }
echo done
