/** Allocate and traverse real binary trees using the default GC. */
public final class Main {
    /** A reference node with two optional children. */
    private static final class Node {
        final Node left, right;

        /** Attach the children to this node. */
        Node(Node left, Node right) { this.left = left; this.right = right; }
    }

    /** Recursively construct all nodes, including leaves. */
    private static Node makeTree(int depth) {
        return depth == 0 ? new Node(null, null) : new Node(makeTree(depth - 1), makeTree(depth - 1));
    }

    /** Count nodes by following child references. */
    private static long check(Node node) {
        return node == null ? 0 : 1 + check(node.left) + check(node.right);
    }

    /** Construct and inspect one temporary tree. */
    private static long temporary(int depth) { return check(makeTree(depth)); }

    /** Execute stretch, temporary and long-lived tree workloads. */
    public static void main(String[] args) {
        if (args.length != 1) { System.exit(2); return; }
        int depth;
        try { depth = Integer.parseInt(args[0]); }
        catch (NumberFormatException error) { System.exit(2); return; }
        if (depth < 6 || depth > 20) { System.exit(2); return; }
        System.out.println(temporary(depth + 1));
        Node longLived = makeTree(depth);
        for (int d = 4; d <= depth; d += 2) {
            long iterations = 1L << (depth - d + 4), sum = 0;
            for (long i = 0; i < iterations; ++i) sum += temporary(d);
            System.out.println(d + " " + iterations + " " + sum);
        }
        System.out.println(check(longLived));
    }
}
