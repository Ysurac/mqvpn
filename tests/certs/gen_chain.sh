#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and contributors
#
# Regenerates the two-tier test chain used by tests/test_server.c:
#
#   mqvpn test root CA  (self-signed; NOT committed and NOT in any store)
#     └─ mqvpn test intermediate CA   → chain-intermediate.der
#          └─ mqvpn-test (leaf)        → chain-leaf.key, chain-leaf.der
#   chain-fullchain.crt = leaf PEM + intermediate PEM (what a server serves)
#
# The CA keys are throwaway: the tests only need the presented chain and a
# root that no trust store knows. Run from this directory.
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
days=3650

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$tmp/root.key" -out "$tmp/root.crt" -days "$days" \
    -subj "/CN=mqvpn test root CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null

openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$tmp/int.key" -out "$tmp/int.csr" \
    -subj "/CN=mqvpn test intermediate CA" 2>/dev/null
printf 'basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n' > "$tmp/int.ext"
openssl x509 -req -in "$tmp/int.csr" -CA "$tmp/root.crt" -CAkey "$tmp/root.key" \
    -CAcreateserial -out "$tmp/int.crt" -days "$days" -extfile "$tmp/int.ext" 2>/dev/null

openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout chain-leaf.key -out "$tmp/leaf.csr" -subj "/CN=mqvpn-test" 2>/dev/null
printf 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:mqvpn-test\nextendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n' > "$tmp/leaf.ext"
openssl x509 -req -in "$tmp/leaf.csr" -CA "$tmp/int.crt" -CAkey "$tmp/int.key" \
    -CAcreateserial -out "$tmp/leaf.crt" -days "$days" -extfile "$tmp/leaf.ext" 2>/dev/null

cat "$tmp/leaf.crt" "$tmp/int.crt" > chain-fullchain.crt
openssl x509 -in "$tmp/leaf.crt" -outform DER -out chain-leaf.der
openssl x509 -in "$tmp/int.crt" -outform DER -out chain-intermediate.der
openssl verify -CAfile "$tmp/root.crt" -untrusted "$tmp/int.crt" "$tmp/leaf.crt"
