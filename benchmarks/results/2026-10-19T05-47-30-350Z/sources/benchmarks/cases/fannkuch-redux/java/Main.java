/** Count prefix reversals across every permutation. */
public final class Main {
    /** Enumerate permutations in prefix-rotation order and count actual reversals. */
    private static long[] run(int n) {
        int[] permutation = new int[n], scratch = new int[n], count = new int[n];
        for (int i = 0; i < n; ++i) permutation[i] = i;
        int r = n, maximum = 0;
        long checksum = 0, ordinal = 0;
        while (true) {
            while (r > 1) { count[r - 1] = r; --r; }
            System.arraycopy(permutation, 0, scratch, 0, n);
            int flips = 0;
            while (scratch[0] != 0) {
                for (int left = 0, right = scratch[0]; left < right; ++left, --right) {
                    int value = scratch[left]; scratch[left] = scratch[right]; scratch[right] = value;
                }
                ++flips;
            }
            if (flips > maximum) maximum = flips;
            checksum += ordinal++ % 2 == 0 ? flips : -flips;
            while (true) {
                if (r == n) return new long[] {checksum, maximum};
                int first = permutation[0];
                for (int i = 0; i < r; ++i) permutation[i] = permutation[i + 1];
                permutation[r] = first; --count[r];
                if (count[r] > 0) break;
                ++r;
            }
        }
    }

    /** Validate the size and emit the checksum and maximum flip count. */
    public static void main(String[] args) {
        if (args.length != 1) { System.exit(2); return; }
        int n;
        try { n = Integer.parseInt(args[0]); }
        catch (NumberFormatException error) { System.exit(2); return; }
        if (n < 1 || n > 11) { System.exit(2); return; }
        long[] result = run(n);
        System.out.println(result[0] + " " + result[1]);
    }
}
