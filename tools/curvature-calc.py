#!/usr/bin/env python3
"""
Curvature-compensated ISE parameter calculator.

Given R_target, max_angle, num_stripes, tile_size, k_sq:
- Computes stripe placements with curvature compensation
- Validates geometric constraints
- Outputs ISE command or parameter table

Usage:
  python3 curvature-calc.py --r-target 1000000000 --angle 10 --stripes 128
  python3 curvature-calc.py --r-target 500000000 --angle 10 --stripes 128 --tile-size 2000
"""

import argparse
import math
import json
import sys


def compute_placements(r_target: float, max_angle_deg: float, num_stripes: int,
                       tile_size: int = 2000, k_sq: int = 40) -> dict:
    """Compute curvature-compensated stripe placements."""
    collar = math.ceil(math.sqrt(k_sq))
    b_max = r_target * math.tan(math.radians(max_angle_deg))
    stride = b_max / num_stripes
    min_stride = tile_size + 2 * collar

    stripes = []
    for j in range(num_stripes):
        b_center = collar + j * stride + tile_size / 2
        b_lo = collar + j * stride

        # Curvature-compensated a_min
        arg = r_target ** 2 - b_center ** 2
        if arg < 0:
            # Beyond the circle — skip
            break
        a_min = int(math.floor(math.sqrt(arg)))
        a_max = a_min + tile_size

        # True distance from origin (inner face center)
        r_actual = math.sqrt(a_min ** 2 + b_center ** 2)

        # Angle from real axis
        angle_deg = math.degrees(math.atan2(b_center, a_min))

        # Radial alignment: cos(angle) = fraction of H that's radial
        radial_efficiency = math.cos(math.radians(angle_deg))

        stripes.append({
            'j': j,
            'b_lo': b_lo,
            'b_center': b_center,
            'a_min': a_min,
            'a_max': a_max,
            'r_actual': r_actual,
            'r_error': abs(r_actual - r_target),
            'angle_deg': angle_deg,
            'radial_efficiency': radial_efficiency,
        })

    # Curvature stats
    if stripes:
        max_delta_a = abs(stripes[0]['a_min'] - stripes[-1]['a_min'])
        max_angle = stripes[-1]['angle_deg']
        min_efficiency = stripes[-1]['radial_efficiency']
        max_r_error = max(s['r_error'] for s in stripes)
    else:
        max_delta_a = max_angle = min_efficiency = max_r_error = 0

    return {
        'params': {
            'r_target': r_target,
            'max_angle_deg': max_angle_deg,
            'num_stripes': len(stripes),
            'tile_size': tile_size,
            'k_sq': k_sq,
            'collar': collar,
            'b_max': b_max,
            'stride': stride,
            'min_stride_for_independence': min_stride,
            'independent': stride >= min_stride,
        },
        'geometry': {
            'max_delta_a': max_delta_a,
            'max_delta_a_in_tiles': max_delta_a / tile_size,
            'max_angle_actual': max_angle,
            'min_radial_efficiency': min_efficiency,
            'max_r_error': max_r_error,
            'lateral_coverage': stripes[-1]['b_center'] if stripes else 0,
            'angular_coverage_pct': max_angle / 90 * 100 if stripes else 0,
        },
        'stripes': stripes,
    }


