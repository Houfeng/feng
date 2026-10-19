import java.util.Arrays;

/** Compute a spectral norm with implicit matrix-vector products. */
public final class Main {
    /** Evaluate one matrix entry using zero-based indices. */
    private static double entry(int i, int j) {
        int sum = i + j;
        return 1.0 / (sum * (sum + 1) / 2 + i + 1);
    }

    /** Multiply a vector by A, evaluating each entry inside the loop. */
    private static void multiplyA(int n, double[] input, double[] output) {
        for (int i = 0; i < n; ++i) {
            double sum = 0;
            for (int j = 0; j < n; ++j) sum += entry(i, j) * input[j];
            output[i] = sum;
        }
    }

    /** Multiply a vector by the transpose of A. */
    private static void multiplyAt(int n, double[] input, double[] output) {
        for (int i = 0; i < n; ++i) {
            double sum = 0;
            for (int j = 0; j < n; ++j) sum += entry(j, i) * input[j];
            output[i] = sum;
        }
    }

    /** Apply both products using the caller's reusable scratch array. */
    private static void multiplyAtA(int n, double[] input, double[] output, double[] scratch) {
        multiplyA(n, input, scratch);
        multiplyAt(n, scratch, output);
    }

    /** Validate input, perform ten power iterations and emit the norm. */
    public static void main(String[] args) {
        if (args.length != 1) { System.exit(2); return; }
        int n;
        try { n = Integer.parseInt(args[0]); }
        catch (NumberFormatException error) { System.exit(2); return; }
        if (n < 1 || n > 10000) { System.exit(2); return; }
        double[] u = new double[n], v = new double[n], scratch = new double[n];
        Arrays.fill(u, 1.0);
        for (int iteration = 0; iteration < 10; ++iteration) {
            multiplyAtA(n, u, v, scratch);
            multiplyAtA(n, v, u, scratch);
        }
        double uv = 0, vv = 0;
        for (int i = 0; i < n; ++i) { uv += u[i] * v[i]; vv += v[i] * v[i]; }
        System.out.println(Math.sqrt(uv / vv));
    }
}
