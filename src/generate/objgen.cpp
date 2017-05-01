#include <set>
#include <cstring>
#include <fstream>
#include <elf.h>
#include "objgen.h"
#include "deferred.h"
#include "concretedeferred.h"
#include "instr/semantic.h"
#include "log/registry.h"
#include "log/log.h"
#include "util/streamasstring.h"

ObjGen::ObjGen(ElfSpace *elfSpace, MemoryBacking *backing, std::string filename) :
    elfSpace(elfSpace), backing(backing), filename(filename) {

    auto header = new Section(".elfheader");
    sectionList.addSection(header);

    auto strtab = new Section(".strtab", SHT_STRTAB);
    strtab->setContent(new DeferredStringList());
    sectionList.addSection(strtab);

    auto shstrtab = new Section(".shstrtab", SHT_STRTAB);
    shstrtab->setContent(new DeferredStringList());
    sectionList.addSection(shstrtab);
}

void ObjGen::generate() {
    LOG(1, "generating object file");
    makeHeader();
    makeSymbolInfo();
    makeText();
    makeRoData();
    makeShdrTable();
    updateSymbolTable();  // must run after .text & shdrTable are created
    updateOffsets();  // must run before updateShdrTable()
    serialize();
}

void ObjGen::makeHeader() {
    auto elfMap = elfSpace->getElfMap();

    // The first data in the elfMap contains the elf header.
    // Make a mutable copy for our own use.
    auto header = new ElfXX_Ehdr(
        *reinterpret_cast<ElfXX_Ehdr *>(elfMap->getMap()));
    auto deferred = new DeferredValueImpl<ElfXX_Ehdr>(header);

    header->e_type = ET_REL;  // object file
    header->e_entry = 0;
    header->e_phoff = 0;
    header->e_phentsize = 0;
    header->e_phnum = 0;

    deferred->addFunction([this] (ElfXX_Ehdr *header) {
        auto shdrTableSection = sectionList[".shdr_table"];
        auto shdrTable = shdrTableSection->castAs<ShdrTableContent *>();
        header->e_shoff = shdrTableSection->getOffset();
        header->e_shnum = shdrTable->getCount();
    });
    deferred->addFunction([this] (ElfXX_Ehdr *header) {
        header->e_shstrndx = sectionList.indexOf(".shstrtab");
    });

    sectionList[".elfheader"]->setContent(deferred);
}

void ObjGen::makeText() {
    // Split separate pages into their own LOAD sections.
    // First, find the set of all pages that are used.
    std::set<address_t> pagesUsed;
    for(auto func : CIter::functions(elfSpace->getModule())) {
        address_t start = func->getAddress() & ~0xfff;
        address_t end = ((func->getAddress() + func->getSize()) + 0xfff) & ~0xfff;
        for(address_t page = start; page < end; page += 0x1000) {
            LOG(1, "code uses page " << std::hex << page);
            pagesUsed.insert(page);
        }
    }

    // Next, map any contiguous pages as single sections.
    std::set<address_t>::iterator i = pagesUsed.begin();
    size_t totalSize = 0;
    while(i != pagesUsed.end()) {
        size_t size = 0;
        std::set<address_t>::iterator j = i;
        while(j != pagesUsed.end() && (*j) == (*i) + size) {
            j++;
            size += 0x1000;
        }

        address_t address = *i;

        LOG(1, "map " << std::hex << address << " size " << size);

        std::string name = StreamAsString()
            << ".text.0x" << std::hex << address;
        auto textSection = new Section(name.c_str(), SHT_PROGBITS,
            SHF_ALLOC | SHF_EXECINSTR);
        auto textValue = new DeferredString(reinterpret_cast<const char *>(*i), size);
        textSection->setContent(textValue);
        sectionList.addSection(textSection);

        makeRelocInfo(name);
        makeSymbolsAndRelocs(address, size, name);

        totalSize += size;
        i = j;
    }
}

void ObjGen::makeSymbolInfo() {
    auto symtab = new SymbolTableContent();
    auto symtabSection = new Section(".symtab", SHT_SYMTAB);
    symtabSection->setContent(symtab);

    auto strtab = sectionList[".strtab"]->castAs<DeferredStringList *>();

    {  // add null symbol
        auto symbol = new ElfXX_Sym();
        symbol->st_name = strtab->add("", 1);  // add empty name
        symbol->st_info = 0;
        symbol->st_other = STV_DEFAULT;
        symbol->st_shndx = 0;
        symbol->st_value = 0;
        symbol->st_size = 0;
        symtab->add(symbol);
    }

    // other symbols will be added later

    symtabSection->getHeader()->setSectionLink(
        new SectionRef(&sectionList, ".strtab"));
    sectionList.addSection(symtabSection);
}

