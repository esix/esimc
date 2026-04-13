#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include "ast.h"
#include "codegen.h"

extern FILE* yyin;
extern int yyparse();
extern Program* programRoot;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: esimc <source.sim> [-o output.ll]\n";
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = "output.ll";

    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
            outputFile = argv[++i];
        }
    }

    yyin = fopen(inputFile.c_str(), "r");
    if (!yyin) {
        std::cerr << "Error: cannot open '" << inputFile << "'\n";
        return 1;
    }

    if (yyparse() != 0) {
        std::cerr << "Parsing failed.\n";
        fclose(yyin);
        return 1;
    }
    fclose(yyin);

    if (!programRoot) {
        std::cerr << "Error: no program parsed.\n";
        return 1;
    }

    CodeGenContext context;
    context.generateCode(*programRoot);
    context.writeIR(outputFile);

    std::cout << "Wrote LLVM IR to " << outputFile << "\n";

    delete programRoot;
    return 0;
}
