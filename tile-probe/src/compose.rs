use crate::tile::TileOperator;

pub fn compose_horizontal(left: &TileOperator, _right: &TileOperator, _k_sq: u64) -> TileOperator {
    left.clone()
}

pub fn compose_vertical(bottom: &TileOperator, _top: &TileOperator, _k_sq: u64) -> TileOperator {
    bottom.clone()
}

pub fn compose_grid(grid: Vec<Vec<TileOperator>>, _k_sq: u64) -> TileOperator {
    grid.into_iter()
        .next()
        .and_then(|row| row.into_iter().next())
        .expect("grid must not be empty")
}
