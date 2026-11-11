#!/usr/bin/env python3
"""Publish a CSV only after an offline Mercury journal replay succeeds."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def export(gateway, journal, output, after=0):
    if not 0 <= after < 2**64:
        raise ValueError('Report cursor must fit in an unsigned 64-bit integer')
    if output.exists():
        raise FileExistsError(output)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix='.reports-') as stream:
        subprocess.run(
            [str(gateway.resolve()), '--replay', str(journal), '--after', str(after)],
            stdout=stream,
            check=True,
        )
        os.fsync(stream.fileno())
        os.link(stream.name, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('journal', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--gateway', type=Path, default=Path('build/mercury_gateway'))
    parser.add_argument('--after', type=int, default=0)
    args = parser.parse_args()
    try:
        export(args.gateway, args.journal, args.output, args.after)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
