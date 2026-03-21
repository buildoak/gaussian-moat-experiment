use std::io::Write;

use serde::Serialize;

use crate::orchestrator::{IseConfig, IseSummary, ShellRecord, StripeRecord};

/// Flattened tile record for JSON output (avoids nested TileResult fields).
#[derive(Serialize)]
struct JsonTileRecord {
    a_lo: i64,
    b_lo: i64,
    width: u32,
    height: u32,
    io_count: usize,
    il_count: usize,
    ir_count: usize,
    ol_count: usize,
    or_count: usize,
    lr_count: usize,
    num_primes: usize,
    connects_below: Option<bool>,
    time_ms: u64,
}

/// Full JSON trace structure.
#[derive(Serialize)]
struct JsonTrace<'a> {
    config: &'a IseConfig,
    shells: &'a [ShellRecord],
    stripes: Vec<JsonStripeRecord<'a>>,
    summary: &'a IseSummary,
}

#[derive(Serialize)]
struct JsonStripeRecord<'a> {
    stripe_id: usize,
    b_lo: i64,
    tiles: Vec<JsonTileRecord>,
    #[serde(skip)]
    _phantom: std::marker::PhantomData<&'a ()>,
}

/// Write full JSON trace to file.
pub fn write_json_trace(
    path: &str,
    config: &IseConfig,
    shells: &[ShellRecord],
    stripes: &[StripeRecord],
    summary: &IseSummary,
) -> std::io::Result<()> {
    let json_stripes: Vec<JsonStripeRecord> = stripes
        .iter()
        .map(|s| JsonStripeRecord {
            stripe_id: s.stripe_id,
            b_lo: s.b_lo,
            tiles: s
                .tiles
                .iter()
                .map(|t| JsonTileRecord {
                    a_lo: t.a_lo,
                    b_lo: t.b_lo,
                    width: t.width,
                    height: t.height,
                    io_count: t.result.io_count,
                    il_count: t.result.il_count,
                    ir_count: t.result.ir_count,
                    ol_count: t.result.ol_count,
                    or_count: t.result.or_count,
                    lr_count: t.result.lr_count,
                    num_primes: t.result.num_primes,
                    connects_below: t.connects_below,
                    time_ms: t.time_ms,
                })
                .collect(),
            _phantom: std::marker::PhantomData,
        })
        .collect();

    let trace = JsonTrace {
        config,
        shells,
        stripes: json_stripes,
        summary,
    };

    let json = serde_json::to_string_pretty(&trace)?;
    let mut file = std::fs::File::create(path)?;
    file.write_all(json.as_bytes())?;
    Ok(())
}

/// Write CSV summary (one row per shell) to file.
pub fn write_csv_summary(path: &str, shells: &[ShellRecord]) -> std::io::Result<()> {
    let mut file = std::fs::File::create(path)?;
    writeln!(
        file,
        "shell_idx,r_center,a_lo,a_hi,f_r,is_candidate,num_primes,shell_time_ms"
    )?;
    for s in shells {
        writeln!(
            file,
            "{},{:.1},{},{},{:.6},{},{},{}",
            s.shell_idx,
            s.r_center,
            s.a_lo,
            s.a_hi,
            s.f_r,
            s.is_candidate,
            s.num_primes,
            s.shell_time_ms
        )?;
    }
    Ok(())
}

