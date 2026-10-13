using System;
using System.Globalization;

/// <summary>Count prefix reversals across every permutation.</summary>
static class Program
{
    /// <summary>Enumerate permutations in prefix-rotation order and count actual reversals.</summary>
    static (long, int) Run(int n)
    {
        int[] permutation = new int[n], scratch = new int[n], count = new int[n];
        for (int i = 0; i < n; ++i) permutation[i] = i;
        int r = n, maximum = 0;
        long checksum = 0, ordinal = 0;
        while (true)
        {
            while (r > 1) { count[r - 1] = r; --r; }
            Array.Copy(permutation, scratch, n);
            int flips = 0;
            while (scratch[0] != 0)
            {
                for (int left = 0, right = scratch[0]; left < right; ++left, --right)
                {
                    int value = scratch[left]; scratch[left] = scratch[right]; scratch[right] = value;
                }
                ++flips;
            }
            if (flips > maximum) maximum = flips;
            checksum += ordinal++ % 2 == 0 ? flips : -flips;
            while (true)
            {
                if (r == n) return (checksum, maximum);
                int first = permutation[0];
                for (int i = 0; i < r; ++i) permutation[i] = permutation[i + 1];
                permutation[r] = first; --count[r];
                if (count[r] > 0) break;
                ++r;
            }
        }
    }

    /// <summary>Validate the size and emit the checksum and maximum flip count.</summary>
    static int Main(string[] args)
    {
        if (args.Length != 1 || !int.TryParse(args[0], NumberStyles.None, CultureInfo.InvariantCulture, out int n)
            || n < 1 || n > 11) return 2;
        var (checksum, maximum) = Run(n);
        Console.WriteLine(checksum + " " + maximum);
        return 0;
    }
}
