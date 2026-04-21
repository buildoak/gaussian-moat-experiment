use std::time::{Duration, Instant};

pub struct ProbeProfile {
    pub total_elapsed: Duration,
    pub phase_times: Vec<(String, Duration)>,
    pub peak_rss_kb: usize,
    pub total_primes_generated: usize,
    pub total_tiles_built: usize,
}

pub fn get_rss_kb() -> usize {
    unsafe {
        let mut usage: libc::rusage = std::mem::zeroed();
        if libc::getrusage(libc::RUSAGE_SELF, &mut usage) != 0 {
            return 0;
        }

        #[cfg(target_os = "macos")]
        {
            usage.ru_maxrss as usize / 1024
        }

        #[cfg(not(target_os = "macos"))]
        {
            usage.ru_maxrss as usize
        }
    }
}

pub struct PhaseTimer {
    current_name: Option<String>,
    current_start: Instant,
    phases: Vec<(String, Duration)>,
}

impl Default for PhaseTimer {
    fn default() -> Self {
        Self::new()
    }
}

impl PhaseTimer {
    pub fn new() -> Self {
        Self {
            current_name: None,
            current_start: Instant::now(),
            phases: Vec::new(),
        }
    }

    pub fn phase(&mut self, name: &str) {
        let now = Instant::now();
        if let Some(current_name) = self.current_name.replace(name.to_string()) {
            self.phases.push((current_name, now - self.current_start));
        }
        self.current_start = now;
    }

    pub fn finish(mut self) -> Vec<(String, Duration)> {
        if let Some(current_name) = self.current_name.take() {
            self.phases
                .push((current_name, Instant::now() - self.current_start));
        }
        self.phases
    }
}
