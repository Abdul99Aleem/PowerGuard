# POWERGUARD  /  TEAM SCHEDULIX
# --------------------------------------------------------------------------
# test_manager_stall_analysis.py
# Original software (c) 2026 Abdul Aleem. MIT License.
# Submission changes (c) 2026 Team Schedulix. See LICENSE for terms.

import tempfile
import unittest
from pathlib import Path
from analyze_manager_stalls import analyze


class ConfirmationAudit(unittest.TestCase):
    def audit(self, rows, minimum=1):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stalls.txt"
            path.write_text("\n".join(rows))
            return analyze(path, minimum)

    def row(self, detected=1_045_000_000, received=1_052_000_000):
        return ("PASS manager_stopped=1 epoch=1 decision=4 stop_ns=1000000000 "
                f"progress_ns=995000000 detected_ns={detected} effective_ns=1050000000 "
                f"supervisor_received_ns={received} confirmation_ns={received-detected}")

    def test_receipt_while_stopped(self):
        self.assertTrue(self.audit([self.row()])["pass"])

    def test_moving_origin_cannot_hide_delay(self):
        self.assertFalse(self.audit([self.row(detected=1_050_000_000)])["pass"])

    def test_late_receipt_fails(self):
        self.assertFalse(self.audit([self.row(received=1_066_000_000)])["pass"])

    def test_replayed_decision_is_not_another_trial(self):
        self.assertFalse(self.audit([self.row(), self.row()], minimum=2)["pass"])


if __name__ == "__main__":
    unittest.main()
