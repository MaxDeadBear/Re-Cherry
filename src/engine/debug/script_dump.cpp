#include "engine/debug/script_dump.h"

#include <bit>
#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/ue3_objects.h"

REXCVAR_DEFINE_BOOL(cherry_debug_dump_script_classes, false, "Lollipop/Debug", "Write every ASH* script class and struct with its fields and live instances to script_classes.txt, their bytecode to script_bytecode.txt and the object and name tables beside them, 600 engine ticks after start or on the next tick when turned on later (tools/ue3script/disasm.py reads them).");
REXCVAR_DEFINE_STRING(cherry_debug_dump_script_values, "", "Lollipop/Debug", "Comma-separated script class names; every property value of their live instances is written to script_values.txt with the class dump. Empty is off.");

namespace cherry::engine::debug {

namespace {

constexpr uint64_t kFirstDumpTick = 600;
constexpr uint32_t kMaxArrayElements = 64;
constexpr int kMaxDepth = 3;

std::string TypeName(uint32_t object) {
  return ue3::ObjectName(ue3::ObjectClass(object));
}

bool StartsWithAsh(std::string_view name) {
  return name.size() >= 3 && (name[0] == 'A' || name[0] == 'a') && (name[1] == 'S' || name[1] == 's') && (name[2] == 'H' || name[2] == 'h');
}

std::string Describe(const ue3::Property& p) {
  const std::string type = TypeName(p.object);
  std::string text = std::format("+0x{:04X} {} {}", p.offset, type, ue3::ObjectName(p.object));
  if (p.array_dim != 1) text += std::format("[{}]", p.array_dim);
  text += std::format(" size={}", p.element_size);
  if (type == "BoolProperty") text += std::format(" mask=0x{:X}", p.extra);
  else if (type == "ArrayProperty" && p.extra) text += std::format(" inner={} {}", TypeName(p.extra), ue3::ObjectName(ue3::ReadProperty(p.extra).extra));
  else if (p.extra && (type == "StructProperty" || type == "ByteProperty" || type == "ObjectProperty" || type == "ClassProperty" || type == "ComponentProperty")) text += " " + ue3::ObjectName(p.extra);
  return text;
}

void DumpStruct(std::ofstream& out, uint32_t structure) {
  const uint32_t super = ue3::SuperStruct(structure);
  out << std::format("{} {} : {} (0x{:08X})\n", ue3::ObjectName(ue3::ObjectClass(structure)), ue3::ObjectPath(structure), super ? ue3::ObjectName(super) : "-", structure);
  for (uint32_t field = ue3::FirstChild(structure); field; field = ue3::NextField(field)) {
    if (ue3::ClassCastFlags(ue3::ObjectClass(field)) & ue3::kCastProperty) out << "  " << Describe(ue3::ReadProperty(field)) << "\n";
    else out << std::format("  {} {}\n", TypeName(field), ue3::ObjectName(field));
  }
}

std::string FormatValue(uint32_t address, const ue3::Property& p, int depth);

std::string FormatStruct(uint32_t address, uint32_t structure, int depth) {
  if (depth >= kMaxDepth) return "{...}";
  std::string text = "{";
  for (uint32_t s = structure; s; s = ue3::SuperStruct(s))
    for (uint32_t field = ue3::FirstChild(s); field; field = ue3::NextField(field)) {
      if (!(ue3::ClassCastFlags(ue3::ObjectClass(field)) & ue3::kCastProperty)) continue;
      const ue3::Property member = ue3::ReadProperty(field);
      text += std::format(" {}=", ue3::ObjectName(field));
      for (uint32_t i = 0; i < member.array_dim; ++i) text += (i ? "," : "") + FormatValue(address + member.offset + i * member.element_size, member, depth + 1);
    }
  return text + " }";
}

std::string FormatValue(uint32_t address, const ue3::Property& p, int depth) {
  const std::string type = TypeName(p.object);
  if (type == "BoolProperty") return (ue3::GetU32(address) & p.extra) ? "true" : "false";
  if (type == "ByteProperty") return std::to_string(ue3::GetU8(address));
  if (type == "IntProperty") return std::to_string(int32_t(ue3::GetU32(address)));
  if (type == "FloatProperty") return std::format("{}", std::bit_cast<float>(ue3::GetU32(address)));
  if (type == "NameProperty") return ue3::NameString(ue3::GetU32(address), ue3::GetU32(address + 4));
  if (type == "ObjectProperty" || type == "ClassProperty" || type == "ComponentProperty") {
    const uint32_t object = ue3::GetU32(address);
    return object ? ue3::ObjectPath(object) : "None";
  }
  if (type == "StructProperty") return FormatStruct(address, p.extra, depth);
  if (type == "ArrayProperty") {
    const uint32_t data = ue3::GetU32(address);
    const uint32_t count = ue3::GetU32(address + 4);
    std::string text = std::format("[{}]", count);
    if (!p.extra || !data || depth >= kMaxDepth) return text;
    const ue3::Property inner = ue3::ReadProperty(p.extra);
    text += " (";
    for (uint32_t i = 0; i < count && i < kMaxArrayElements; ++i) text += (i ? ", " : "") + FormatValue(data + i * inner.element_size, inner, depth + 1);
    if (count > kMaxArrayElements) text += ", ...";
    return text + ")";
  }
  return "?";
}

void DumpValues(std::ofstream& out, uint32_t object) {
  out << std::format("{} {} (0x{:08X})\n", ue3::ObjectName(ue3::ObjectClass(object)), ue3::ObjectPath(object), object);
  for (uint32_t s = ue3::ObjectClass(object); s; s = ue3::SuperStruct(s))
    for (uint32_t field = ue3::FirstChild(s); field; field = ue3::NextField(field)) {
      if (!(ue3::ClassCastFlags(ue3::ObjectClass(field)) & ue3::kCastProperty)) continue;
      const ue3::Property p = ue3::ReadProperty(field);
      for (uint32_t i = 0; i < p.array_dim; ++i) {
        const std::string index = p.array_dim > 1 ? std::format("[{}]", i) : "";
        out << std::format("  {}.{}{} = {}\n", ue3::ObjectName(s), ue3::ObjectName(field), index, FormatValue(object + p.offset + i * p.element_size, p, 0));
      }
    }
}

void DumpFunctions(std::ofstream& out, uint32_t structure) {
  for (uint32_t field = ue3::FirstChild(structure); field; field = ue3::NextField(field)) {
    const std::string kind = TypeName(field);
    if (kind == "State") DumpFunctions(out, field);
    if (kind != "Function") continue;
    out << ue3::ObjectPath(field) << " ";
    for (uint8_t byte : ue3::Script(field)) out << std::format("{:02X}", byte);
    out << '\n';
  }
}

void DumpTables(const std::vector<uint32_t>& classes) {
  std::ofstream bytecode("script_bytecode.txt");
  for (uint32_t cls : classes) DumpFunctions(bytecode, cls);
  std::ofstream objects("script_objects.txt");
  for (uint32_t i = 0, count = ue3::ObjectCount(); i < count; ++i)
    if (const uint32_t object = ue3::ObjectAt(i)) objects << std::format("{:08X} {} {}", object, TypeName(object), ue3::ObjectPath(object)) << '\n';
  std::ofstream names("script_names.txt");
  for (uint32_t i = 0, count = ue3::NameCount(); i < count; ++i) names << i << ' ' << ue3::NameString(i, 0) << '\n';
}

void DumpClasses() {
  std::ofstream out("script_classes.txt");
  std::vector<uint32_t> classes;
  const uint32_t count = ue3::ObjectCount();
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t object = ue3::ObjectAt(i);
    if (!object) continue;
    const std::string kind = ue3::ObjectName(ue3::ObjectClass(object));
    if (kind == "Class" && StartsWithAsh(ue3::ObjectName(object))) classes.push_back(object);
    else if (kind == "ScriptStruct" && (StartsWithAsh(ue3::ObjectName(object)) || StartsWithAsh(ue3::ObjectName(ue3::ObjectOuter(object))))) DumpStruct(out, object);
  }
  for (uint32_t cls : classes) DumpStruct(out, cls);
  out << "\n# live instances\n";
  for (uint32_t cls : classes) {
    const std::vector<uint32_t> instances = ue3::Instances(cls);
    size_t own = 0;
    for (uint32_t object : instances) own += ue3::ObjectClass(object) == cls;
    if (!own) continue;
    out << std::format("{} x{}\n", ue3::ObjectName(cls), own);
    for (uint32_t object : instances)
      if (ue3::ObjectClass(object) == cls) out << std::format("  {} (0x{:08X})\n", ue3::ObjectPath(object), object);
  }
  DumpTables(classes);
  REXLOG_INFO("[script_dump] {} objects, {} ASH classes written to script_classes.txt", count, classes.size());
}

void DumpRequestedValues() {
  const std::string list = REXCVAR_GET(cherry_debug_dump_script_values);
  if (list.empty()) return;
  std::ofstream out("script_values.txt");
  for (size_t start = 0; start < list.size();) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    const std::string name = list.substr(start, end - start);
    start = end + 1;
    const uint32_t cls = ue3::FindClass(name);
    if (!cls) {
      out << "# class " << name << " is not loaded\n";
      continue;
    }
    for (uint32_t object : ue3::Instances(cls)) DumpValues(out, object);
  }
}

}  // namespace

std::string FormatScriptValue(uint32_t address, uint32_t property) {
  return FormatValue(address, ue3::ReadProperty(property), 0);
}

void TickScriptDump() {
  static uint64_t ticks = 0;
  static bool dumped = false;
  ++ticks;
  if (!REXCVAR_GET(cherry_debug_dump_script_classes)) {
    dumped = false;
    return;
  }
  if (dumped || ticks < kFirstDumpTick) return;
  dumped = true;
  DumpClasses();
  DumpRequestedValues();
}

}  // namespace cherry::engine::debug
