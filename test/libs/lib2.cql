
@import(lib3);
@export(f3,f4,c1,c2);
@define f3() { printf("Running f3 from lib2\n"); }
@define f4() { printf("Running f4 from lib2\n"); }
c1 = 1;
c2 = 2;
printf("in lib2\n");
f2(); //from lib3
printf("finished loading lib2\n");
