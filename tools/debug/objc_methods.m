// objc_methods Class...: list instance methods (Metal loaded)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
int main(int c, char **v) { @autoreleasepool { MTLCreateSystemDefaultDevice();
  for (int i = 1; i < c; ++i) { Class k = objc_getClass(v[i]); printf("== %s super %s\n", v[i], k ? class_getName(class_getSuperclass(k)) : "?");
    unsigned n = 0; Method *m = class_copyMethodList(k, &n);
    for (unsigned j = 0; j < n; ++j) { const char *s = sel_getName(method_getName(m[j])); if (strstr(s, "init") || strstr(s, "Struct") || strstr(s, "struct")) printf("  %s %s\n", s, method_getTypeEncoding(m[j])); }
    free(m); } } return 0; }
