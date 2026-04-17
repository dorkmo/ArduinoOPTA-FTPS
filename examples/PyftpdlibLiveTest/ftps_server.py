"""
Explicit FTPS test server for ArduinoOPTA-FTPS library validation.

Uses pyftpdlib with TLS (AUTH TLS / Explicit FTPS) to serve files over
protected passive-mode connections. Designed to be used alongside the
PyftpdlibLiveTest.ino sketch running on an Arduino Opta.

Usage:
    python ftps_server.py [--host 0.0.0.0] [--port 21] [--pasv-range 60000-60100]

Requires: pyftpdlib, pyOpenSSL
    pip install pyftpdlib pyOpenSSL

The server.crt and server.key files must be in the same directory.
Run gen_cert.py first if they don't exist.
"""

import argparse
import os
import sys
import logging

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import TLS_FTPHandler
from pyftpdlib.servers import FTPServer


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    parser = argparse.ArgumentParser(description="Explicit FTPS test server")
    parser.add_argument("--host", default="0.0.0.0", help="Listen address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=21, help="Control port (default: 21)")
    parser.add_argument("--pasv-range", default="60000-60100",
                        help="Passive port range (default: 60000-60100)")
    parser.add_argument("--user", default="optauser", help="FTP username (default: optauser)")
    parser.add_argument("--password", default="optapass", help="FTP password (default: optapass)")
    parser.add_argument("--root", default=os.path.join(script_dir, "ftp_root"),
                        help="FTP root directory (default: ./ftp_root)")
    parser.add_argument("--cert", default=os.path.join(script_dir, "server.crt"),
                        help="TLS certificate file (default: ./server.crt)")
    parser.add_argument("--key", default=os.path.join(script_dir, "server.key"),
                        help="TLS private key file (default: ./server.key)")
    args = parser.parse_args()

    # Validate cert/key exist
    if not os.path.isfile(args.cert):
        print(f"ERROR: Certificate not found: {args.cert}")
        print("Run gen_cert.py first to generate server.crt and server.key")
        sys.exit(1)
    if not os.path.isfile(args.key):
        print(f"ERROR: Key file not found: {args.key}")
        sys.exit(1)

    # Create FTP root if needed
    os.makedirs(args.root, exist_ok=True)

    # Parse passive port range
    pasv_low, pasv_high = (int(x) for x in args.pasv_range.split("-"))

    # Set up user with full permissions in the FTP root
    authorizer = DummyAuthorizer()
    authorizer.add_user(args.user, args.password, args.root, perm="elradfmwMT")

    # Configure TLS handler for Explicit FTPS (AUTH TLS)
    handler = TLS_FTPHandler
    handler.authorizer = authorizer
    handler.certfile = args.cert
    handler.keyfile = args.key

    # Require TLS on both control and data channels (match library expectations)
    handler.tls_control_required = True
    handler.tls_data_required = True

    # Passive mode port range
    # Mbed TLS clients don't perform TLS session resumption (RFC 5077 tickets),
    # so disable the session-reuse requirement to avoid 425 rejections.
    handler.require_ssl_reuse = False

    handler.passive_ports = range(pasv_low, pasv_high + 1)

    # Banner
    handler.banner = "pyftpdlib FTPS test server ready (ArduinoOPTA-FTPS validation)"

    # Enable logging
    logging.basicConfig(level=logging.DEBUG)

    server = FTPServer((args.host, args.port), handler)
    server.max_cons = 5
    server.max_cons_per_ip = 3

    # Print summary
    print("=" * 60)
    print("  ArduinoOPTA-FTPS Test Server (pyftpdlib + TLS)")
    print("=" * 60)
    print(f"  Listen:     {args.host}:{args.port}")
    print(f"  PASV range: {pasv_low}-{pasv_high}")
    print(f"  User:       {args.user}")
    print(f"  FTP root:   {args.root}")
    print(f"  Cert:       {args.cert}")
    print(f"  Key:        {args.key}")
    print(f"  TLS mode:   Explicit FTPS (AUTH TLS)")
    print("=" * 60)
    print("  Waiting for Arduino Opta connections...")
    print("  Press Ctrl+C to stop.")
    print("=" * 60)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")
        server.close_all()


if __name__ == "__main__":
    main()
