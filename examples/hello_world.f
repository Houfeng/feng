mod org.houfeng;

type User {
  let name: string;
  let age: int;
  say(msg: string): void {
    print(msg);
  }
}

@cdecl("libc")
extern fn print(msg: string):void;

fn main(args: string[]) {
  let user = User {
    name: "Houfeng",
    age: 18
  };
  print("Hello World: " + user.name);
}