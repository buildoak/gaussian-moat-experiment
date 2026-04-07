pub struct UnionFind {
    parent: Vec<u32>,
    child_count: Vec<u32>,
    rank: Vec<u8>,
    size: Vec<u64>,
    generation: Vec<u32>,
    free_list: Vec<u32>,
    current_gen: u32,
    live_count: usize,
}

impl UnionFind {
    pub fn new(capacity: usize) -> Self {
        Self {
            parent: Vec::with_capacity(capacity),
            child_count: Vec::with_capacity(capacity),
            rank: Vec::with_capacity(capacity),
            size: Vec::with_capacity(capacity),
            generation: Vec::with_capacity(capacity),
            free_list: Vec::new(),
            current_gen: 0,
            live_count: 0,
        }
    }

    /// Create a new set. Returns (slot_id, generation).
    pub fn make_set(&mut self) -> (u32, u32) {
        self.current_gen = self.current_gen.wrapping_add(1);
        if self.current_gen == 0 {
            self.current_gen = 1;
        }
        let gen = self.current_gen;

        if let Some(slot) = self.free_list.pop() {
            let idx = slot as usize;
            debug_assert_eq!(self.parent[idx], slot);
            debug_assert_eq!(self.child_count[idx], 0);
            self.parent[idx] = slot;
            self.rank[idx] = 0;
            self.size[idx] = 1;
            self.generation[idx] = gen;
            self.live_count += 1;
            (slot, gen)
        } else {
            let slot = self.parent.len() as u32;
            self.parent.push(slot);
            self.child_count.push(0);
            self.rank.push(0);
            self.size.push(1);
            self.generation.push(gen);
            self.live_count += 1;
            (slot, gen)
        }
    }

    /// Recycle a slot. The caller guarantees this slot will never be accessed again.
    pub fn recycle(&mut self, slot: u32) -> bool {
        debug_assert!((slot as usize) < self.parent.len());
        debug_assert!(self.live_count > 0);
        self.live_count = self.live_count.saturating_sub(1);

        let idx = slot as usize;
        if self.child_count[idx] != 0 {
            return false;
        }

        let parent = self.parent[idx];
        if parent != slot {
            let pidx = parent as usize;
            debug_assert!(self.child_count[pidx] > 0);
            self.child_count[pidx] = self.child_count[pidx].saturating_sub(1);
            self.parent[idx] = slot;
        }

        self.rank[idx] = 0;
        self.free_list.push(slot);
        true
    }

    #[inline]
    pub fn find(&mut self, mut x: u32) -> u32 {
        // Path halving
        while self.parent[x as usize] != x {
            let idx = x as usize;
            let parent = self.parent[idx];
            let grandparent = self.parent[parent as usize];
            if grandparent != parent {
                let pidx = parent as usize;
                let gpidx = grandparent as usize;
                debug_assert!(self.child_count[pidx] > 0);
                self.child_count[pidx] = self.child_count[pidx].saturating_sub(1);
                self.child_count[gpidx] = self.child_count[gpidx].saturating_add(1);
            }
            self.parent[idx] = grandparent;
            x = grandparent;
        }
        x
    }

    pub fn union(&mut self, x: u32, y: u32) -> u32 {
        let rx = self.find(x);
        let ry = self.find(y);
        if rx == ry {
            return rx;
        }

        let rxi = rx as usize;
        let ryi = ry as usize;

        if self.rank[rxi] < self.rank[ryi] {
            self.parent[rxi] = ry;
            self.child_count[ryi] = self.child_count[ryi].saturating_add(1);
            self.size[ryi] = self.size[ryi].saturating_add(self.size[rxi]);
            ry
        } else if self.rank[rxi] > self.rank[ryi] {
            self.parent[ryi] = rx;
            self.child_count[rxi] = self.child_count[rxi].saturating_add(1);
            self.size[rxi] = self.size[rxi].saturating_add(self.size[ryi]);
            rx
        } else {
            self.parent[ryi] = rx;
            self.child_count[rxi] = self.child_count[rxi].saturating_add(1);
            self.size[rxi] = self.size[rxi].saturating_add(self.size[ryi]);
            self.rank[rxi] += 1;
            rx
        }
    }

    #[inline]
    pub fn size(&mut self, x: u32) -> u64 {
        let root = self.find(x);
        self.size[root as usize]
    }

    #[inline]
    pub fn generation(&self, slot: u32) -> u32 {
        self.generation[slot as usize]
    }

    pub fn live_count(&self) -> usize {
        self.live_count
    }

    pub fn total_slots(&self) -> usize {
        self.parent.len()
    }

    pub fn is_empty(&self) -> bool {
        self.live_count == 0
    }
}

#[cfg(test)]
mod tests {
    use super::UnionFind;

    #[test]
    fn make_set_and_find() {
        let mut uf = UnionFind::new(4);
        let (a, ga) = uf.make_set();
        let (b, gb) = uf.make_set();
        assert_eq!(uf.find(a), a);
        assert_eq!(uf.find(b), b);
        assert_ne!(ga, gb);
        assert_eq!(uf.live_count(), 2);
    }

    #[test]
    fn union_and_size() {
        let mut uf = UnionFind::new(8);
        let (a, _) = uf.make_set();
        let (b, _) = uf.make_set();
        let (c, _) = uf.make_set();
        let (d, _) = uf.make_set();

        uf.union(a, b);
        assert_eq!(uf.size(a), 2);
        uf.union(c, d);
        assert_eq!(uf.size(c), 2);
        uf.union(a, c);
        assert_eq!(uf.size(d), 4);
        assert_eq!(uf.find(a), uf.find(d));
    }

    #[test]
    fn recycle_reuses_slots() {
        let mut uf = UnionFind::new(4);
        let (a, ga) = uf.make_set();
        let (_b, _) = uf.make_set();
        assert_eq!(uf.live_count(), 2);
        assert_eq!(uf.total_slots(), 2);

        assert!(uf.recycle(a));
        assert_eq!(uf.live_count(), 1);

        let (c, gc) = uf.make_set();
        assert_eq!(c, a); // slot reused
        assert_ne!(gc, ga); // generation changed
        assert_eq!(uf.live_count(), 2);
        assert_eq!(uf.total_slots(), 2); // no new allocation
    }

    #[test]
    fn path_halving() {
        let mut uf = UnionFind::new(8);
        for _ in 0..5 {
            uf.make_set();
        }
        uf.parent[1] = 0;
        uf.parent[2] = 1;
        uf.parent[3] = 2;
        uf.parent[4] = 3;
        uf.child_count[0] = 1;
        uf.child_count[1] = 1;
        uf.child_count[2] = 1;
        uf.child_count[3] = 1;

        let root = uf.find(4);
        assert_eq!(root, 0);
        assert_eq!(uf.parent[4], 2);
        assert_eq!(uf.parent[2], 0);
    }

    #[test]
    fn recycle_skips_reuse_when_slot_has_live_children() {
        let mut uf = UnionFind::new(8);
        let (a, _) = uf.make_set();
        let (b, _) = uf.make_set();
        let (c, _) = uf.make_set();

        let root = uf.union(a, b);
        let root = uf.union(root, c);
        assert_eq!(uf.live_count(), 3);

        assert!(!uf.recycle(root));
        assert_eq!(uf.live_count(), 2);

        let (next, _) = uf.make_set();
        assert_ne!(next, root);
    }
}
