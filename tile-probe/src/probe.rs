pub struct ProbeConfig {
    pub k_sq: u64,
    pub r_min: f64,
    pub r_max: f64,
    pub strip_width: f64,
    pub num_strips: usize,
    pub tile_depth: f64,
    pub trace: bool,
}

pub struct ProbeResult {
    pub moat_found: bool,
    pub moat_radius: Option<f64>,
    pub shells_processed: usize,
    pub total_primes: usize,
    pub total_tiles: usize,
}

pub struct ShellProfile {
    pub shell_idx: usize,
    pub r_center: f64,
    pub primes_in_shell: usize,
    pub tiles_built: usize,
    pub compose_time_ms: u64,
    pub origin_reaches_outer: bool,
}
