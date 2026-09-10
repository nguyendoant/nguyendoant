#!/bin/bash

mkdir -p certs
cd certs

echo "Generating SSL/TLS certificates for testing..."

# Generate CA certificate
openssl req -x509 -newkey rsa:4096 \
    -keyout ca.key -out ca.crt \
    -days 365 -nodes \
    -subj "/C=US/ST=California/L=San Francisco/O=Test Lab/CN=Test CA"

# Generate server certificate
openssl req -x509 -newkey rsa:4096 \
    -keyout server.key -out server.crt \
    -days 365 -nodes \
    -subj "/C=US/ST=California/L=San Francisco/O=Test Lab/CN=localhost"

# Generate client certificate
openssl req -x509 -newkey rsa:4096 \
    -keyout client.key -out client.crt \
    -days 365 -nodes \
    -subj "/C=US/ST=California/L=San Francisco/O=Test Lab/CN=client"

chmod 600 *.key

echo ""
echo "✓ Certificates generated successfully in certs/ directory"
echo ""
ls -lh

cd ..
