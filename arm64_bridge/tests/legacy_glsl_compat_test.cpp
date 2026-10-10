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
    const std::string header = "#version 320 es\nprecision highp float;\n"
        "#define ENABLE_UNIFORM_BUFFER_OBJECTS 0\n#define ENABLE_MULTIDRAW_INDIRECT 0\n";
    const std::string vertex = header + "out gl_PerVertex {\nvec4 gl_Position;\n};\n"
        "layout(location=0) in vec4 _Un_AttrPosition0;\n"
        "layout(location=0) out vec4 UnTexcoord0;\n"
        "layout(location=3) out vec4 UnTexcoord3;\n"
        "void main(){gl_Position=_Un_AttrPosition0;UnTexcoord0=vec4(1);UnTexcoord3=vec4(2);}\n";
    const std::string fragment = header + "out vec4 FragmentColor;\n"
        "layout(location=0) in vec4 UnTexcoord0;\n"
        "void main(){FragmentColor=UnTexcoord0;}\n";
    unsigned interfaceCases = 0;
    const auto check = [&](bool pass, const char* name) {
        ++interfaceCases;
        if (!pass) { std::fprintf(stderr, "SSO interface %s failed\n", name); ++failures; }
    };
    std::string paddedVertex, paddedFragment, again;
    check(legacy_glsl::CompleteSsoInterface(vertex, true, &paddedVertex), "original VS");
    check(legacy_glsl::CompleteSsoInterface(fragment, false, &paddedFragment), "original FS");
    check(paddedVertex.ends_with(vertex.substr(vertex.find("void main("))) &&
          paddedFragment.ends_with(fragment.substr(fragment.find("void main("))), "body byte identity");
    check(paddedVertex.find("layout(location=0) in vec4 _Un_AttrPosition0;") != std::string::npos,
          "vertex attribute untouched");
    check(paddedFragment.find("UnTexcoord3") == paddedFragment.rfind("UnTexcoord3") &&
          paddedVertex.find("UnTexcoord12") == paddedVertex.rfind("UnTexcoord12"), "new declarations unread");
    check(legacy_glsl::CompleteSsoInterface(paddedVertex, true, &again) && again == paddedVertex,
          "VS idempotence");
    check(legacy_glsl::CompleteSsoInterface(paddedFragment, false, &again) && again == paddedFragment,
          "FS idempotence");
    for (const unsigned location : {0u,1u,2u,3u,4u,5u,6u,7u,12u,13u}) {
        const std::string suffix = " vec4 UnTexcoord" + std::to_string(location) + ";\n";
        check(paddedVertex.find(") out" + suffix) != std::string::npos &&
              paddedFragment.find(") in" + suffix) != std::string::npos, "matching located ABI");
    }
    const std::string invalid[] = {
        "#version 300 es\nprecision highp float;\nvoid main(){}\n",
        header + "layout(location=14) in vec4 UnTexcoord14;\nvoid main(){}\n",
        header + "layout(location=0) in vec3 UnTexcoord0;\nvoid main(){}\n",
        header + "layout(location=0) in vec4 UnTexcoord0[2];\nvoid main(){}\n",
        header + "layout(location=0) flat in vec4 UnTexcoord0;\nvoid main(){}\n",
        header + "layout(location=0) in vec4 UnTexcoord1;\nvoid main(){}\n",
        header + "layout(location=0) in vec4 UnTexcoord0;\nlayout(location=0) in vec4 UnTexcoord0;\nvoid main(){}\n",
        header + "#if 1\nlayout(location=0) in vec4 UnTexcoord0;\n#endif\nvoid main(){}\n",
        header + "#define ENABLE_UNIFORM_BUFFER_OBJECTS 1\nvoid main(){}\n",
        header + "#define ENABLE_MULTIDRAW_INDIRECT 1\nvoid main(){}\n",
        header + "precision mediump float;\nvoid main(){}\n",
        header + "#if 0\nvoid main(){}\n",
        header + "#endif\nvoid main(){}\n",
        header + "layout(location=99999999999999) in vec4 UnTexcoord0;\nvoid main(){}\n",
        header + "void main(){}\nvoid main(){}\n"
    };
    for (const auto& source : invalid) {
        again = "retained";
        check(!legacy_glsl::CompleteSsoInterface(source, false, &again) && again == "retained",
              "unsupported interface uses real mono fallback");
    }
    check(!legacy_glsl::CompleteSsoInterface(fragment, false, nullptr), "null output");
    std::printf("legacy_glsl_compat: %zu cases, %u failures\n",
                sizeof(tests) / sizeof(tests[0]) + interfaceCases, failures);
    return failures ? 1 : 0;
}
