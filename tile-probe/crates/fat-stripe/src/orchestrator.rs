//! Campaign orchestrator: radial stripes -> column chunks -> composition -> verdict

use std::time::Instant;

use crate::chunk::process_chunk;
use crate::config::FatStripeConfig;
use moat_kernel::compose::{compose_horizontal, compose_vertical};
use moat_kernel::tile::TileOperator;

/// Result of a complete fat-stripe campaign
pub struct CampaignResult {
    /// true = moat proven (no inner-to-outer spanning component)
    pub blocked: bool,
    pub num_stripes: usize,
    pub num_chunks_total: usize,
    pub total_tiles: u64,
    pub elapsed_ms: u64,
}

/// Run the full UB campaign over the annular strip [r_min, r_max].
pub fn run_campaign(config: &FatStripeConfig, r_min: f64, r_max: f64) -> CampaignResult {
    let start = Instant::now();

    let tile_height = config.tile_height as i64;
    let tile_width = config.tile_width as i64;
    let chunk_tiles = config.chunk_size as i64;
    let k_sq = config.k_sq;

    let a_start = r_min.floor() as i64;
    let a_end = r_max.ceil() as i64;

    let mut num_stripes = 0_usize;
    let mut num_chunks_total = 0_usize;
    let mut total_tiles = 0_u64;
    let mut composed_full: Option<TileOperator> = None;

    let r_max_sq = r_max * r_max;

    let mut a_lo = a_start;
    while a_lo < a_end {
        let a_hi = (a_lo + tile_height).min(a_end);

        // First octant: b in [0, b_max] where b <= a AND a^2 + b^2 <= r_max^2.
        // The tightest constraint at each a is b <= sqrt(r_max^2 - a^2),
        // but a varies across the stripe.  Use a_lo (the smallest a in this
        // stripe) to get the most generous b_max, then clamp to a_hi for the
        // first-octant constraint b <= a.
        let b_from_radius = {
            let a_lo_f = a_lo as f64;
            let diff = r_max_sq - a_lo_f * a_lo_f;
            if diff > 0.0 { diff.sqrt().ceil() as i64 } else { 0 }
        };
        let b_max_stripe = b_from_radius.min(a_hi).min(config.b_max);

        let mut stripe_op: Option<TileOperator> = None;

        let mut b_lo = 0_i64;
        while b_lo < b_max_stripe {
            let b_chunk_hi = (b_lo + chunk_tiles * tile_width).min(b_max_stripe);

            let chunk_op = process_chunk(config, a_lo, a_hi, b_lo, b_chunk_hi);
            num_chunks_total += 1;

            let chunk_b_span = (b_chunk_hi - b_lo) as u64;
            total_tiles += (chunk_b_span + tile_width as u64 - 1) / tile_width as u64;

            stripe_op = Some(match stripe_op {
                None => chunk_op,
                Some(left) => compose_horizontal(&left, &chunk_op, k_sq),
            });

            b_lo = b_chunk_hi;
        }

        if let Some(stripe) = stripe_op {
            composed_full = Some(match composed_full {
                None => stripe,
                Some(bottom) => compose_vertical(&bottom, &stripe, k_sq),
            });
        }

        num_stripes += 1;

        eprintln!(
            "  stripe {}: a=[{}, {}), chunks_so_far={}, tiles_so_far={}",
            num_stripes, a_lo, a_hi, num_chunks_total, total_tiles
        );

        a_lo = a_hi;
    }

    let elapsed_ms = start.elapsed().as_millis() as u64;

    // Verdict: does any component span from R < r_min to R > r_max in the
    // circular sense?  We cannot rely on rectangular face bits because the
    // Cartesian tiling does not align with circular annulus boundaries.
    // Instead, inspect the actual (a, b) coordinates of ALL face ports and
    // group by component: a component is "spanning" if it has at least one
    // port with radius <= r_min + collar_f AND at least one port with
    // radius >= r_max - collar_f.
    let collar_f = (k_sq as f64).sqrt().ceil();
    let r_inner_thresh = r_min + collar_f;
    let r_outer_thresh = (r_max - collar_f).max(0.0);
    let r_inner_sq = r_inner_thresh * r_inner_thresh;
    let r_outer_sq = r_outer_thresh * r_outer_thresh;

    let blocked = if let Some(ref full_op) = composed_full {
        let nc = full_op.num_components;
        let mut has_inner = vec![false; nc];
        let mut has_outer = vec![false; nc];

        // Scan ALL face-port lists — ports carry exact (a, b) coordinates.
        let all_ports = full_op.face_inner.iter()
            .chain(full_op.face_outer.iter())
            .chain(full_op.face_left.iter())
            .chain(full_op.face_right.iter());

        for port in all_ports {
            let r_sq = port.a as f64 * port.a as f64 + port.b as f64 * port.b as f64;
            if port.component < nc {
                if r_sq <= r_inner_sq {
                    has_inner[port.component] = true;
                }
                if r_sq >= r_outer_sq {
                    has_outer[port.component] = true;
                }
            }
        }

        let spanning = (0..nc).any(|c| has_inner[c] && has_outer[c]);
        let spanning_count = (0..nc).filter(|&c| has_inner[c] && has_outer[c]).count();
        eprintln!(
            "  verdict: {} spanning component(s), thresholds r_in={:.1} r_out={:.1}",
            spanning_count, r_inner_thresh, r_outer_thresh,
        );

        // Blocked = no component spans inner-to-outer in the radial sense
        !spanning
    } else {
        true
    };

    eprintln!(
        "campaign complete: blocked={}, stripes={}, chunks={}, tiles={}, elapsed={}ms",
        blocked, num_stripes, num_chunks_total, total_tiles, elapsed_ms
    );

    CampaignResult {
        blocked,
        num_stripes,
        num_chunks_total,
        total_tiles,
        elapsed_ms,
    }
}
