/* Owned binary operands must survive evaluation and be released on both
 * normal and exceptional exits, independent of representation or producer. */
static void test_binary_operand_lifetime(void) {
    static const char source[] =
        "module binary_lifetime;\n"
        "var made=0;var dropped=0;var calls=0;\n"
        "func require(ok:bool):void{if !ok{throw 99;}}\n"
        "spec View{let n:int;}\n"
        "type Ref:View{let n:int;func ~Ref(){if self.n!=0{dropped+=1;}}}\n"
        "@value type Value{let text:string;let ref:Ref;}\n"
        "type Pair(string,Ref);spec Action():int;\n"
        "func make(n:int):Ref{made+=1;return Ref{n:n};}\n"
        "func identity<T>(value:T):T{return value;}\n"
        "func text():string{calls+=1;return \"a\"+\"b\";}\n"
        "func view():View{return make(1);}\n"
        "func value():Value{return Value{text:text(),ref:make(1)};}\n"
        "func pair():Pair{return (text(),make(1));}\n"
        "func array():Ref[]{return [make(1)];}\n"
        "func action():Action{let ref=make(1);return (){return ref.n;};}\n"
        "func failText():string{throw 41;}\n"
        "func failRef():Ref{throw 42;}\n"
        "func failView():View{throw 43;}\n"
        "func strings():void{\n"
        "require(text()+\"c\"==\"abc\");require(\"x\"+text()==\"xab\");\n"
        "require(text()+text()==\"abab\");require((text()+\"c\")+text()==\"abcab\");\n"
        "require(text()==text());require(text()!=\"z\");require(\"z\"!=text());\n"
        "require(identity<string>(text())==identity<string>(text()));\n"
        "let chosen=if true{text()+text();}else{\"unused\";};require(chosen==\"abab\");\n"
        "let matched=match 1{1{text()+text();}else{\"unused\";}};require(matched==chosen);\n"
        "let caught=try text()+failText() catch ex:int{require(ex==41);\"caught\";};\n"
        "require(caught==\"caught\");\n"
        "}\n"
        "func references():void{\n"
        "require(make(1)!=make(1));require(!(make(2)==make(2)));\n"
        "require(identity<Ref>(make(3))!=identity<Ref>(make(3)));\n"
        "require(array()!=array());require(!(array()==array()));\n"
        "require(action()!=action());require(!(action()==action()));\n"
        "require(view()!=view());require(!(view()==view()));\n"
        "require(value()!=value());require(!(value()==value()));\n"
        "require(pair()!=pair());require(!(pair()==pair()));\n"
        "}\n"
        "func borrowed():void{let ref=make(1);let wrapped=Value{text:text(),ref:ref};\n"
        "require(identity<Ref>(ref)==ref);require(ref==identity<Ref>(ref));\n"
        "require(identity<Value>(wrapped)==wrapped);require(wrapped==identity<Value>(wrapped));\n"
        "let viewed:View=ref;require(identity<View>(viewed)==viewed);require(viewed==identity<View>(viewed));}\n"
        "func exceptions():void{var caught=0;\n"
        "try make(1)==failRef() catch ex:int{require(ex==42);caught+=1;}\n"
        "try view()==failView() catch ex:int{require(ex==43);caught+=1;}\n"
        "try (text()+text())+failText() catch ex:int{require(ex==41);caught+=1;}\n"
        "require(caught==3);}\n"
        "func loops():void{calls=0;var i=0;\n"
        "while i<4 && text()==\"ab\"{i+=1;if i==2{continue;}}\n"
        "require(i==4 && calls==4);\n"
        "for var j=0;j<3 && text()==\"ab\";j+=1{if j==2{break;}}\n"
        "require(calls==7);require(false && text()==\"ab\" || true);require(calls==7);\n"
        "require(true || text()==\"ab\");require(calls==7);\n"
        "require(true && text()==\"ab\");require(calls==8);\n"
        "require(false || text()==\"ab\");require(calls==9);}\n"
        "func main(args:string[]):void{strings();references();require(made==dropped);\n"
        "borrowed();require(made==dropped);exceptions();require(made==dropped);loops();}\n";
    char workspace[] = "temp/binary-lifetime-XXXXXX";
    ASSERT(mkdtemp(workspace) != NULL);
    char *path = path_join(workspace, "main.ff");
    write_text_file(path, source);
    for (int release = 0; release < 2; ++release) {
        char *out = path_join(workspace, release ? "release" : "default");
        char *binary = path_join(out, "bin/binary_lifetime");
        native_exception_build(path, out, "binary_lifetime", "bin", NULL, release != 0);
        char *text = run_binary_capture_stdout_or_die(binary);
        ASSERT(strcmp(text, "") == 0);
        free(text);
        free(binary);
        free(out);
    }
    char *error = NULL;
    ASSERT(feng_cli_project_remove_tree(workspace, &error));
    free(error);
    free(path);
    puts("binary operands: managed/aggregate temporaries, generics, exceptions and lazy loops verified");
}