void ObjGen::makeRelocInfo(const std::string &textSection) {
    auto reloc = new RelocSectionContent(
        new SectionRef(&sectionList, textSection));
    auto relocSection = new Section(".rela" + textSection, SHT_RELA, SHF_INFO_LINK);
    relocSection->setContent(reloc);

    sectionList.addSection(relocSection);
}

void ObjGen::makeSymbolsAndRelocs(address_t begin, size_t size,
    const std::string &textSection) {

    // Add symbols to the symbol list, but only for those functions
    // which fall into the given range [begin, begin+size).

    for(auto func : CIter::functions(elfSpace->getModule())) {
        if(blacklistedSymbol(func->getName())) {
            continue;  // skip making a symbol for this function
        }

        if(func->getAddress() < begin
            || func->getAddress() + func->getSize() >= begin + size) {
            continue;  // not in this text section
        }

        // fix addresses for objgen (set base to 0)
        func->getPosition()->set(func->getAddress() - backing->getBase());

        LOG(1, "making symbol for " << func->getName());
        makeSymbolInText(func, textSection);
        makeRelocInText(func, textSection);

        // undo address fix
        func->getPosition()->set(backing->getBase() + func->getAddress());
    }
}

void ObjGen::makeSymbolInText(Function *func, const std::string &textSection) {
    auto strtab = sectionList[".strtab"]->castAs<DeferredStringList *>();
    auto symtab = sectionList[".symtab"]->castAs<SymbolTableContent *>();

    // add name to string table
    auto index = strtab->add(func->getName(), true);
    auto value = symtab->add(func, func->getSymbol(), index);
    value->addFunction([this, textSection] (ElfXX_Sym *symbol) {
        symbol->st_shndx = sectionList.indexOf(textSection);
    });

    for(auto alias : func->getSymbol()->getAliases()) {
        // add name to string table
        auto name = std::string(alias->getName());
        auto index = strtab->add(name, true);
        auto value = symtab->add(func, alias, index);
        value->addFunction([this, textSection] (ElfXX_Sym *symbol) {
            symbol->st_shndx = sectionList.indexOf(textSection);
        });
    }

    for(auto block : CIter::children(func)) {
        for(auto instr : CIter::children(block)) {
            if(auto link = instr->getSemantic()->getLink()) {
                if(auto sol = dynamic_cast<PLTLink *>(link)) {
                    auto sym = sol->getPLTTrampoline()->getTargetSymbol();
                    auto name = std::string(sym->getName());
                    auto index = strtab->add(name, true);
                    auto value = symtab->add(nullptr, sym, index);
                    LOG(1, "got undefined symbol with name " << name);
                    value->addFunction([this, textSection] (ElfXX_Sym *symbol) {
                        symbol->st_shndx = SHN_UNDEF;
                    });
                }
            }
        }
    }
}

void ObjGen::makeRelocInText(Function *func, const std::string &textSection) {
    auto symtab = sectionList[".symtab"]->castAs<SymbolTableContent *>();
    auto reloc = sectionList[".rela" + textSection]->castAs<RelocSectionContent *>();

    auto rodata = elfSpace->getElfMap()->findSection(".rodata")->getHeader();
    auto roDataOffset = rodata->sh_offset;
    for(auto block : CIter::children(func)) {
        for(auto instr : CIter::children(block)) {
            if(auto link = instr->getSemantic()->getLink()) {
                LOG(1, "adding relocation at " << instr->getName());
                auto deferred = reloc->add(instr, link);
                if(deferred == nullptr)
                    continue;

                if(dynamic_cast<DataOffsetLink *>(link)) {
                    deferred->addFunction([this, symtab] (ElfXX_Rela *rela) {
                        size_t index = symtab->indexOfSectionSymbol(".rodata", &sectionList);
                        rela->r_info = ELFXX_R_INFO(index, R_X86_64_PC32);
                    });
                    deferred->addFunction([this, roDataOffset] (ElfXX_Rela *rela) {
                        // This affects the order in which things are called and breaks
                        // for other relocations
                        rela->r_addend -= roDataOffset;
                    });
                }
                else if(auto link2 = dynamic_cast<PLTLink *>(link)) {
                    deferred->addFunction([this, symtab, link2] (ElfXX_Rela *rela) {
                        auto elfSym = symtab->find(
                            link2->getPLTTrampoline()->getTargetSymbol());
                        size_t index = symtab->indexOf(elfSym);
                        LOG(1, "looks like we're using index " << index);
                        LOG(1, "...which is for symbol " << symtab->getKey(elfSym)->getName());
                        rela->r_info = ELFXX_R_INFO(index, R_X86_64_PLT32);
                    });
                }
            }
        }
    }
}

