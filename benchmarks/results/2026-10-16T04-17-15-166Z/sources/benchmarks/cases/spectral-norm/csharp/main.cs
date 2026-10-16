using System;
using System.Globalization;

/// <summary>Compute a spectral norm with implicit matrix-vector products.</summary>
static class Program
{
    /// <summary>Evaluate one matrix entry using zero-based indices.</summary>
    static double Entry(int i, int j)
    {
        int sum = i + j;
        return 1.0 / (sum * (sum + 1) / 2 + i + 1);
    }

    /// <summary>Multiply a vector by A, evaluating each entry inside the loop.</summary>
    static void MultiplyA(int n, double[] input, double[] output)
    {
        for (int i = 0; i < n; ++i)
        {
            double sum = 0;
            for (int j = 0; j < n; ++j) sum += Entry(i, j) * input[j];
            output[i] = sum;
        }
    }

    /// <summary>Multiply a vector by the transpose of A.</summary>
    static void MultiplyAt(int n, double[] input, double[] output)
    {
        for (int i = 0; i < n; ++i)
        {
            double sum = 0;
            for (int j = 0; j < n; ++j) sum += Entry(j, i) * input[j];
            output[i] = sum;
        }
    }

    /// <summary>Apply both products using the caller's reusable scratch array.</summary>
    static void MultiplyAtA(int n, double[] input, double[] output, double[] scratch)
    {
        MultiplyA(n, input, scratch);
        MultiplyAt(n, scratch, output);
    }

    /// <summary>Validate input, perform ten power iterations and emit the norm.</summary>
    static int Main(string[] args)
    {
        if (args.Length != 1 || !int.TryParse(args[0], NumberStyles.None, CultureInfo.InvariantCulture, out int n)
            || n < 1 || n > 10000) return 2;
        double[] u = new double[n], v = new double[n], scratch = new double[n];
        Array.Fill(u, 1.0);
        for (int iteration = 0; iteration < 10; ++iteration)
        {
            MultiplyAtA(n, u, v, scratch);
            MultiplyAtA(n, v, u, scratch);
        }
        double uv = 0, vv = 0;
        for (int i = 0; i < n; ++i) { uv += u[i] * v[i]; vv += v[i] * v[i]; }
        Console.WriteLine(Math.Sqrt(uv / vv).ToString("R", CultureInfo.InvariantCulture));
        return 0;
    }
}