/// Print human-readable summary to stdout.
pub fn print_summary(config: &IseConfig, _shells: &[ShellRecord], summary: &IseSummary) {
    println!(
        "ISE k^2={} r=[{:.0}, {:.0}] tiles={}x{} stripes={}",
        config.k_sq, config.r_min, config.r_max, config.tile_width, config.tile_height,
        config.num_stripes
    );
    println!(
        "  {} shells, {} tiles, {:.2}s, {:.1} MB RSS",
        summary.total_shells,
        summary.total_tiles,
        summary.total_time_ms as f64 / 1000.0,
        summary.peak_rss_mb
    );

    if summary.candidates.is_empty() {
        println!("  Result: no moat candidates detected");
    } else {
        println!(
            "  Result: {} CANDIDATE(S) detected",
            summary.candidates.len()
        );
        for &(idx, r_center) in &summary.candidates {
            println!("    shell {} at R ~ {:.1}", idx, r_center);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::orchestrator::{run_ise, IseConfig};

    /// Gate 6: JSON round-trip validation
    #[test]
    fn json_round_trip() {
        let config = IseConfig {
            k_sq: 2,
            r_min: 0.0,
            r_max: 20.0,
            tile_width: 4,
            tile_height: 4,
            num_stripes: 4,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);

        // Write JSON trace to tmpfile
        let tmpdir = std::env::temp_dir();
        let json_path = tmpdir.join("ise_test_trace.json");
        let json_path_str = json_path.to_str().unwrap();

        write_json_trace(
            json_path_str,
            &result.config,
            &result.shells,
            &result.stripes,
            &result.summary,
        )
        .expect("Failed to write JSON trace");

        // Parse JSON back
        let json_str = std::fs::read_to_string(&json_path).expect("Failed to read JSON");
        let parsed: serde_json::Value =
            serde_json::from_str(&json_str).expect("Failed to parse JSON");

        // Verify structure
        assert!(parsed["config"].is_object(), "config should be an object");
        assert!(parsed["shells"].is_array(), "shells should be an array");
        assert!(parsed["stripes"].is_array(), "stripes should be an array");
        assert!(parsed["summary"].is_object(), "summary should be an object");

        // Shell count matches
        let shell_count = parsed["shells"].as_array().unwrap().len();
        assert_eq!(
            shell_count,
            result.shells.len(),
            "Shell count mismatch"
        );

        // io_counts arrays have correct length
        for shell in parsed["shells"].as_array().unwrap() {
            let io_counts = shell["io_counts"].as_array().unwrap();
            assert_eq!(
                io_counts.len(),
                config.num_stripes,
                "io_counts should have {} elements",
                config.num_stripes
            );
        }

        // f_r values in [0.0, 1.0]
        for shell in parsed["shells"].as_array().unwrap() {
            let f_r = shell["f_r"].as_f64().unwrap();
            assert!(
                (0.0..=1.0).contains(&f_r),
                "f_r should be in [0, 1], got {}",
                f_r
            );
        }

        // Stripe count matches
        let stripe_count = parsed["stripes"].as_array().unwrap().len();
        assert_eq!(
            stripe_count,
            config.num_stripes,
            "Stripe count mismatch"
        );

        // Tile dimensions in stripe records match config
        for stripe in parsed["stripes"].as_array().unwrap() {
            for tile in stripe["tiles"].as_array().unwrap() {
                assert_eq!(
                    tile["width"].as_u64().unwrap(),
                    config.tile_width as u64
                );
                assert_eq!(
                    tile["height"].as_u64().unwrap(),
                    config.tile_height as u64
                );
            }
        }

        // Cleanup
        let _ = std::fs::remove_file(&json_path);
    }

    /// Gate 6: CSV field check
    #[test]
    fn csv_round_trip() {
        let config = IseConfig {
            k_sq: 2,
            r_min: 0.0,
            r_max: 20.0,
            tile_width: 4,
            tile_height: 4,
            num_stripes: 4,
            fallback_heights: vec![],
            trace: false,
        };

        let result = run_ise(&config);

        let tmpdir = std::env::temp_dir();
        let csv_path = tmpdir.join("ise_test_summary.csv");
        let csv_path_str = csv_path.to_str().unwrap();

        write_csv_summary(csv_path_str, &result.shells).expect("Failed to write CSV");

        let csv_str = std::fs::read_to_string(&csv_path).expect("Failed to read CSV");
        let lines: Vec<&str> = csv_str.lines().collect();

        // Header + data rows
        assert_eq!(
            lines.len(),
            result.shells.len() + 1,
            "CSV should have header + {} data rows",
            result.shells.len()
        );

        // Check header
        assert_eq!(
            lines[0],
            "shell_idx,r_center,a_lo,a_hi,f_r,is_candidate,num_primes,shell_time_ms"
        );

        // Check each data row has 8 fields
        for (i, line) in lines[1..].iter().enumerate() {
            let fields: Vec<&str> = line.split(',').collect();
            assert_eq!(
                fields.len(),
                8,
                "Row {} should have 8 fields, got {}",
                i,
                fields.len()
            );
        }

        // Cleanup
        let _ = std::fs::remove_file(&csv_path);
    }
}