void ObjGen::makeRoData() {
    auto elfMap = elfSpace->getElfMap();
    auto oldRoDataShdr = elfMap->findSection(".rodata")->getHeader();
    auto roDataSection = new Section(".rodata", SHT_PROGBITS, SHF_ALLOC);
    const char *address = elfMap->getCharmap() + oldRoDataShdr->sh_offset;
    auto content = new DeferredString(address, oldRoDataShdr->sh_size);
    roDataSection->setContent(content);
    sectionList.addSection(roDataSection);
}

void ObjGen::makeShdrTable() {
    LOG(1, "generating shdr");
    auto shdrTable = new ShdrTableContent();
    auto shdrTableSection = new Section(".shdr_table", shdrTable);

    auto shstrtab = sectionList[".shstrtab"]->castAs<DeferredStringList *>();

    auto nullSection = new Section("", static_cast<ElfXX_Word>(SHT_NULL));
    auto nullDeferred = shdrTable->add(nullSection);
    nullDeferred->getElfPtr()->sh_name
        = shstrtab->add(nullSection->getName(), true);

    for(auto section : sectionList) {
        if(section->hasHeader()) {
            auto deferred = shdrTable->add(section);
            deferred->getElfPtr()->sh_name
                = shstrtab->add(section->getName(), true);

            if(dynamic_cast<SymbolTableContent *>(section->getContent())) {
                deferred->addFunction([this, shdrTable] (ElfXX_Shdr *shdr) {
                    //shdr->sh_info = shdrTable->getCount();
                    shdr->sh_info = sectionSymbolCount + 1;
                    shdr->sh_entsize = sizeof(ElfXX_Sym);
                    shdr->sh_addralign = 8;
                });
            }
            else if(auto v = dynamic_cast<RelocSectionContent *>(section->getContent())) {
                deferred->addFunction([this, v] (ElfXX_Shdr *shdr) {
                    shdr->sh_info = sectionList.indexOf(v->getTargetSection());
                    shdr->sh_addralign = 8;
                    shdr->sh_entsize = sizeof(ElfXX_Rela);
                    shdr->sh_link = sectionList.indexOf(".symtab");
                });
            }
        }
    }

    sectionList.addSection(shdrTableSection);
}

void ObjGen::updateSymbolTable() {
    // update section indices in symbol table
    auto shdrTable = sectionList[".shdr_table"]->castAs<ShdrTableContent *>();
    auto symtab = sectionList[".symtab"]->castAs<SymbolTableContent *>();

    // add section symbols
    int index = 1;  // skip the NULL symbol at index 0
    for(auto shdr : *shdrTable) {
        auto section = shdrTable->getKey(shdr);
        if(!section->getHeader()) continue;
        if(section->getHeader()->getShdrType() == SHT_NULL) continue;

        auto symbol = new Symbol(0, 0, "",
            Symbol::typeFromElfToInternal(STT_SECTION),
            Symbol::bindFromElfToInternal(STB_LOCAL),
            0, sectionList.indexOf(section));
        symtab->add(symbol, index ++);
    }
    symtab->recalculateIndices();
    sectionSymbolCount = index - 1;
}

void ObjGen::updateOffsets() {
    // every section is written to the file, even those without Headers
    size_t offset = 0;
    for(auto section : sectionList) {
        LOG(1, "section [" << section->getName() << "] is at offset " << std::dec << offset);
        section->setOffset(offset);
        offset += section->getContent()->getSize();
    }
}

void ObjGen::serialize() {
    std::ofstream fs(filename, std::ios::out | std::ios::binary);
    for(auto section : sectionList) {
        LOG(1, "serializing " << section->getName()
            << " @ " << std::hex << section->getOffset()
            << " of size " << std::dec << section->getContent()->getSize());
        fs << *section;
    }
    fs.close();
}

bool ObjGen::blacklistedSymbol(const std::string &name) {
    static bool initialized = false;
    static std::set<std::string> blacklist;

    if(!initialized) {
        // Remove these symbols which are only in executables and will be added
        // back in by the linker.
        blacklist.insert("_init");
        blacklist.insert("_fini");
        blacklist.insert("register_tm_clones");
        blacklist.insert("deregister_tm_clones");
        blacklist.insert("frame_dummy");
        blacklist.insert("__do_global_dtors_aux");
        blacklist.insert("__libc_csu_init");
        blacklist.insert("__libc_csu_fini");
        blacklist.insert("_start");
        initialized = true;
    }

    return blacklist.find(name) != blacklist.end();
}