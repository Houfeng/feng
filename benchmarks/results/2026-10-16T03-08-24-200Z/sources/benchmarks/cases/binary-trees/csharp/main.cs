using System;
using System.Globalization;

/// <summary>A reference node with two optional children.</summary>
sealed class Node
{
    public readonly Node? Left, Right;

    /// <summary>Attach the children to this node.</summary>
    public Node(Node? left, Node? right) { Left = left; Right = right; }
}

/// <summary>Allocate and traverse real binary trees using the default GC.</summary>
static class Program
{
    /// <summary>Recursively construct all nodes, including leaves.</summary>
    static Node MakeTree(int depth) => depth == 0
        ? new Node(null, null) : new Node(MakeTree(depth - 1), MakeTree(depth - 1));

    /// <summary>Count nodes by following child references.</summary>
    static long Check(Node? node) => node is null ? 0 : 1 + Check(node.Left) + Check(node.Right);

    /// <summary>Construct and inspect one temporary tree.</summary>
    static long Temporary(int depth) => Check(MakeTree(depth));

    /// <summary>Execute stretch, temporary and long-lived tree workloads.</summary>
    static int Main(string[] args)
    {
        if (args.Length != 1 || !int.TryParse(args[0], NumberStyles.None, CultureInfo.InvariantCulture, out int depth)
            || depth < 6 || depth > 20) return 2;
        Console.WriteLine(Temporary(depth + 1));
        var longLived = MakeTree(depth);
        for (int d = 4; d <= depth; d += 2)
        {
            long iterations = 1L << (depth - d + 4), sum = 0;
            for (long i = 0; i < iterations; ++i) sum += Temporary(d);
            Console.WriteLine(d + " " + iterations + " " + sum);
        }
        Console.WriteLine(Check(longLived));
        return 0;
    }
}
