//! Campaign orchestrator: radial stripes → column chunks → composition → verdict

use crate::config::FatStripeConfig;
use moat_kernel::tile::TileOperator;

/// Result of a complete fat-stripe campaign
pub struct CampaignResult {
    /// true = moat proven (no inner-to-outer spanning)
    pub blocked: bool,
    pub num_stripes: usize,
    pub num_chunks_total: usize,
    pub total_tiles: u64,
    pub elapsed_ms: u64,
}

/// Run the full UB campaign over the annular strip [r_min, r_max]
pub fn run_campaign(config: &FatStripeConfig, r_min: f64, r_max: f64) -> CampaignResult {
    todo!("Wave 2-3: implement campaign loop")
}
