#!/usr/bin/env python3

import argparse
import shutil


def _start_signature() -> bytes:
    encoded = b""
    encoded += (0).to_bytes(1, byteorder="big")
    encoded += (147).to_bytes(1, byteorder="big")
    encoded += (4).to_bytes(1, byteorder="big")
    encoded += (16).to_bytes(1, byteorder="big")
    encoded += b"duckdb_signature"
    encoded += (128).to_bytes(1, byteorder="big")
    encoded += (4).to_bytes(1, byteorder="big")
    return encoded


def _padded(text: str) -> bytes:
    encoded = text.encode("ascii")
    return encoded + (b"\x00" * (32 - len(encoded)))


def main() -> None:
    parser = argparse.ArgumentParser(description="Append DuckDB extension metadata")
    parser.add_argument("--library-file", required=True)
    parser.add_argument("--extension-name", required=True)
    parser.add_argument("--out-file", required=True)
    parser.add_argument("--duckdb-version", default="v1.5.0")
    parser.add_argument("--duckdb-platform", default="windows_amd64")
    parser.add_argument("--extension-version", default="0.1.0")
    parser.add_argument("--abi-type", default="C_STRUCT")
    args = parser.parse_args()

    tmp_file = args.out_file + ".tmp"
    shutil.copyfile(args.library_file, tmp_file)

    with open(tmp_file, "ab") as file:
        file.write(_start_signature())
        file.write(_padded(""))
        file.write(_padded(""))
        file.write(_padded(""))
        file.write(_padded(args.abi_type))
        file.write(_padded(args.extension_version))
        file.write(_padded(args.duckdb_version))
        file.write(_padded(args.duckdb_platform))
        file.write(_padded("4"))
        file.write(b"\x00" * 256)

    shutil.move(tmp_file, args.out_file)


if __name__ == "__main__":
    main()
