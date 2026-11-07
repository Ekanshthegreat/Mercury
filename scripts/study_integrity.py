"""Checksums for the four portable latency-study artifacts."""
import hashlib
import re

ARTIFACTS = ('comparison.csv', 'configurations.json', 'runs.csv', 'summary.md')


def digest(path):
    hasher = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            hasher.update(block)
    return hasher.hexdigest()


def write_checksums(directory):
    text = ''.join(f'{digest(directory / name)}  {name}\n' for name in ARTIFACTS)
    (directory / 'SHA256SUMS.txt').write_text(text, encoding='ascii')


def verify_checksums(directory):
    path = directory / 'SHA256SUMS.txt'
    if not path.exists():
        return False  # Older studies can still undergo numerical verification.
    found = {}
    for line in path.read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
        if not match or match[2] not in ARTIFACTS or match[2] in found:
            raise ValueError('Invalid latency-study checksum inventory')
        found[match[2]] = match[1]
    if set(found) != set(ARTIFACTS):
        raise ValueError('Incomplete latency-study checksum inventory')
    for name, expected in found.items():
        if digest(directory / name) != expected:
            raise ValueError('Latency-study checksum mismatch: ' + name)
    return True
