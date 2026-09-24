#include <cstdio>

/* A native C++ control case diagnoses toolchain failures without any protocol or plugin. */
struct Large {
  long Values[8];
};

/* Function sanitizer places its signature before this ordinary function's entry symbol. */
static Large produce(Large Value, int Fail) {
  if (Fail) throw 66;
  Value.Values[7] += Value.Values[0];
  return Value;
}

static Large (*volatile Callback)(Large, int) = produce;

/* Exit successfully only when the platform delivers the native exception to its catch. */
int main() {
  try {
    Large Value = {{3, 0, 0, 0, 0, 0, 0, 7}};
    Value = Callback(Value, 1);
    return static_cast<int>(Value.Values[7]);
  } catch (int Value) {
    std::printf("native catch %d\n", Value);
    return Value == 66 ? 0 : 1;
  }
}
