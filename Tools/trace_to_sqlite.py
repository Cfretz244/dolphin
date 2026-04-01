#!/usr/bin/env python3
"""
Convert a Dolphin trace collection binary (.dpht) file to SQLite.

Usage:
    python3 trace_to_sqlite.py input.dpht [output.db]

If output.db is not specified, it defaults to input.dpht with .db extension.
"""

import sqlite3
import struct
import sys
from pathlib import Path

MAGIC = 0x54485044  # "DPHT" little-endian
FORMAT_VERSION = 2

EDGE_TYPES = {0: "static", 1: "dynamic", 2: "call"}


def read_trace_file(path: str):
    blocks = []
    edges = []
    smc_events = []

    with open(path, "rb") as f:
        # Read header (20 bytes)
        header_data = f.read(20)
        if len(header_data) < 20:
            raise ValueError("File too small for header")

        magic, version, block_count, edge_count, smc_count = struct.unpack(
            "<5I", header_data
        )

        if magic != MAGIC:
            raise ValueError(f"Bad magic: {magic:#010x}, expected {MAGIC:#010x}")
        if version != FORMAT_VERSION:
            raise ValueError(
                f"Unsupported version: {version}, expected {FORMAT_VERSION}"
            )

        print(f"Blocks: {block_count}, Edges: {edge_count}, SMC events: {smc_count}")

        # Read blocks (8 bytes each: u32 addr + u32 size)
        for _ in range(block_count):
            data = f.read(8)
            if len(data) < 8:
                raise ValueError("Truncated block record")
            ppc_addr, block_size = struct.unpack("<2I", data)
            blocks.append((ppc_addr, block_size))

        # Read edges (12 bytes each: u32 from + u32 to + u8 type + 3 padding)
        for _ in range(edge_count):
            data = f.read(12)
            if len(data) < 12:
                raise ValueError("Truncated edge record")
            from_addr, to_addr = struct.unpack_from("<2I", data, 0)
            edge_type = data[8]
            edges.append(
                (
                    from_addr,
                    to_addr,
                    EDGE_TYPES.get(edge_type, f"unknown_{edge_type}"),
                )
            )

        # Read SMC events (16 bytes each)
        for _ in range(smc_count):
            data = f.read(16)
            if len(data) < 16:
                raise ValueError("Truncated SMC record")
            addr, length, event_counter = struct.unpack("<2I Q", data)
            smc_events.append((addr, length, event_counter))

    return blocks, edges, smc_events


def write_sqlite(db_path: str, blocks, edges, smc_events):
    conn = sqlite3.connect(db_path)
    c = conn.cursor()

    c.execute("DROP TABLE IF EXISTS blocks")
    c.execute("DROP TABLE IF EXISTS edges")
    c.execute("DROP TABLE IF EXISTS smc_regions")

    c.execute(
        """
        CREATE TABLE blocks (
            ppc_addr INTEGER PRIMARY KEY,
            length INTEGER NOT NULL
        )
    """
    )

    c.execute(
        """
        CREATE TABLE edges (
            from_addr INTEGER NOT NULL,
            to_addr INTEGER NOT NULL,
            edge_type TEXT NOT NULL,
            PRIMARY KEY (from_addr, to_addr, edge_type)
        )
    """
    )

    c.execute(
        """
        CREATE TABLE smc_regions (
            addr INTEGER NOT NULL,
            length INTEGER NOT NULL,
            event_counter INTEGER NOT NULL
        )
    """
    )

    c.executemany("INSERT INTO blocks VALUES (?, ?)", blocks)
    c.executemany("INSERT INTO edges VALUES (?, ?, ?)", edges)
    c.executemany("INSERT INTO smc_regions VALUES (?, ?, ?)", smc_events)

    # Create useful indexes
    c.execute("CREATE INDEX idx_edges_from ON edges(from_addr)")
    c.execute("CREATE INDEX idx_edges_to ON edges(to_addr)")

    conn.commit()

    # Print summary statistics
    c.execute("SELECT COUNT(*) FROM blocks")
    (block_count,) = c.fetchone()
    c.execute("SELECT edge_type, COUNT(*) FROM edges GROUP BY edge_type")
    edge_stats = c.fetchall()
    c.execute("SELECT COUNT(*) FROM smc_regions")
    (smc_count,) = c.fetchone()

    c.execute("SELECT MIN(ppc_addr), MAX(ppc_addr) FROM blocks")
    lo, hi = c.fetchone()

    print(f"\nSummary:")
    print(f"  Blocks: {block_count} (0x{lo:08X} - 0x{hi:08X})")
    print(f"  Edges:")
    for edge_type, count in edge_stats:
        print(f"    {edge_type}: {count}")
    print(f"  SMC events: {smc_count}")

    # Blocks with most outgoing edges (switch tables, dispatchers)
    c.execute(
        """SELECT from_addr, COUNT(*) as n FROM edges
           GROUP BY from_addr ORDER BY n DESC LIMIT 5"""
    )
    rows = c.fetchall()
    if rows:
        print(f"\n  Top branch-heavy blocks:")
        for addr, n in rows:
            print(f"    0x{addr:08X}: {n} outgoing edges")

    # Call graph stats
    c.execute(
        "SELECT COUNT(DISTINCT from_addr), COUNT(DISTINCT to_addr) FROM edges WHERE edge_type='call'"
    )
    callers, callees = c.fetchone()
    print(f"\n  Call graph: {callers} call sites -> {callees} unique targets")

    conn.close()


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.dpht [output.db]")
        sys.exit(1)

    input_path = sys.argv[1]
    if len(sys.argv) >= 3:
        output_path = sys.argv[2]
    else:
        output_path = str(Path(input_path).with_suffix(".db"))

    print(f"Reading: {input_path}")
    blocks, edges, smc_events = read_trace_file(input_path)

    print(f"Writing: {output_path}")
    write_sqlite(output_path, blocks, edges, smc_events)

    print(f"\nDone. SQLite database written to {output_path}")


if __name__ == "__main__":
    main()
