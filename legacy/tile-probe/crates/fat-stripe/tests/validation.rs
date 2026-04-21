use fat_stripe::config::FatStripeConfig;
use fat_stripe::orchestrator::run_campaign;

/// Test 1a: k^2=26, small region (R=10-50) -- composition sanity check.
/// At k^2=26 the step distance is ~5.1 lattice units, so near the origin
/// the Gaussian prime graph is well-connected.  Fat-stripe should report
/// NOT blocked (connected annulus).
#[test]
fn small_region_k26_connected() {
    let config = FatStripeConfig::new(26, 50, 10, 110_000, 50);
    let result = run_campaign(&config, 10.0, 50.0);
    assert!(
        !result.blocked,
        "k^2=26 at R=10-50 should be connected (not blocked), but got blocked=true"
    );
    assert!(result.num_stripes > 0);
    println!(
        "Test 1a passed: blocked={}, stripes={}, tiles={}, elapsed={}ms",
        result.blocked, result.num_stripes, result.total_tiles, result.elapsed_ms
    );
}

/// Test 1b: k^2=2, very near origin (R=0-10) -- connected at tiny step.
/// At k^2=2 the step distance is ~1.4.  The densest region near the origin
/// is connected.  Should report NOT blocked.
#[test]
fn near_origin_k2_connected() {
    let config = FatStripeConfig::new(2, 50, 10, 110_000, 50);
    let result = run_campaign(&config, 0.0, 10.0);
    assert!(
        !result.blocked,
        "k^2=2 at R=0-10 should be connected (not blocked), but got blocked=true"
    );
    println!(
        "Test 1b passed: blocked={}, stripes={}, tiles={}, elapsed={}ms",
        result.blocked, result.num_stripes, result.total_tiles, result.elapsed_ms
    );
}

/// Test 2: k^2=2, R=10-50 -- known blocked (moat at R~11.7).
/// At k^2=2, step distance ~1.41 is too small for primes to connect
/// across the annulus at R=10-50.  The annulus should be blocked
/// (no component spans from R~10 to R~50).
#[test]
fn k2_annulus_blocked() {
    let config = FatStripeConfig::new(2, 50, 10, 110_000, 50);
    let result = run_campaign(&config, 10.0, 50.0);
    assert!(
        result.blocked,
        "k^2=2 at R=10-50 should be blocked (moat exists at R~11.7), but got blocked=false"
    );
    println!(
        "Test 2 passed: blocked={}, stripes={}, tiles={}, elapsed={}ms",
        result.blocked, result.num_stripes, result.total_tiles, result.elapsed_ms
    );
}

/// Test 3: k^2=26, R=100-200 -- well-connected region.
/// At this moderate radius, k^2=26 provides plenty of connectivity.
/// Should report NOT blocked.
#[test]
fn k26_moderate_radius_connected() {
    let config = FatStripeConfig::new(26, 100, 10, 110_000, 200);
    let result = run_campaign(&config, 100.0, 200.0);
    assert!(
        !result.blocked,
        "k^2=26 at R=100-200 should be connected, but got blocked=true"
    );
    println!(
        "Test 3 passed: blocked={}, stripes={}, tiles={}, elapsed={}ms",
        result.blocked, result.num_stripes, result.total_tiles, result.elapsed_ms
    );
}

/// Test 4: k^2=26, narrow band across Tsuchimura moat at R~1,015,639.
/// The ISE tool detects a moat candidate at a single-tile level.  But
/// at the full-annulus level, the annulus can still be traversed by
/// components that go around the moat. This test validates that fat-stripe
/// correctly reports the full-annulus verdict: NOT blocked (the annulus
/// has traversing components even though the origin component is trapped).
/// This is the correct behavior -- fat-stripe checks annulus connectivity,
/// not origin-component reachability.
#[test]
#[ignore]
fn tsuchimura_annulus_k26() {
    let config = FatStripeConfig::new(26, 200, 50, 110_000, 1_016_500);
    let result = run_campaign(&config, 1_015_000.0, 1_016_500.0);
    // The annulus is NOT blocked: components span it even though the origin
    // component cannot cross R=1,015,639.  Fat-stripe's job is annulus
    // connectivity, not origin reachability.
    assert!(
        !result.blocked,
        "k^2=26 annulus at R~1,015,639 should NOT be blocked at annulus level"
    );
    println!(
        "Test 4 passed: blocked={}, stripes={}, tiles={}, elapsed={}ms",
        result.blocked, result.num_stripes, result.total_tiles, result.elapsed_ms
    );
}

/// Test 5: k^2=4, R=40-100 -- blocked annulus.
/// At k^2=4, step distance is 2.0.  The moat for k^2=4 is at R~45.3.
/// The annulus [40, 100] spans the moat region.  At this small step
/// distance, the prime graph fragments significantly, and no component
/// spans the full annulus.
#[test]
#[ignore]
fn k4_blocked_annulus() {
    let config = FatStripeConfig::new(4, 50, 10, 110_000, 100);
    let result = run_campaign(&config, 40.0, 100.0);
    assert!(
        result.blocked,
        "k^2=4 at R=40-100 should be blocked (moat at R~45.3), but got blocked=false"
    );
    println!(
        "Test 5 passed: blocked={}, stripes={}, tiles={}, elapsed={}ms",
        result.blocked, result.num_stripes, result.total_tiles, result.elapsed_ms
    );
}
