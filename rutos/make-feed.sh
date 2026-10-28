#!/bin/sh
# Turns the Puffin SDK's packages into an opkg feed:
#   make-feed.sh <dist/packages> <feed dir> [usign public key]
# The feed is the .ipk files with a Packages index (and Packages.gz, which the router's opkg
# fetches for a src/gz line). The router checks feeds' signatures (option check_signature,
# usign): with OPKG_FEED_KEY set to a usign secret key, Packages is signed into Packages.sig,
# and checked against the public key, which goes in the feed as feed.pub for opkg-key add.
set -eu

src=$1
out=$2
pub=${3:-}

mkdir -p "$out"
rm -f "$out"/*.ipk "$out"/Packages*
for ipk in "$src"/*.ipk; do
	name=${ipk##*/}
	cp "$ipk" "$out/$name"
	# An ipk is an ar archive; the index entry is its control file plus where to get it.
	ar p "$ipk" control.tar.gz | tar -xzO ./control | sed '/^$/d' >> "$out/Packages"
	printf 'Filename: %s\nSize: %s\nSHA256sum: %s\n\n' "$name" \
		"$(stat -c %s "$ipk")" "$(sha256sum "$ipk" | cut -d ' ' -f 1)" >> "$out/Packages"
done
gzip -9nc "$out/Packages" > "$out/Packages.gz"

if [ -n "${OPKG_FEED_KEY:-}" ]; then
	key=$(mktemp)
	trap 'rm -f "$key"' EXIT
	printf '%s\n' "$OPKG_FEED_KEY" > "$key"
	# opkg-key verify checks the uncompressed list.
	usign -S -m "$out/Packages" -s "$key" -x "$out/Packages.sig"
	if [ -n "$pub" ]; then
		usign -V -q -m "$out/Packages" -p "$pub" -x "$out/Packages.sig" ||
			{ echo "Packages.sig does not match $pub" >&2; exit 1; }
		cp "$pub" "$out/feed.pub"
	fi
else
	echo "OPKG_FEED_KEY not set: the feed is unsigned" >&2
fi
