mod org.houfeng;

type User {
  name: string;
  age: int;
}

@cdecl("libc");
extern func printf(msg: string);

main(args: string[]) {
  let user = User {
    name: "Houfeng",
    age: 18
  };
  print("Hello World: " + user.name);
} 