/// Phase 1 stub: no early termination. All wedges process all primes.
/// The progress signal will be implemented in Phase 2 with an
/// AtomicU64-based epoch protocol for early termination.
pub struct ProgressSignal;

impl ProgressSignal {
    pub fn new(_num_wedges: u32) -> Self {
        ProgressSignal
    }

    pub fn should_stop(&self) -> bool {
        false
    }
}
