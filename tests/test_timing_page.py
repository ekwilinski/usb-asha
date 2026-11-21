import unittest

from capture_timing import HEADER, PACKET, page_header


def page(first, end, count):
    return HEADER.pack(first, end, 0) + bytes(PACKET.size * count)


class TimingPageTests(unittest.TestCase):
    def test_stale_usb_reply_retries_same_cursor(self):
        with self.assertRaises(ValueError):
            page_header(page(21530, 22554, 32), 22538)
        self.assertEqual(page_header(page(22538, 22555, 17), 22538), (22538, 22555, 0, 0))

    def test_overwritten_history_is_a_visible_gap(self):
        self.assertEqual(page_header(page(14932, 15956, 32), 9508), (14932, 15956, 0, 5424))

    def test_counter_wrap(self):
        first = 2**32 - 2
        self.assertEqual(page_header(page(first, 1, 3), first), (first, 1, 0, 0))

    def test_short_transfer_is_rejected(self):
        with self.assertRaises(ValueError):
            page_header(page(65, 70, 3), 65)
