#!/usr/bin/env python3
"""Deterministic codec tests for the raw EloqStore paged-row inspector."""

import base64
import binascii
import unittest

import eloqstore_paged_rows as rows


class PagedRowsTest(unittest.TestCase):
    def test_partition_matches_redis_hash_tag_rules(self):
        plain = b"paged:object"
        self.assertEqual(rows.partition_of(plain),
                         binascii.crc_hqx(plain, 0) % 1024)
        self.assertEqual(rows.partition_of(b"left{shared}right"),
                         binascii.crc_hqx(b"shared", 0) % 1024)
        for key in (b"empty{}tag", b"unterminated{tag"):
            self.assertEqual(rows.partition_of(key),
                             binascii.crc_hqx(key, 0) % 1024)

    def test_page_envelope_round_trip(self):
        key = b"binary\x00key\xff"
        encoded_key = (rows.page_prefix(key) + bytes([0]) +
                       (0xFEDCBA98).to_bytes(4, "big"))
        result = rows.decode_page_key(encoded_key)
        self.assertEqual(base64.b64decode(result["object_key_base64"]), key)
        self.assertEqual(result["kind"], "hash")
        self.assertEqual(result["page_id"], 0xFEDCBA98)

    def test_malformed_page_envelopes_are_rejected(self):
        good = rows.page_prefix(b"key") + b"\x00\x00\x00\x00\x07"
        bad = [b"", good[:-1], b"X" + good[1:],
               good[:8] + (99).to_bytes(4, "big") + good[12:],
               good[:-5] + b"\x02" + good[-4:]]
        for value in bad:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    rows.decode_page_key(value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
