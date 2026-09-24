"""w2-041 probe self-test helper: listen on 127.0.0.1:<port> for <seconds> (default 8124, 40 s)."""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8124
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 40
s = socket.socket()
s.bind(("127.0.0.1", port))
s.listen()
time.sleep(seconds)
