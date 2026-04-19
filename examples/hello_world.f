mod org.houfeng;

type User {
  let name: string;
  let age: int;
}

@cdecl("libc");
extern fn print(msg: string):void;

main(args: string[]) {
  let user = User {
    name: "Houfeng",
    age: 18
  };
  print("Hello World: " + user.name);
}