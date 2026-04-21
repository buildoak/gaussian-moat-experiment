pub mod bridge;
pub mod config;
pub mod driver;
pub mod orchestrator;
pub mod protocol;

pub use bridge::tile_operator_from_raw;
pub use config::CudaFatStripeConfig;
pub use driver::{CudaDriver, CudaError};
pub use orchestrator::{run_campaign, CampaignResult};
pub use protocol::{
    read_all_tiles, read_campaign_summary, read_job_manifest, read_stream_header,
    read_tile_result, write_job_manifest, CampaignSummary, FacePortRecord, FacePortStream,
    FacePortStreamHeader, FatStripeJobHeader, ProtocolError, RawTileResult,
    TileJob, TilePorts, TileResultHeader, STREAM_FLAG_CAMPAIGN_SUMMARY,
};
