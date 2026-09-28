/** A node owns its two optional children. */
struct Node {
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

/** Construct a complete tree with separately owned nodes. */
fn make_tree(depth: u32) -> Box<Node> {
    if depth == 0 {
        Box::new(Node {
            left: None,
            right: None,
        })
    } else {
        Box::new(Node {
            left: Some(make_tree(depth - 1)),
            right: Some(make_tree(depth - 1)),
        })
    }
}

/** Count real nodes by following the tree's children. */
fn check(node: &Node) -> u64 {
    1 + node.left.as_deref().map_or(0, check) + node.right.as_deref().map_or(0, check)
}

/** Construct, inspect and release one temporary tree. */
fn temporary(depth: u32) -> u64 {
    let tree = make_tree(depth);
    check(&tree)
}

/** Execute the stretch, temporary and long-lived tree workloads. */
fn main() {
    let args: Vec<String> = std::env::args().collect();
    assert_eq!(args.len(), 2);
    let depth: u32 = args[1].parse().expect("invalid depth");
    assert!((6..=20).contains(&depth));
    println!("{}", temporary(depth + 1));
    let long_lived = make_tree(depth);
    for d in (4..=depth).step_by(2) {
        let iterations = 1u64 << (depth - d + 4);
        let mut sum = 0u64;
        for _ in 0..iterations {
            sum += temporary(d);
        }
        println!("{} {} {}", d, iterations, sum);
    }
    println!("{}", check(&long_lived));
}
