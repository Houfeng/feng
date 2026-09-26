/* Shared tuple construction must initialize each explicit field exactly once,
 * independent of the concrete representation selected by its descriptor. */
static void test_tuple_initializer_lifetime(void) {
    static const char source[] =
        "module tuple_lifetime;\n"
        "var created=0;var destroyed=0;\n"
        "func require(ok:bool):void{if !ok{throw 99;}}\n"
        "spec View{let n:int;}spec Callback():int;\n"
        "type Ref:View{let n:int;func ~Ref(){if self.n!=0{destroyed+=1;}}}\n"
        "@value type Value{let ref:Ref;}type Pair<T>(bool,T);\n"
        "type Cursor<T>{let value:T;func next():Pair<T>{return (true,self.value);}}\n"
        "func make(n:int):Ref{created+=1;return Ref{n:n};}\n"
        "func pair<T>(value:T):Pair<T>{return (true,value);}\n"
        "type Fixed(Ref,int);type Open<T>(T,int);type Nested(Pair<Ref>,int);\n"
        "func fail():int{throw 7;}\n"
        "func fixed():Fixed{return (make(8),fail());}\n"
        "func opened<T>(value:T):Open<T>{return (value,fail());}\n"
        "func nested():Nested{return ((true,make(9)),fail());}\n"
        "func borrowed():Fixed{let ref=make(10);return (ref,fail());}\n"
        "func handler():Fixed{let caught=try fail() catch e:int{return (make(11),e);};"
        "return (make(12),caught);}\n"
        "func exits():int{let unused:Fixed=(make(13),if true{return 21;}else{0;});return 0;}\n"
        "func exceptions():void{var caught=0;\n"
        "try fixed() catch e:int{require(e==7);caught+=1;}\n"
        "try opened<Ref>(make(14)) catch e:int{require(e==7);caught+=1;}\n"
        "try opened<Value>(Value{ref:make(15)}) catch e:int{require(e==7);caught+=1;}\n"
        "try opened<View>(make(16)) catch e:int{require(e==7);caught+=1;}\n"
        "try opened<string>(\"x\"+\"y\") catch e:int{require(e==7);caught+=1;}\n"
        "try opened<Ref[]>([make(17)]) catch e:int{require(e==7);caught+=1;}\n"
        "try nested() catch e:int{require(e==7);caught+=1;}\n"
        "try borrowed() catch e:int{require(e==7);caught+=1;}\n"
        "require(caught==8);require(exits()==21);let result=handler();require(result.item2==7);}\n"
        "func normal():void{let ref=make(3);\n"
        "let a=pair<Ref>(ref);require(a.item1 && a.item2.n==3);\n"
        "let b=pair<Value>(Value{ref:ref});require(b.item2.ref.n==3);\n"
        "let c=pair<View>(ref);require(c.item2.n==3);\n"
        "let fn:Callback=(){return ref.n;};let d=pair<Callback>(fn);require(d.item2()==3);\n"
        "let e=pair<Ref[]>([ref]);require(e.item2[0].n==3);\n"
        "let f=pair<string>(\"a\"+\"b\");require(f.item2==\"ab\");\n"
        "let g=pair<int>(3);require(g.item2==3);\n"
        "let h=pair<Pair<Ref>>(a);require(h.item2.item2.n==3);\n"
        "let cursor=Cursor<Ref>{value:ref};let next=cursor.next();require(next.item2.n==3);\n"
        "for var i=0;i<3;i+=1{let result=pair<Ref>(make(i+4));require(result.item2.n==i+4);}}\n"
        "func main(args:string[]):void{normal();require(created==destroyed);"
        "exceptions();require(created==destroyed);}\n";
    char workspace[] = "temp/tuple-lifetime-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *path = path_join(workspace, "main.ff");
    write_text_file(path, source);
    for (int release = 0; release < 2; ++release) {
        char *out = path_join(workspace, release ? "release" : "default");
        char *binary = path_join(out, "bin/tuple_lifetime");
        native_exception_build(path, out, "tuple_lifetime", "bin", NULL, release != 0);
        char *output = run_binary_capture_stdout_or_die(binary);
        ASSERT(strcmp(output, "") == 0);
        free(output);
        free(binary);
        free(out);
    }
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    free(path);
    puts("tuple initializers: shared function/type descriptors and managed field representations verified");
}