def print_summary(result: dict):
    """Print human-readable summary."""
    p = result['params']
    g = result['geometry']

    print(f"{'='*60}")
    print(f"  Curvature-Compensated ISE Parameters")
    print(f"{'='*60}")
    print(f"  R target:     {p['r_target']:,.0f}")
    print(f"  Max angle:    {p['max_angle_deg']}°")
    print(f"  Stripes:      {p['num_stripes']}")
    print(f"  Tile size:    {p['tile_size']}×{p['tile_size']}")
    print(f"  k²:           {p['k_sq']} (collar={p['collar']})")
    print(f"  Stride:       {p['stride']:,.0f}")
    print(f"  Independent:  {'YES' if p['independent'] else 'NO (stride < W+2c)'}")
    print()
    print(f"  Geometry:")
    print(f"    Max Δa:              {g['max_delta_a']:,.0f} ({g['max_delta_a_in_tiles']:.1f} tiles)")
    print(f"    Max angle:           {g['max_angle_actual']:.2f}°")
    print(f"    Min radial eff:      {g['min_radial_efficiency']:.4f} ({g['min_radial_efficiency']*100:.1f}%)")
    print(f"    Max R error:         {g['max_r_error']:.1f} units")
    print(f"    Lateral coverage:    {g['lateral_coverage']:,.0f}")
    print(f"    Angular coverage:    {g['angular_coverage_pct']:.2f}% of first quadrant")
    print()

    # Sample stripes
    stripes = result['stripes']
    print(f"  Stripe placements (first 5, middle, last 5):")
    print(f"  {'j':>5} | {'b_center':>12} | {'a_min':>14} | {'angle':>7} | {'R error':>8} | {'eff':>6}")
    print(f"  {'-'*5}-+-{'-'*12}-+-{'-'*14}-+-{'-'*7}-+-{'-'*8}-+-{'-'*6}")

    show = list(range(min(5, len(stripes))))
    if len(stripes) > 10:
        show += [len(stripes) // 2]
        show += list(range(len(stripes) - 5, len(stripes)))
    else:
        show = list(range(len(stripes)))

    for i in show:
        s = stripes[i]
        print(f"  {s['j']:5d} | {s['b_center']:12,.0f} | {s['a_min']:14,d} | {s['angle_deg']:6.2f}° | {s['r_error']:7.1f} | {s['radial_efficiency']:.4f}")

    print()


def print_ise_command(result: dict, num_shells: int = 20):
    """Print the ISE command to run."""
    p = result['params']
    stride_int = max(int(p['stride']), int(p['min_stride_for_independence']))

    print(f"  ISE command (once --r-target is implemented):")
    print(f"  ./tile-probe/target/release/ise \\")
    print(f"    --k-squared {p['k_sq']} \\")
    print(f"    --r-target {int(p['r_target'])} \\")
    print(f"    --num-shells {num_shells} \\")
    print(f"    --tile-size {p['tile_size']} \\")
    print(f"    --stripes {p['num_stripes']} \\")
    print(f"    --stripe-stride {stride_int} \\")
    print(f"    --trace --profile")
    print()


def main():
    parser = argparse.ArgumentParser(description='Curvature-compensated ISE parameter calculator')
    parser.add_argument('--r-target', type=float, required=True, help='Target radius from origin')
    parser.add_argument('--angle', type=float, default=10.0, help='Max angle from real axis (degrees)')
    parser.add_argument('--stripes', type=int, default=128, help='Number of stripes')
    parser.add_argument('--tile-size', type=int, default=2000, help='Tile side length')
    parser.add_argument('--k-sq', type=int, default=40, help='Step bound k²')
    parser.add_argument('--num-shells', type=int, default=20, help='Shells per probe')
    parser.add_argument('--json', action='store_true', help='Output JSON instead of table')

    args = parser.parse_args()

    result = compute_placements(
        r_target=args.r_target,
        max_angle_deg=args.angle,
        num_stripes=args.stripes,
        tile_size=args.tile_size,
        k_sq=args.k_sq,
    )

    if args.json:
        # Compact: omit per-stripe details, keep params + geometry
        out = {k: v for k, v in result.items() if k != 'stripes'}
        out['stripes_sample'] = result['stripes'][:3] + result['stripes'][-3:]
        print(json.dumps(out, indent=2))
    else:
        print_summary(result)
        print_ise_command(result, args.num_shells)

        # Validation warnings
        g = result['geometry']
        if g['min_radial_efficiency'] < 0.95:
            print(f"  ⚠️  WARNING: Radial efficiency drops to {g['min_radial_efficiency']:.3f} at max angle.")
            print(f"     Consider reducing --angle to keep efficiency > 0.95.")
        if not result['params']['independent']:
            print(f"  ⚠️  WARNING: Stride {result['params']['stride']:.0f} < min {result['params']['min_stride_for_independence']}")
            print(f"     Stripes are NOT independent. Fine for UB, not for ISE statistics.")


if __name__ == '__main__':
    main()
