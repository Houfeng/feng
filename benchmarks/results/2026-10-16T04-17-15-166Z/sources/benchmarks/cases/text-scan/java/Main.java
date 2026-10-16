import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** Read and validate fixed-format ASCII log records. */
public final class Main {
    /** Read the complete file and emit four 64-bit aggregates. */
    private static int scan(String[] args) throws IOException {
        if (args.length != 1) return 2;
        byte[] data = Files.readAllBytes(Path.of(args[0]));
        if (data.length % 8 != 0) return 2;
        long lines = 0, warnings = 0, errors = 0, sum = 0;
        for (int i = 0; i < data.length; i += 8) {
            byte level = data[i];
            if ((level != 73 && level != 87 && level != 69) || data[i + 1] != 44 || data[i + 7] != 10) return 2;
            long value = 0;
            for (int j = i + 2; j < i + 7; ++j) {
                byte digit = data[j];
                if (digit < 48 || digit > 57) return 2;
                value = value * 10 + digit - 48;
            }
            ++lines;
            if (level == 87) ++warnings;
            if (level == 69) ++errors;
            sum += value;
        }
        System.out.println(lines + " " + warnings + " " + errors + " " + sum);
        return 0;
    }

    /** Preserve malformed-input failure as a nonzero process exit. */
    public static void main(String[] args) throws IOException {
        int status = scan(args);
        if (status != 0) System.exit(status);
    }
}
