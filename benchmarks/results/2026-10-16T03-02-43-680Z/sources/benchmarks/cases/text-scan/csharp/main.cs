using System;
using System.IO;

/// <summary>Read and validate fixed-format ASCII log records.</summary>
static class Program
{
    /// <summary>Read the complete file and emit four 64-bit aggregates.</summary>
    static int Main(string[] args)
    {
        if (args.Length != 1) return 2;
        byte[] data = File.ReadAllBytes(args[0]);
        if (data.Length % 8 != 0) return 2;
        long lines = 0, warnings = 0, errors = 0, sum = 0;
        for (int i = 0; i < data.Length; i += 8)
        {
            byte level = data[i];
            if ((level != 73 && level != 87 && level != 69) || data[i + 1] != 44 || data[i + 7] != 10) return 2;
            long value = 0;
            for (int j = i + 2; j < i + 7; ++j)
            {
                byte digit = data[j];
                if (digit < 48 || digit > 57) return 2;
                value = value * 10 + digit - 48;
            }
            ++lines;
            if (level == 87) ++warnings;
            if (level == 69) ++errors;
            sum += value;
        }
        Console.WriteLine(lines + " " + warnings + " " + errors + " " + sum);
        return 0;
    }
}
