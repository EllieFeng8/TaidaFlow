# w2-050 test fixture (dev-only): writes a large CSV (about 1 GB) with a valid export name for the
# nginx download / Range / resume checks. Every line carries its own sequence number, so any wrong
# byte range changes the SHA-256. Usage: python -B make_big_csv.py <path> [lines]
import sys

path = sys.argv[1]
lines = int(sys.argv[2]) if len(sys.argv) > 2 else 16_000_000
line = b'"%09d","2026/01/01 00:00:00","12.34","56.78","90.12","34.56"\r\n'
with open(path, 'wb', buffering=1 << 20) as f:
    f.write(b'\xef\xbb\xbf"seq","time","a","b","c","d"\r\n')
    for i in range(1, lines + 1):
        f.write(line % i)
