#include "legacy_glsl_compat.h"
#include <cstdio>
#include <string>

int main() {
    struct Test { const char* input; const char* expected; };
    const Test tests[] = {
        {"Temporary1 = (textureProj( PSampler1, Temporary1.rgba ).rrrr );",
         "Temporary1 = (vec4(textureProj( PSampler1, Temporary1.rgba )).rrrr );"},
        {"textureProj(s, vec4(f(1.0), 2.0, 3.0, 1.0)).rrrr",
         "vec4(textureProj(s, vec4(f(1.0), 2.0, 3.0, 1.0))).rrrr"},
        {"textureProj (s,p) \n.rrrr + textureProj(s,q).rrrr",
         "vec4(textureProj (s,p)) \n.rrrr + vec4(textureProj(s,q)).rrrr"},
        {"float a=textureProj(s,p);", "float a=textureProj(s,p);"},
        {"textureProj(s,p).rgba", "textureProj(s,p).rgba"},
        {"Temporary0.rrrr", "Temporary0.rrrr"},
        {"mytextureProj(s,p).rrrr", "mytextureProj(s,p).rrrr"},
        {"textureProjOther(s,p).rrrr", "textureProjOther(s,p).rrrr"},
        {"textureProj(s,p).rrrrx", "textureProj(s,p).rrrrx"},
        {"// textureProj(s,p).rrrr\ntextureProj(s,p).rrrr",
         "// textureProj(s,p).rrrr\nvec4(textureProj(s,p)).rrrr"},
        {"/* textureProj(s,p).rrrr */", "/* textureProj(s,p).rrrr */"},
        {"textureProj(s, /* ) */ p).rrrr", "vec4(textureProj(s, /* ) */ p)).rrrr"},
        {"textureProj(s, p /*", "textureProj(s, p /*"},
        {"textureProj(s, p", "textureProj(s, p"},
        {"", ""},
    };
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        const std::string output = legacy_glsl::NormalizeProjectedSwizzles(tests[i].input);
        if (output != tests[i].expected) {
            std::fprintf(stderr, "case %u failed: %s\n", i, output.c_str());
            ++failures;
        }
    }
    std::printf("legacy_glsl_compat: %zu cases, %u failures\n",
                sizeof(tests) / sizeof(tests[0]), failures);
    return failures ? 1 : 0;
}
