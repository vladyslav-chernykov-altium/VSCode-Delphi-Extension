#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rsm2pdb::model {

enum class PrimitiveKind {
    Bool,
    Char,
    WChar,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float32, Float64, Float80,
};

using TypeId = std::uint32_t;
constexpr TypeId kNoType = 0;

struct PrimitiveType {
    PrimitiveKind kind;
};

struct PointerType {
    TypeId pointee;
};

struct ArrayType {
    TypeId element;
    std::uint64_t length;
};

struct RecordField {
    std::string name;
    TypeId type;
    std::uint64_t byte_offset;
};

struct RecordType {
    std::string name;
    std::uint64_t byte_size;
    std::vector<RecordField> fields;
};

struct EnumEntry {
    std::string name;
    std::int64_t value;
};

struct EnumType {
    std::string name;
    TypeId underlying;
    std::vector<EnumEntry> entries;
};

struct Type {
    std::variant<PrimitiveType, PointerType, ArrayType, RecordType, EnumType> kind;
};

struct LineEntry {
    std::uint64_t address;
    std::uint32_t file_id;
    std::uint32_t line;
};

enum class SymbolKind {
    Unknown,
    Function,    // code (e.g. .text segment)
    Variable     // data (e.g. .data / .bss / .tls segments)
};

// One formal parameter or local variable belonging to a function.
// `stack_offset` is byte-signed relative to the subprogram's frame base
// (DW_AT_frame_base). The DWARF emitter wraps it as DW_OP_fbreg.
struct LocalVar {
    std::string  name;
    TypeId       type = kNoType;
    std::int32_t stack_offset = 0;
};

struct Symbol {
    std::string name;
    std::uint64_t address;
    std::uint64_t size;
    TypeId type;
    SymbolKind kind = SymbolKind::Unknown;
    // Populated for kind == Function. Empty otherwise.
    std::vector<LocalVar> params;
    std::vector<LocalVar> locals;     // includes Result for functions
};

struct CompileUnit {
    std::string source_path;
    std::vector<Symbol> symbols;
    std::vector<LineEntry> lines;
};

class Module {
public:
    TypeId addType(Type t);
    const Type& getType(TypeId id) const;
    std::size_t typeCount() const { return types_.size(); }

    std::vector<CompileUnit> units;
    std::vector<std::string> source_files;

private:
    std::vector<Type> types_;
};

} // namespace rsm2pdb::model
