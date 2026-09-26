"""Source-integration regression checks; NOT a substitute for Windows native CI."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[2]
NATIVE = (ROOT/'src/features/recorder/InputInjector.cpp').read_text()
EXPORTS = (ROOT/'src/bridge/exports.cpp').read_text()
HARNESS = (ROOT/'poc/BotControllerPocHarness/PocHarnessPlugin.cs').read_text()

class Gate2COnlyContract(unittest.TestCase):
    def test_default_off(self):
        self.assertIn('bool gate2cOnly = false;', HARNESS)
        self.assertIn(': api.StartUsercmdMovement(slot, forwardMove: 1.0f, leftMove: 0.0f)', HARNESS)
    def test_exclusive_start(self):
        start = NATIVE.split('int64_t StartUsercmdMovementGate2COnly(')[1].split('namespace {')[0]
        self.assertIn('!g_usercmdMovements[slot].empty()', start)
        self.assertIn('g_gate2cOnlyOwners[slot].movementId', start)
        self.assertIn('std::scoped_lock lock(g_usercmdInjectionMutex)', start)
    def test_same_lock_covers_prc_mutation(self):
        body = NATIVE.split('bool ApplyUsercmdMovement(int slot,')[1].split('// Merges active injections')[0]
        self.assertLess(body.index('std::scoped_lock lock(g_usercmdInjectionMutex)'), body.index('base->set_forwardmove('))
        self.assertIn('g_gate2cOnlyOwners[slot].movementId', body)
    def test_same_intent_peek_and_lazy_expiry(self):
        body = NATIVE.split('bool PeekUsercmdMovement(int slot,')[1].split('// Replaces Bot AI analog')[0]
        self.assertIn('ExpireGate2COnlyLocked(slot)', body)
        self.assertIn('g_usercmdMovements[slot].back()', body)
    def test_owner_scoped_cancel(self):
        body = NATIVE.split('bool CancelUsercmdMovement(int slot,')[1].split('// Cancels one injection')[0]
        self.assertIn('g_gate2cOnlyOwners[slot].movementId == movementId', body)
    def test_cleanup(self):
        self.assertIn('g_gate2cOnlyOwners[slot] = {};', NATIVE)
        self.assertIn('g_gate2cOnlyOwners.fill({});', NATIVE)
    def test_export_and_harness_abi(self):
        self.assertIn('BotController_StartUsercmdMovementGate2COnly(int slot, float forwardMove, float leftMove, int maxDurationMs)', EXPORTS)
        self.assertIn('extern long BotController_StartUsercmdMovementGate2COnly(int slot, float forwardMove, float leftMove, int maxDurationMs)', HARNESS)
    def test_diagnostics_not_removed(self):
        self.assertIn('gate2b::RecordPlayerRunCommandObservation(', NATIVE)
        self.assertIn('gate2c::CommitFrame(', NATIVE)
        self.assertIn('gate2c::CommitResolvedSample(', NATIVE)

if __name__ == '__main__': unittest.main()
