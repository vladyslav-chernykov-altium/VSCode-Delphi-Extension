// C++ baseline fixture for FE.1.5 PDB-structure RE.
//
// Compiles with MSVC cl /Zi /Od to produce a reference PDB that
// cppvsdbg is known to handle correctly for `obj.method()` Watch
// expressions. We dump this PDB with llvm-pdbutil and compare
// against our rsm2pdb output to identify the exact CodeView shape
// VS expects.
//
// Build (from this directory, in a vcvars64 shell):
//   cl /Zi /Od /EHsc main.cpp /Fe:main.exe /link /DEBUG

class TAnimal {
public:
    int fAge;
    int GetAge() { return fAge; }
};

class TMammal : public TAnimal {
public:
    int fFurLen;
    int GetFurLen() { return fFurLen; }
};

class TDog : public TMammal {
public:
    int fBarkCount;
    int GetBarkCount() { return fBarkCount; }
    // FE.2 baseline: one-param + multi-param methods.
    void SetBarkCount(int v) { fBarkCount = v; }
    int  AddBarks(int a, int b) { return a + b; }
};

int main() {
    TDog d;
    d.fAge = 5;
    d.fFurLen = 10;
    d.fBarkCount = 42;

    // BP here -- try Watch `d.GetBarkCount()`, `d.SetBarkCount(99)`,
    // `d.AddBarks(2, 3)` in cppvsdbg.
    int x = d.GetBarkCount();
    d.SetBarkCount(99);
    int y = d.AddBarks(2, 3);
    return x + y;
}
