use std::time::Duration;

pub struct ProbeProfile {
    pub total_elapsed: Duration,
    pub phase_times: Vec<(String, Duration)>,
    pub peak_rss_kb: usize,
    pub total_primes_generated: usize,
    pub total_tiles_built: usize,
}

pub fn get_rss_kb() -> usize {
    0
}

pub struct PhaseTimer;

impl PhaseTimer {
    pub fn new() -> Self {
        Self
    }

    pub fn phase(&mut self, _name: &str) {}

    pub fn finish(self) -> Vec<(String, Duration)> {
        Vec::new()
    }
}
