#ifndef EGALITO_PASS_SHADOWSTACK_H
#define EGALITO_PASS_SHADOWSTACK_H

#include "chunkpass.h"
#include "instr/concrete.h"

class ShadowStack : public ChunkPass {
private:
    unsigned int offset;
    unsigned int size;
    int sentinel;
    Instruction *saveSSPInsn;
    Instruction *restoreSSPInsn;
    Instruction *clearSSPInsn;
public:
    ShadowStack(const char * stackStartAddress);
    virtual void visit(Function *function);
    virtual void visit(Block *block);
    virtual void visit(Instruction *instruction);
private:
    void addInstructions(Block *block, Instruction *instruction, bool isPrologue);
    std::vector<unsigned char> intToBytes(int value);
};

#endif
